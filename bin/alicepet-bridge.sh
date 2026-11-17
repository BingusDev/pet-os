#!/usr/bin/env bash
# Start the Talk-to-Alice bridge for AlicePET (cardputer-pet-terminal bridge/server.mjs).
#
# It reuses that project's .env (PET_BRIDGE_TOKEN, names) but always talks to the
# dedicated OpenClaw session agent:main:pet (delivery: none), never the Telegram
# session that .env's OPENCLAW_SESSION_KEY points at, so PET chats stay off Telegram.
#
#   BIND_HOST=<mac tailscale ip> bin/alicepet-bridge.sh   tailnet only (for the device)
#   bin/alicepet-bridge.sh                               127.0.0.1 only (desktop testing)
#   BIND_HOST=0.0.0.0 bin/alicepet-bridge.sh             every interface (LAN too)
#
# The bridge drives Alice (agent main, full tools), so any non-loopback bind
# requires PET_BRIDGE_TOKEN (in .env or the environment); the device sends it
# from /etc/alicepet.conf [bridge] token=.
#
# Push-to-talk: POST /api/pet/voice transcribes locally with openai-whisper
# (PET_VOICE_MODEL, default large-v3-turbo) and asks the same session.
#
# Foreground process; stop with Ctrl-C. No LaunchAgent is installed.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BRIDGE_ROOT="${BRIDGE_ROOT:-$HERE/../vendor/cardputer-pet-terminal}"
cd "$BRIDGE_ROOT"

CALLER_TOKEN="${PET_BRIDGE_TOKEN:-}"
if [[ -f .env ]]; then
  while IFS= read -r line || [[ -n "$line" ]]; do
    [[ -z "$line" || "$line" == \#* ]] && continue
    key="${line%%=*}"
    value="${line#*=}"
    [[ "$key" =~ ^[A-Za-z_][A-Za-z0-9_]*$ ]] || continue
    export "$key=$value"
  done < .env
fi

# A token from the caller's environment wins over .env (whose value is currently empty).
if [[ -n "$CALLER_TOKEN" ]]; then export PET_BRIDGE_TOKEN="$CALLER_TOKEN"; fi
export OPENCLAW_SESSION_KEY="agent:main:pet"
export PET_SESSION_COMMANDS=0
export PET_ARTIFACT_DIR="${PET_ARTIFACT_DIR_ALICEPET:-/tmp/alicepet-bridge}"
export PET_SESSION_STATE_PATH="$PET_ARTIFACT_DIR/session-state.json"
export PET_NAVI_NAME="Alice"
export PET_DEVICE_NAME="AlicePET"

export BIND_HOST="${BIND_HOST:-127.0.0.1}"
if [[ "$BIND_HOST" != "127.0.0.1" && -z "${PET_BRIDGE_TOKEN:-}" ]]; then
  echo "Refusing to listen on $BIND_HOST without PET_BRIDGE_TOKEN (generate one: openssl rand -hex 24)." >&2
  exit 1
fi

# Create the session once; delivery stays "none", so replies are only read back by the bridge.
openclaw gateway call sessions.create --json --params '{"key":"agent:main:pet"}' >/dev/null 2>&1 || true

# voice-route.mjs adds POST /api/pet/voice (local whisper -> same session); see that file.
exec node --import "$HERE/bind-host.mjs" --import "$HERE/voice-route.mjs" bridge/server.mjs
