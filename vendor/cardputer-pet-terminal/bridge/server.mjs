#!/usr/bin/env node
import http from 'node:http';
import { execFile } from 'node:child_process';
import { promisify } from 'node:util';
import fs from 'node:fs/promises';
import path from 'node:path';
import { randomUUID } from 'node:crypto';

const execFileAsync = promisify(execFile);

const PORT = Number(process.env.PET_BRIDGE_PORT || 8797);
const BRIDGE_TOKEN = process.env.PET_BRIDGE_TOKEN || '';
const DEFAULT_OPENCLAW_SESSION_KEY = process.env.OPENCLAW_SESSION_KEY || '';
const ARTIFACT_DIR = process.env.PET_ARTIFACT_DIR || '/tmp/cardputer-pet-terminal';
const SESSION_COMMANDS_ENABLED = process.env.PET_SESSION_COMMANDS !== '0';
const SESSION_KEY_PREFIX = process.env.PET_SESSION_KEY_PREFIX || 'pet-cardputer';
const SESSION_STATE_PATH = process.env.PET_SESSION_STATE_PATH || path.join(ARTIFACT_DIR, 'session-state.json');
const PET_TIME_ZONE = process.env.PET_TIME_ZONE || process.env.TZ || Intl.DateTimeFormat().resolvedOptions().timeZone || 'America/New_York';
const PET_WEATHER_LOCATION = process.env.PET_WEATHER_LOCATION || '';
const PET_NAVI_NAME = process.env.PET_NAVI_NAME || 'StarterNavi';
const PET_NAVI_DISPLAY_NAME = process.env.PET_NAVI_DISPLAY_NAME || `${PET_NAVI_NAME}.exe`;
const PET_DEVICE_NAME = process.env.PET_DEVICE_NAME || `${PET_NAVI_NAME} PET`;
const PET_CONTEXT_ENABLED = process.env.PET_CONTEXT !== '0';
const PET_CONTEXT_HINT = process.env.PET_CONTEXT_HINT ||
  `This message came from ${PET_NAVI_NAME}'s Cardputer PET terminal, a tiny typed NetNavi companion device. Reply as ${PET_NAVI_NAME} in a concise, useful, warm style for a small screen. Avoid markdown tables and long code blocks unless explicitly requested.`;
const PET_CARD_STYLE = [
  `You are replying to ${PET_NAVI_NAME}'s Cardputer PET terminal.`,
  "Format for a 240x135 tiny screen.",
  "Use plain ASCII punctuation only.",
  "No markdown tables, no code blocks, no long preamble.",
  "Prefer 3 to 6 short lines.",
  "Use compact labels like TARGET:, NEXT:, WATCH:, TIME:, NOTE: when helpful.",
  "Be warm and NetNavi-like, but keep it useful."
].join('\n');
const PET_MAX_DEVICE_REPLY_CHARS = Number(process.env.PET_MAX_DEVICE_REPLY_CHARS || 900);
// How much of an ask reaches the Navi (was a fixed 600). The PET learns this from
// `max_input` in each ask reply and sizes its prompts (command output, long questions) to it.
const PET_MAX_INPUT_CHARS = Math.max(600, Number(process.env.PET_MAX_INPUT_CHARS || 4000));
const PET_SLOW_ACTION_FALLBACK_MS = Number(process.env.PET_SLOW_ACTION_FALLBACK_MS || 12000);

let activeOpenClawSessionKey = DEFAULT_OPENCLAW_SESSION_KEY;
let previousOpenClawSessionKeys = [];
let lastWeatherCard = null;
let lastRemindersCard = null;
const startedAt = new Date();
const stats = {
  ask_count: 0,
  action_count: 0,
  last_latency_ms: null,
  last_ask_at: null,
  last_action_at: null,
  last_action: '',
  last_error: '',
  last_error_at: null
};
let lastDeviceTelemetry = null;

function sendJson(res, status, data) {
  res.writeHead(status, {
    'Content-Type': 'application/json; charset=utf-8',
    'Cache-Control': 'no-store'
  });
  res.end(JSON.stringify(data));
}

async function readJsonBody(req) {
  const chunks = [];
  for await (const chunk of req) chunks.push(chunk);
  const raw = Buffer.concat(chunks).toString('utf8').trim();
  return raw ? JSON.parse(raw) : {};
}

function requireToken(req, res) {
  if (!BRIDGE_TOKEN) return true;
  const url = new URL(req.url || '/', `http://${req.headers.host || 'localhost'}`);
  const header = req.headers.authorization || '';
  const token = url.searchParams.get('token') || header.replace(/^Bearer\s+/i, '');
  if (token === BRIDGE_TOKEN) return true;
  sendJson(res, 401, { ok: false, error: 'Unauthorized' });
  return false;
}

async function gatewayCall(method, params = {}, timeoutMs = 45000) {
  const { stdout } = await execFileAsync('openclaw', ['gateway', 'call', method, '--json', '--params', JSON.stringify(params)], { timeout: timeoutMs });
  return JSON.parse(stdout || '{}');
}

function shortSessionKey(key) {
  if (!key) return 'none';
  return key.length <= 20 ? key : `${key.slice(0, 12)}...${key.slice(-6)}`;
}

function formatDuration(ms) {
  const totalSeconds = Math.max(0, Math.floor(ms / 1000));
  const hours = Math.floor(totalSeconds / 3600);
  const minutes = Math.floor((totalSeconds % 3600) / 60);
  const seconds = totalSeconds % 60;
  if (hours) return `${hours}h ${minutes}m`;
  if (minutes) return `${minutes}m ${seconds}s`;
  return `${seconds}s`;
}

function noteError(err) {
  stats.last_error = String(err?.message || err || '').slice(0, 180);
  stats.last_error_at = new Date().toISOString();
}

function updateTelemetry(body = {}, req) {
  const now = new Date();
  const battery = Number(body.battery);
  const rssi = Number(body.rssi);
  const uptimeMs = Number(body.uptime_ms);
  lastDeviceTelemetry = {
    device: typeof body.device === 'string' ? body.device.slice(0, 40) : 'Cardputer',
    firmware: typeof body.firmware === 'string' ? body.firmware.slice(0, 32) : '',
    ip: typeof body.ip === 'string' ? body.ip.slice(0, 48) : '',
    ssid: typeof body.ssid === 'string' ? body.ssid.slice(0, 48) : '',
    battery: Number.isFinite(battery) ? battery : null,
    rssi: Number.isFinite(rssi) ? rssi : null,
    uptime_ms: Number.isFinite(uptimeMs) ? uptimeMs : null,
    charging: typeof body.charging === 'string' ? body.charging.slice(0, 20) : '',
    remote: req?.socket?.remoteAddress || '',
    seen_at: now.toISOString()
  };
  return lastDeviceTelemetry;
}

// Command lines ("CMD: ...") pass through untouched: stripping * or squeezing
// spaces there breaks PowerShell/shell math and globs. Prose loses markdown emphasis.
function sanitizeProseLine(line) {
  return line
    .replace(/\*\*(\S(?:[^*]*\S)?)\*\*/g, '$1')
    .replace(/(^|\s)\*(\S(?:[^*]*\S)?)\*(?=\s|[.,!?:;]|$)/g, '$1$2')
    .replace(/^(\s*)\* /, '$1- ')
    .replace(/[ \t]{2,}/g, ' ');
}

function sanitizeReply(text) {
  const base = String(text || '')
    .replace(/\r/g, '')
    .replace(/[‘’‚‛]/g, "'")
    .replace(/[“”„‟]/g, '"')
    .replace(/[–—]/g, '-')
    .replace(/…/g, '...')
    .replace(/\u00a0/g, ' ')
    .replace(/```[\s\S]*?```/g, block => block.replace(/```/g, '').slice(0, 900));
  return base
    .split('\n')
    .map(line => (/^\s*CMD:/i.test(line) ? line.trim() : sanitizeProseLine(line)))
    .join('\n')
    .replace(/[ \t]+\n/g, '\n')
    .replace(/\n{3,}/g, '\n\n')
    .trim();
}

function deviceReply(text, maxChars = PET_MAX_DEVICE_REPLY_CHARS) {
  const clean = sanitizeReply(text);
  if (clean.length <= maxChars) return clean;
  return `${clean.slice(0, Math.max(0, maxChars - 22)).trim()}\n\n[Reply shortened]`;
}

function formatPetInput(input) {
  const text = String(input || '').trim();
  if (!PET_CONTEXT_ENABLED || !text) return text;
  return `${PET_CONTEXT_HINT}\n\nUser typed on Cardputer: ${text}`;
}

function moodSignals(text, context = {}) {
  const t = String(text || '').toLowerCase();
  const scores = { calm: 0, happy: 0, alert: 0, thinking: 0 };
  const reasons = [];
  const add = (mood, amount, reason) => {
    scores[mood] += amount;
    if (reason) reasons.push(`${mood}:${reason}`);
  };

  if (!t.trim()) add('thinking', 2, 'empty');
  if (context.ok === false || /\b(error|failed|failure|crash|offline|unavailable|unauthorized|timeout|timed out)\b/.test(t)) {
    add('alert', 5, 'hard-error');
  }
  if (/\b(urgent|overdue|missed|blocked|stuck|late|warning|careful|problem|risk|watch out)\b/.test(t)) {
    add('alert', 4, 'attention');
  }
  if (/last err:\s*(?!none\b)\S/.test(t)) add('alert', 4, 'last-error');
  if (/last err:\s*none/.test(t)) add('calm', 3, 'system-ok');

  if (/\b(checking|working on it|still working|thinking|maybe|not sure|unclear|investigating|looking into|try again|in a moment)\b/.test(t)) {
    add('thinking', 3, 'uncertain');
  }
  if (/\b(done|saved|ready|complete|completed|synced|success|online|connected)\b/.test(t)) {
    add('happy', 2, 'success');
  }
  if (/\b(yay|awesome|perfect|great|nice|love it|excellent|sweet)\b/.test(t)) {
    add('happy', 3, 'positive');
  }

  if (context.action === 'system_status') add('calm', 4, 'system-status');
  if (context.action === 'weather') add('calm', 2, 'weather');
  if (context.kind === 'note') add('calm', 1, 'note');
  if (context.fromCache || /\bcached\b/.test(t)) add('thinking', 1, 'cached');

  return { scores, reasons };
}

function inferMood(text, context = {}) {
  const { scores, reasons } = moodSignals(text, context);
  const priority = ['alert', 'thinking', 'happy', 'calm'];
  let best = 'calm';
  for (const mood of priority) {
    if (scores[mood] > scores[best]) best = mood;
  }
  if (scores[best] <= 0) best = 'calm';

  if (best === 'happy' && scores.alert > 0) best = scores.alert >= scores.happy ? 'alert' : 'thinking';
  if (best === 'calm' && context.ok === false) best = 'alert';
  return { mood: best, reason: reasons.slice(0, 4).join(',') || 'default' };
}

function createSessionKey() {
  const stamp = new Date().toISOString().replace(/[-:.TZ]/g, '').slice(0, 14);
  return `${SESSION_KEY_PREFIX}-${stamp}-${randomUUID().slice(0, 8)}`;
}

async function loadSessionState() {
  if (!SESSION_COMMANDS_ENABLED) return;
  try {
    const parsed = JSON.parse(await fs.readFile(SESSION_STATE_PATH, 'utf8'));
    if (typeof parsed?.activeOpenClawSessionKey === 'string' && parsed.activeOpenClawSessionKey.trim()) {
      activeOpenClawSessionKey = parsed.activeOpenClawSessionKey.trim();
    }
    if (Array.isArray(parsed?.previousOpenClawSessionKeys)) {
      previousOpenClawSessionKeys = parsed.previousOpenClawSessionKeys
        .filter(key => typeof key === 'string' && key.trim())
        .slice(-10);
    }
    console.log(`[pet-bridge] restored OpenClaw session ${shortSessionKey(activeOpenClawSessionKey)}`);
  } catch (err) {
    if (err?.code !== 'ENOENT') console.warn(`[pet-bridge] could not load session state: ${err?.message || err}`);
  }
}

async function saveSessionState() {
  if (!SESSION_COMMANDS_ENABLED) return;
  await fs.mkdir(path.dirname(SESSION_STATE_PATH), { recursive: true });
  await fs.writeFile(SESSION_STATE_PATH, JSON.stringify({
    activeOpenClawSessionKey,
    defaultOpenClawSessionKey: DEFAULT_OPENCLAW_SESSION_KEY,
    previousOpenClawSessionKeys: previousOpenClawSessionKeys.slice(-10),
    updatedAt: new Date().toISOString()
  }, null, 2));
}

async function createOpenClawSession(requestedKey) {
  const result = await gatewayCall('sessions.create', { key: requestedKey });
  return typeof result?.key === 'string' && result.key.trim() ? result.key.trim() : requestedKey;
}

async function sendToOpenClaw(input) {
  const started = Date.now();
  if (!activeOpenClawSessionKey) {
    stats.last_latency_ms = Date.now() - started;
    return `PET link is online. Demo echo: ${input}`;
  }

  const before = await gatewayCall('sessions.get', { key: activeOpenClawSessionKey });
  const beforeMaxSeq = Math.max(0, ...(before?.messages || []).map(message => message?.__openclaw?.seq || 0));

  await gatewayCall('sessions.send', {
    key: activeOpenClawSessionKey,
    message: formatPetInput(input)
  });

  const deadline = Date.now() + 45000;
  while (Date.now() < deadline) {
    const after = await gatewayCall('sessions.get', { key: activeOpenClawSessionKey });
    const candidate = [...(after?.messages || [])]
      .reverse()
      .find(message => message?.role === 'assistant' && (message?.__openclaw?.seq || 0) > beforeMaxSeq);
    const textBlock = (candidate?.content || []).find(part => part?.type === 'text' && typeof part?.text === 'string');
    if (textBlock?.text?.trim()) {
      stats.last_latency_ms = Date.now() - started;
      return sanitizeReply(textBlock.text);
    }
    await new Promise(resolve => setTimeout(resolve, 900));
  }

  stats.last_latency_ms = Date.now() - started;
  return 'I received it. The OpenClaw session is still working on the reply.';
}

function weatherPrompt() {
  const locationHint = PET_WEATHER_LOCATION ? ` for ${PET_WEATHER_LOCATION}` : '';
  return `${PET_CARD_STYLE}

Build a WEATHER card${locationHint}.
Goal: give current conditions and the practical thing I should know before going outside.
If no exact location is available, use the best location context you have and say so briefly.
Suggested shape:
NOW: ...
LATER: ...
BRING: ...
Hard limit: 5 short lines, no extra commentary.`;
}

function cacheWeatherCard(reply) {
  const text = deviceReply(reply, 520);
  if (!text) return text;
  lastWeatherCard = {
    reply: text,
    at: new Date()
  };
  return text;
}

function cachedWeatherReply(reason) {
  if (lastWeatherCard?.reply) {
    return `${lastWeatherCard.reply}\n\nNOTE: cached ${formatDuration(Date.now() - lastWeatherCard.at.getTime())} ago; ${reason}.`;
  }
  const locationLine = PET_WEATHER_LOCATION ? `LOC: ${PET_WEATHER_LOCATION}` : 'LOC: set PET_WEATHER_LOCATION on the Mac bridge';
  return [
    'WEATHER LINK SLOW',
    locationLine,
    `NOW: ${PET_NAVI_NAME} is still checking.`,
    'NEXT: Try Weather again in a moment.'
  ].join('\n');
}

async function handleWeatherCommand() {
  if (!activeOpenClawSessionKey) {
    return cachedWeatherReply('OpenClaw session unavailable');
  }

  const request = sendToOpenClaw(weatherPrompt())
    .then(cacheWeatherCard)
    .catch(err => {
      noteError(err);
      return cachedWeatherReply('weather refresh failed');
    });

  return Promise.race([
    request,
    new Promise(resolve => setTimeout(() => resolve(cachedWeatherReply('weather refresh is slow')), PET_SLOW_ACTION_FALLBACK_MS))
  ]);
}

function remindersPrompt() {
  return `${PET_CARD_STYLE}

Build a VIEW REMINDERS card.
Goal: show only reminders, promises, deadlines, and follow-ups I may forget.
Do not decide my next task unless a reminder is overdue or urgent.
Group by urgency and include timing when known. If timing is unknown, mark it as unscheduled.
Hard limit: 6 short lines.
Suggested shape:
NOW: ...
SOON: ...
LATER: ...
UNSCHEDULED: ...
If nothing is known, give one helpful next capture prompt.`;
}

function cacheRemindersCard(reply) {
  const text = deviceReply(reply, 620);
  if (!text) return text;
  lastRemindersCard = {
    reply: text,
    at: new Date()
  };
  return text;
}

function cachedRemindersReply(reason) {
  if (lastRemindersCard?.reply) {
    return `${lastRemindersCard.reply}\n\nNOTE: cached ${formatDuration(Date.now() - lastRemindersCard.at.getTime())} ago; ${reason}.`;
  }
  return [
    'REMINDERS LINK SLOW',
    `NOW: ${PET_NAVI_NAME} is still checking.`,
    'SOON: Try View Reminders again.',
    'TIP: Use Ask to capture a new reminder.'
  ].join('\n');
}

async function handleRemindersCommand() {
  if (!activeOpenClawSessionKey) {
    return cachedRemindersReply('OpenClaw session unavailable');
  }

  const request = sendToOpenClaw(remindersPrompt())
    .then(cacheRemindersCard)
    .catch(err => {
      noteError(err);
      return cachedRemindersReply('reminders refresh failed');
    });

  return Promise.race([
    request,
    new Promise(resolve => setTimeout(() => resolve(cachedRemindersReply('reminders refresh is slow')), PET_SLOW_ACTION_FALLBACK_MS))
  ]);
}

function formatBridgeTime(date = new Date()) {
  return new Intl.DateTimeFormat('en-US', {
    timeZone: PET_TIME_ZONE,
    hour: 'numeric',
    minute: '2-digit',
    timeZoneName: 'short'
  }).format(date);
}

function formatBridgeDate(date = new Date()) {
  return new Intl.DateTimeFormat('en-US', {
    timeZone: PET_TIME_ZONE,
    weekday: 'short',
    month: 'short',
    day: 'numeric'
  }).format(date);
}

async function handleUtilityCommand(kind) {
  if (kind === 'what_next') {
    return sendToOpenClaw(`${PET_CARD_STYLE}

Build a WHAT'S NEXT card.
Goal: choose what I should do or pay attention to next, right now.
Use reminders only as supporting context. Do not list a reminders inbox here.
Prefer one actionable priority, the reason it matters, and the first tiny step.
If calendar/tasks/reminders are not available, infer from recent context and say so briefly.
Suggested shape:
TARGET: ...
NEXT: ...
WHY: ...
AFTER: ...`);
  }
  if (kind === 'reminders') {
    return handleRemindersCommand();
  }
  if (kind === 'weather') {
    return handleWeatherCommand();
  }
  if (kind === 'system_status') {
    return systemStatusCard();
  }
  throw new Error(`Unknown action: ${kind}`);
}

async function handleQuickCommand(kind, input, meta = {}) {
  const text = String(input || '').trim();
  if (!text) throw new Error('Missing quick command input');
  const offlineContext = meta?.offline
    ? `\nThis was saved offline on the PET and is being manually synced later. If timing matters, say whether it may be stale before acting. Device saved_at_ms=${Number(meta.saved_at_ms || 0)}.`
    : '';

  if (kind === 'idea') {
    return sendToOpenClaw(`${PET_CARD_STYLE}

Quick command: IDEA TO ${PET_NAVI_NAME.toUpperCase()}.
Goal: capture this idea in ${PET_NAVI_NAME}/OpenClaw memory or working context. Summarize it, tag it if useful, and suggest one next action only if obvious.${offlineContext}

Idea:
${text}

Reply with:
SAVED: ...
TAG: ...
NEXT: ...`);
  }

  if (kind === 'reminder') {
    return sendToOpenClaw(`${PET_CARD_STYLE}

Quick command: REMIND ME.
Goal: interpret this as a reminder/follow-up request. If the request lacks a date/time, ask one compact clarification. If it is actionable as-is, restate the reminder clearly.${offlineContext}

Reminder request:
${text}

Reply with:
REMINDER: ...
WHEN: ...
NEXT: ...`);
  }

  if (kind === 'note') {
    return sendToOpenClaw(`${PET_CARD_STYLE}

Quick command: LOCAL NOTE SYNC.
Goal: ingest this PET note into ${PET_NAVI_NAME}/OpenClaw context. If it seems stale because it was saved offline, say so briefly and preserve the useful part.${offlineContext}

Note:
${text}`);
  }

  throw new Error(`Unknown quick command: ${kind}`);
}

function systemStatusCard() {
  const device = lastDeviceTelemetry;
  const seenAgo = device?.seen_at ? formatDuration(Date.now() - Date.parse(device.seen_at)) : 'never';
  const battery = Number.isFinite(device?.battery) ? `${device.battery}%` : '--';
  const rssi = Number.isFinite(device?.rssi) ? `${device.rssi}dBm` : '--';
  const latency = Number.isFinite(stats.last_latency_ms) ? `${stats.last_latency_ms}ms` : '--';
  const lastError = stats.last_error ? stats.last_error : 'none';
  return [
    'SYSTEM STATUS',
    `Bridge: online ${formatDuration(Date.now() - startedAt.getTime())}`,
    `Device: ${device ? `seen ${seenAgo} ago` : 'not seen'}`,
    `Wi-Fi: ${device?.ip || '--'} ${rssi}`,
    `Battery: ${battery}`,
    `Session: ${shortSessionKey(activeOpenClawSessionKey)}`,
    `Latency: ${latency}`,
    `Last err: ${lastError}`
  ].join('\n');
}

function actionTitle(action) {
  if (action === 'what_next') return "WHAT'S NEXT";
  if (action === 'reminders') return 'VIEW REMINDERS';
  if (action === 'weather') return 'WEATHER';
  if (action === 'system_status') return 'STATUS';
  return PET_NAVI_NAME.toUpperCase();
}

function quickTitle(kind) {
  if (kind === 'idea') return `IDEA TO ${PET_NAVI_NAME.toUpperCase()}`;
  if (kind === 'reminder') return 'REMIND ME';
  if (kind === 'note') return 'NOTE SYNC';
  return 'QUICK';
}

function statusPayload() {
  return {
    ok: true,
    service: 'cardputer-pet-terminal',
    name: PET_NAVI_NAME,
    navi: PET_NAVI_NAME,
    navi_display: PET_NAVI_DISPLAY_NAME,
    device_name: PET_DEVICE_NAME,
    mood: activeOpenClawSessionKey ? 'calm' : 'demo',
    time: formatBridgeTime(),
    date: formatBridgeDate(),
    session: {
      configured: Boolean(activeOpenClawSessionKey),
      key: shortSessionKey(activeOpenClawSessionKey)
    },
    bridge: {
      uptime_ms: Date.now() - startedAt.getTime(),
      uptime: formatDuration(Date.now() - startedAt.getTime()),
      last_latency_ms: stats.last_latency_ms,
      ask_count: stats.ask_count,
      action_count: stats.action_count,
      last_error: stats.last_error,
      last_error_at: stats.last_error_at
    },
    device: lastDeviceTelemetry,
    endpoints: {
      health: '/health',
      status: '/api/pet/status',
      telemetry: '/api/pet/telemetry',
      ask: '/api/pet/ask',
      action: '/api/pet/action',
      quick: '/api/pet/quick'
    }
  };
}

async function handleSessionCommand(req, res, pathname) {
  if (!SESSION_COMMANDS_ENABLED) return sendJson(res, 404, { ok: false, error: 'Session commands disabled' });
  if (pathname.endsWith('/new')) {
    if (activeOpenClawSessionKey) previousOpenClawSessionKeys.push(activeOpenClawSessionKey);
    activeOpenClawSessionKey = await createOpenClawSession(createSessionKey());
    previousOpenClawSessionKeys = [...new Set(previousOpenClawSessionKeys)].filter(key => key !== activeOpenClawSessionKey).slice(-10);
    await saveSessionState();
    return sendJson(res, 200, { ok: true, reply: 'New PET session ready.', session: shortSessionKey(activeOpenClawSessionKey) });
  }
  if (pathname.endsWith('/default')) {
    if (!DEFAULT_OPENCLAW_SESSION_KEY) return sendJson(res, 400, { ok: false, error: 'OPENCLAW_SESSION_KEY is not configured' });
    if (activeOpenClawSessionKey && activeOpenClawSessionKey !== DEFAULT_OPENCLAW_SESSION_KEY) previousOpenClawSessionKeys.push(activeOpenClawSessionKey);
    activeOpenClawSessionKey = DEFAULT_OPENCLAW_SESSION_KEY;
    await saveSessionState();
    return sendJson(res, 200, { ok: true, reply: 'Default PET session restored.', session: shortSessionKey(activeOpenClawSessionKey) });
  }
  return sendJson(res, 404, { ok: false, error: 'Unknown session command' });
}

await loadSessionState();

const server = http.createServer(async (req, res) => {
  const url = new URL(req.url || '/', `http://${req.headers.host || 'localhost'}`);
  const pathname = url.pathname.replace(/\/+$/, '') || '/';

  try {
    if (req.method === 'GET' && (pathname === '/' || pathname === '/health')) {
      return sendJson(res, 200, statusPayload());
    }

    if (pathname.startsWith('/api/') && !requireToken(req, res)) return;

    if (req.method === 'GET' && pathname === '/api/pet/status') {
      return sendJson(res, 200, statusPayload());
    }

    if (req.method === 'POST' && pathname === '/api/pet/telemetry') {
      const body = await readJsonBody(req);
      return sendJson(res, 200, { ok: true, device: updateTelemetry(body, req), bridge: statusPayload().bridge });
    }

    if (req.method === 'POST' && pathname === '/api/pet/ask') {
      const body = await readJsonBody(req);
      const input = typeof body.input === 'string' ? body.input.trim() : '';
      if (!input) return sendJson(res, 400, { ok: false, error: 'Missing string field: input' });
      stats.ask_count++;
      stats.last_ask_at = new Date().toISOString();
      const reply = deviceReply(await sendToOpenClaw(input.slice(0, PET_MAX_INPUT_CHARS)));
      const mood = inferMood(reply, { surface: 'ask', ok: true });
      return sendJson(res, 200, { ok: true, reply, mood: mood.mood, mood_reason: mood.reason, max_input: PET_MAX_INPUT_CHARS, at: new Date().toISOString() });
    }

    if (req.method === 'POST' && pathname === '/api/pet/action') {
      const body = await readJsonBody(req);
      const action = typeof body.action === 'string' ? body.action.trim() : '';
      stats.action_count++;
      stats.last_action = action;
      stats.last_action_at = new Date().toISOString();
      const replyLimit = action === 'weather' ? 520 : action === 'reminders' ? 620 : PET_MAX_DEVICE_REPLY_CHARS;
      const reply = deviceReply(await handleUtilityCommand(action), replyLimit);
      const mood = inferMood(reply, { surface: 'action', action, ok: true, fromCache: /\bcached\b/i.test(reply) });
      return sendJson(res, 200, { ok: true, action, title: actionTitle(action), reply, mood: mood.mood, mood_reason: mood.reason, at: new Date().toISOString() });
    }

    if (req.method === 'POST' && pathname === '/api/pet/quick') {
      const body = await readJsonBody(req);
      const kind = typeof body.kind === 'string' ? body.kind.trim() : '';
      const input = typeof body.input === 'string' ? body.input.trim() : '';
      if (!kind || !input) return sendJson(res, 400, { ok: false, error: 'Missing fields: kind and input' });
      stats.action_count++;
      stats.last_action = `quick:${kind}`;
      stats.last_action_at = new Date().toISOString();
      const reply = deviceReply(await handleQuickCommand(kind, input.slice(0, 800), body.meta || {}));
      const mood = inferMood(reply, { surface: 'quick', kind, ok: true, offline: Boolean(body.meta?.offline) });
      return sendJson(res, 200, { ok: true, kind, title: quickTitle(kind), reply, mood: mood.mood, mood_reason: mood.reason, at: new Date().toISOString() });
    }

    if (req.method === 'POST' && pathname.startsWith('/api/pet/session/')) {
      return handleSessionCommand(req, res, pathname);
    }

    return sendJson(res, 404, { ok: false, error: 'Not found' });
  } catch (err) {
    noteError(err);
    console.error(`[pet-bridge] ${req.method} ${pathname} failed`, err);
    return sendJson(res, 500, { ok: false, error: String(err?.message || err), mood: 'alert', mood_reason: 'server-error' });
  }
});

server.listen(PORT, '0.0.0.0', () => {
  console.log(`[pet-bridge] listening on :${PORT}`);
  console.log(`[pet-bridge] OpenClaw session ${shortSessionKey(activeOpenClawSessionKey)}`);
});
