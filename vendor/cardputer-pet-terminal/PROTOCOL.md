# Cardputer PET Protocol

Base URL:

```text
http://<mac-lan-ip>:8797
```

If `PET_BRIDGE_TOKEN` is configured, send:

```text
Authorization: Bearer <token>
```

## Health

```http
GET /health
```

Response:

```json
{
  "ok": true,
  "service": "cardputer-pet-terminal",
  "name": "StarterNavi",
  "navi": "StarterNavi",
  "navi_display": "StarterNavi.exe",
  "device_name": "StarterNavi PET",
  "mood": "calm",
  "time": "9:41 PM EDT",
  "session": {
    "configured": true,
    "key": "pet-cardpu...a1b2c3"
  }
}
```

## Status

```http
GET /api/pet/status
```

Same shape as `/health`. The firmware polls this periodically for idle screen state.

## Ask Navi

```http
POST /api/pet/ask
Content-Type: application/json
```

Request:

```json
{
  "input": "What should I do next?"
}
```

Response:

```json
{
  "ok": true,
  "reply": "Start with the calendar item at 3 PM, then clear the reminder about groceries.",
  "mood": "calm",
  "at": "2026-06-06T20:00:00.000Z"
}
```

## Menu Actions

```http
POST /api/pet/action
Content-Type: application/json
```

Request:

```json
{
  "action": "reminders"
}
```

Actions:

- `reminders`
- `weather`
- `system_status`

## Quick Commands

```http
POST /api/pet/quick
Content-Type: application/json
```

Request:

```json
{
  "kind": "note",
  "input": "Offline note to sync later",
  "meta": {
    "offline": true,
    "saved_at_ms": 123456
  }
}
```

Kinds:

- `note`

`meta.offline` is optional. Current firmware only uses this endpoint when manually syncing compatible SD-card outbox items.

## Session Helpers

Optional bridge-only helpers:

```http
POST /api/pet/session/new
POST /api/pet/session/default
```

These are not in the Cardputer UI yet, but they are useful during local testing.
