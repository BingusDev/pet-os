#!/usr/bin/env node

const RELAY_URL = (process.env.PET_RELAY_URL || '').replace(/\/+$/, '');
const RELAY_BRIDGE_TOKEN = process.env.PET_RELAY_BRIDGE_TOKEN || '';
const LOCAL_BRIDGE_URL = (process.env.PET_LOCAL_BRIDGE_URL || `http://127.0.0.1:${process.env.PET_BRIDGE_PORT || 8797}`).replace(/\/+$/, '');
const LOCAL_BRIDGE_TOKEN = process.env.PET_BRIDGE_TOKEN || '';
const POLL_DELAY_MS = Number(process.env.PET_RELAY_POLL_DELAY_MS || 800);
const STATUS_INTERVAL_MS = Number(process.env.PET_RELAY_STATUS_INTERVAL_MS || 30000);

let lastStatusPushMs = 0;

function assertConfig() {
  const missing = [];
  if (!RELAY_URL) missing.push('PET_RELAY_URL');
  if (!RELAY_BRIDGE_TOKEN) missing.push('PET_RELAY_BRIDGE_TOKEN');
  if (missing.length) {
    console.error(`[pet-relay-client] missing required env: ${missing.join(', ')}`);
    process.exit(1);
  }
}

function sleep(ms) {
  return new Promise(resolve => setTimeout(resolve, ms));
}

async function fetchJson(url, options = {}, timeoutMs = 60000) {
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), timeoutMs);
  try {
    const res = await fetch(url, {
      ...options,
      signal: controller.signal,
      headers: {
        accept: 'application/json',
        ...(options.body ? { 'content-type': 'application/json' } : {}),
        ...(options.headers || {})
      }
    });
    const text = await res.text();
    let body = {};
    try {
      body = text ? JSON.parse(text) : {};
    } catch {
      body = { ok: false, error: `Non-JSON response: ${text.slice(0, 160)}` };
    }
    return { status: res.status, body };
  } finally {
    clearTimeout(timer);
  }
}

async function relayFetch(path, options = {}, timeoutMs = 60000) {
  return fetchJson(`${RELAY_URL}${path}`, {
    ...options,
    headers: {
      authorization: `Bearer ${RELAY_BRIDGE_TOKEN}`,
      ...(options.headers || {})
    }
  }, timeoutMs);
}

async function localBridgeFetch(path, body, timeoutMs = 55000) {
  const headers = {};
  if (LOCAL_BRIDGE_TOKEN) headers.authorization = `Bearer ${LOCAL_BRIDGE_TOKEN}`;
  return fetchJson(`${LOCAL_BRIDGE_URL}${path}`, {
    method: body ? 'POST' : 'GET',
    headers,
    body: body ? JSON.stringify(body) : undefined
  }, timeoutMs);
}

function localPathForJob(job) {
  if (job.kind === 'ask') return '/api/pet/ask';
  if (job.kind === 'action') return '/api/pet/action';
  if (job.kind === 'quick') return '/api/pet/quick';
  throw new Error(`Unsupported relay job kind: ${job.kind}`);
}

async function pushStatus(force = false) {
  if (!force && Date.now() - lastStatusPushMs < STATUS_INTERVAL_MS) return;
  lastStatusPushMs = Date.now();
  const local = await localBridgeFetch('/api/pet/status', null, 5000).catch(err => ({
    status: 502,
    body: { ok: false, error: String(err?.message || err) }
  }));
  await relayFetch('/api/bridge/status', {
    method: 'POST',
    body: JSON.stringify(local.body)
  }, 8000);
}

async function processJob(job) {
  const path = localPathForJob(job);
  const startedAt = Date.now();
  const local = await localBridgeFetch(path, job.body, 60000);
  await relayFetch(`/api/bridge/jobs/${encodeURIComponent(job.id)}/result`, {
    method: 'POST',
    body: JSON.stringify({
      status: local.status,
      body: local.body
    })
  }, 10000);
  console.log(`[pet-relay-client] job ${job.id} ${job.kind} -> ${local.status} in ${Date.now() - startedAt}ms`);
}

async function main() {
  assertConfig();
  console.log(`[pet-relay-client] relay: ${RELAY_URL}`);
  console.log(`[pet-relay-client] local bridge: ${LOCAL_BRIDGE_URL}`);
  await pushStatus(true).catch(err => console.warn(`[pet-relay-client] initial status failed: ${err?.message || err}`));

  while (true) {
    try {
      await pushStatus();
      const { body } = await relayFetch('/api/bridge/jobs/next', {}, 35000);
      if (body?.job) {
        console.log(`[pet-relay-client] job ${body.job.id} ${body.job.kind}`);
        await processJob(body.job);
      } else {
        await sleep(POLL_DELAY_MS);
      }
    } catch (err) {
      console.warn(`[pet-relay-client] ${err?.message || err}`);
      await sleep(2500);
    }
  }
}

main();

process.on('unhandledRejection', err => {
  console.warn(`[pet-relay-client] unhandled rejection: ${err?.stack || err?.message || err}`);
});

process.on('uncaughtException', err => {
  console.error(`[pet-relay-client] uncaught exception: ${err?.stack || err?.message || err}`);
  process.exit(1);
});
