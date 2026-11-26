// Preload for cardputer-pet-terminal bridge/server.mjs (loaded by bin/pet-os-bridge.sh):
// adds the PET-os push-to-talk endpoint without editing that project.
//
//   POST /api/pet/voice            body: a WAV (or any ffmpeg-readable audio), max 8 MB
//   Authorization: Bearer <PET_BRIDGE_TOKEN>   (same rule as the other /api routes)
//   -> {"ok":true,"transcript":"...","reply":"...","mood":"..."}
//   -> {"ok":false,"error":"..."}          (nothing heard / transcription failed)
//
// Transcription runs locally on the Mac with the openai-whisper CLI (the same
// large-v3-turbo model the AI Note Taker uses; nothing leaves the machine). The
// text is then posted to this same bridge's /api/pet/ask over loopback, so the
// reply comes from the same OpenClaw session (agent:main:pet) as typed chat.
//
// Env: PET_VOICE_WHISPER (default: whisper on PATH), PET_VOICE_MODEL
// (default large-v3-turbo), PET_VOICE_PROMPT (vocabulary hint).
import http from 'node:http';
import { spawn } from 'node:child_process';
import { mkdtemp, writeFile, readFile, rm } from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';

const MAX_BYTES = 8 * 1024 * 1024;
const WHISPER = process.env.PET_VOICE_WHISPER || 'whisper';
const MODEL = process.env.PET_VOICE_MODEL || 'large-v3-turbo';
const PROMPT = process.env.PET_VOICE_PROMPT ||
  'Alice, Holden, Raspberry Pi, grandworld-pi, grandworld-media, Jellyfin, Mox, Tailscale, PET, AdGuard, docker.';
const TIMEOUT_MS = Number(process.env.PET_VOICE_TIMEOUT_MS || 120000);

function send(res, status, data) {
  res.writeHead(status, { 'Content-Type': 'application/json; charset=utf-8', 'Cache-Control': 'no-store' });
  res.end(JSON.stringify(data));
}

function authorized(req) {
  const token = process.env.PET_BRIDGE_TOKEN || '';
  if (!token) return true;
  const url = new URL(req.url || '/', 'http://localhost');
  const header = req.headers.authorization || '';
  return (url.searchParams.get('token') || header.replace(/^Bearer\s+/i, '')) === token;
}

async function readBody(req) {
  const chunks = [];
  let size = 0;
  for await (const chunk of req) {
    size += chunk.length;
    if (size > MAX_BYTES) throw Object.assign(new Error('audio too large (8 MB max)'), { status: 413 });
    chunks.push(chunk);
  }
  return Buffer.concat(chunks);
}

function run(cmd, args) {
  return new Promise((resolve, reject) => {
    const child = spawn(cmd, args, { stdio: ['ignore', 'ignore', 'pipe'] });
    let stderr = '';
    const timer = setTimeout(() => child.kill('SIGKILL'), TIMEOUT_MS);
    child.stderr.on('data', d => { stderr = (stderr + d).slice(-2000); });
    child.on('error', err => { clearTimeout(timer); reject(err); });
    child.on('close', code => {
      clearTimeout(timer);
      code === 0 ? resolve() : reject(new Error(`${path.basename(cmd)} exited ${code}: ${stderr.trim().split('\n').pop() || ''}`));
    });
  });
}

export async function transcribe(audio) {
  const dir = await mkdtemp(path.join(os.tmpdir(), 'pet-os-voice-'));
  try {
    const input = path.join(dir, 'ptt.wav');
    await writeFile(input, audio);
    await run(WHISPER, [input, '--model', MODEL, '--language', 'en', '--task', 'transcribe',
      '--output_format', 'txt', '--output_dir', dir, '--fp16', 'False', '--initial_prompt', PROMPT,
      '--condition_on_previous_text', 'False', '--verbose', 'False']);
    const text = (await readFile(path.join(dir, 'ptt.txt'), 'utf8')).replace(/\s+/g, ' ').trim();
    // Whisper hallucinates these on silence.
    if (/^(you|thank you\.?|thanks for watching!?|\.+)$/i.test(text)) return '';
    return text;
  } finally {
    await rm(dir, { recursive: true, force: true });
  }
}

// Same-process loopback call to /api/pet/ask, so voice uses the exact typed-chat path.
function askBridge(server, transcript) {
  const address = server.address();
  const host = !address.address || address.address === '0.0.0.0' || address.address === '::' ? '127.0.0.1' : address.address;
  const body = JSON.stringify({ input: transcript });
  return new Promise((resolve, reject) => {
    const req = http.request({
      host, port: address.port, path: '/api/pet/ask', method: 'POST',
      headers: {
        'Content-Type': 'application/json',
        'Content-Length': Buffer.byteLength(body),
        ...(process.env.PET_BRIDGE_TOKEN ? { Authorization: `Bearer ${process.env.PET_BRIDGE_TOKEN}` } : {}),
      },
    }, res => {
      const chunks = [];
      res.on('data', c => chunks.push(c));
      res.on('end', () => {
        try { resolve(JSON.parse(Buffer.concat(chunks).toString('utf8'))); } catch (err) { reject(err); }
      });
    });
    req.on('error', reject);
    req.end(body);
  });
}

async function handleVoice(server, req, res) {
  if (!authorized(req)) return send(res, 401, { ok: false, error: 'Unauthorized' });
  const audio = await readBody(req);
  if (audio.length < 1000) return send(res, 400, { ok: false, error: 'no audio' });
  const started = Date.now();
  const transcript = await transcribe(audio);
  const sttMs = Date.now() - started;
  console.log(`[pet-voice] ${audio.length} bytes -> ${transcript.length} chars in ${sttMs} ms`);
  if (!transcript) return send(res, 200, { ok: false, error: "I didn't catch that. Hold SPACE and try again?", stt_ms: sttMs });
  // X-Pet-Prefix (percent-encoded): context the PET wants in front of what was
  // said, e.g. Jack In's access mode and the CMD: rule. The transcript returned
  // to the PET stays just the spoken words.
  let prefix = '';
  try { prefix = decodeURIComponent(String(req.headers['x-pet-prefix'] || '')).slice(0, 600); } catch { prefix = ''; }
  const answer = await askBridge(server, prefix ? `${prefix} ${transcript}` : transcript);
  return send(res, 200, { ...answer, transcript, stt_ms: sttMs });
}

const originalCreateServer = http.createServer;
http.createServer = function (...args) {
  const handlerIndex = args.findIndex(a => typeof a === 'function');
  if (handlerIndex < 0) return originalCreateServer.apply(this, args);
  const handler = args[handlerIndex];
  let server;
  args[handlerIndex] = (req, res) => {
    const pathname = new URL(req.url || '/', 'http://localhost').pathname.replace(/\/+$/, '');
    if (req.method === 'POST' && pathname === '/api/pet/voice') {
      handleVoice(server, req, res).catch(err => {
        console.error('[pet-voice] failed', err);
        if (!res.headersSent) send(res, err.status || 500, { ok: false, error: String(err.message || err) });
      });
      return;
    }
    return handler(req, res);
  };
  server = originalCreateServer.apply(this, args);
  return server;
};
