# JACK IN protocol (v2)

The PET is a USB device; the PC sees a read-only drive (`ALICEPET`, the app)
and a network adapter. `usb-gadget.sh` gives the PET `10.55.0.1` and the PC
`10.55.0.2` (no gateway, no DNS, so the PC never routes through the PET).

**The PC always dials out.** Nothing listens on the PC, so there is no firewall
prompt and nothing to remove afterwards.

| Port (on the PET) | Use |
|---|---|
| 7341 | Control line: one JSON object per line, UTF-8 |
| 7342 | Share PC Internet: tunnels the PC dials back |
| 127.0.0.1:3128 | HTTP proxy for PET programs while sharing (not reachable from the PC) |

One PC session at a time; a newer control connection replaces the old one
(the app relaunching itself elevated).

## Who decides what

Everything that can change the PC is decided **on the PC**, by the person at its
keyboard: Full Access, Request Admin (the Windows UAC prompt) and Share PC
Internet are buttons in the app. The PET can *ask* for them (`ask`), can *take
them away* (`drop`, `jackout`), and can send a command (`run`) that the app
judges by its access mode: **Read** runs only read-only commands (Get-*,
Test-*, sort/select/format, ipconfig, whoami, systeminfo, tasklist...), **Ask**
shows Yes/No for each one, **Full** runs them except destructive ones (delete,
kill, uninstall, registry/boot/account/security/policy changes, anything it
can't parse), which still get Yes/No. The app decides; the PET never can.

Alice's command loop: when she answers a `chat`/`voice` with a `CMD:` line, the
PET sends it as `run` with `agent: true`, and the `result` goes back to her as
the next turn, until she answers without a command. After 25 commands (Full;
8 in Read/Ask) for one message the PET holds her next one and sends
`agent_more`; the PC asks "Keep going?" and answers `agent_more` `yes`. Read mode (`state` `mode: "read"`), `abort` or JACK OUT ends it. Every command, from either side, is shown in
the app's live log and on the PET. The PC's microphone records only while
the person holds the app's talk button; the PET can't start it.

## PC → PET

| `t` | Fields | Meaning |
|---|---|---|
| `hello` | `host`, `os`, `user`, `admin`, `full`, `input`, `mode` (`read`/`ask`/`full`), `app` | Session start (also after an elevated relaunch). `input`: true in Ask or Full (keys may be forwarded) |
| `scan` | `cpu`, `ram` (% used), `disk`, `disk_free` (%), `startup`, `defender` (`on`/`off`/`?`), `defender_since`, `top` [`{name, mb}`] | Read-only scan results |
| `state` | `full`, `admin`, `net`, `input`, `mode`, optional `denied` (`"admin"` when UAC was cancelled) | Something changed on the PC |
| `open` | `chip` | Open a PC-capable chip (`terminal`, `jellyfin`, `retro`, `lan-tech`) on the PET. Ignored for any other id. Only honoured after [K] was pressed on the PET (`pet` `control: true`), and `terminal` only after its own [K] (`pet` `terminal: true`); otherwise the PET answers `denied` |
| `home` | | Leave the chip the PC opened (its player or game is stopped first) and go back to the PET's Jack In screen. The PC sends `open` or `home` one at a time: the PET always leaves the open chip before entering the next |
| `mirror` | `on` | Start (`true`) or stop (`false`) the PET screen mirror. The PET stops when no PC is linked |
| `key` | `k` | One key: `Left`, `Right`, `Up`, `Down`, `Enter`, `Escape`, `Backspace`, `Tab`. Dropped unless `input` is true and the PET granted control ([K]) |
| `char` | `c` | One UTF-8 character (1 to 4 bytes). Dropped unless `input` is true and the PET granted control |
| `pointer` | `x`, `y`, `b` (`down`/`up`) | A mouse button in the mirror, in PET pixels (`-1`/`-1`: release only). Same rule as `key` |
| `log` | `text`, `ok` | A command the app ran itself (scan steps) |
| `result` | `id`, `ok`, `out` (at most ~4 KB), optional `reason` (`"read-only"`: not run in Read mode; `"declined"`: No on the PC) | Result of a PET `run` |
| `abort` | | Stop Alice's command loop |
| `agent_more` | `yes` | Answer to the PET's `agent_more`: true runs her held command and allows as many again |
| `net_fail` | `id` | The PC could not reach the host of `net_open` `id` |
| `chat` | `text` | Typed to Alice in the app; the PET asks the bridge |
| `voice` | `wav` (base64, 16 kHz mono 16-bit WAV, at most 15 s) | Spoken to Alice; the PET sends it to the bridge's voice route |
| `progress` | `item`, `pos` (seconds of show time), `paused`, `state`: `playing` / `stopped` / `ended` / `error` / `seek` / `handoff`, optional `error` | The PET's video playing in the app: sent every 10 s, and once when it stops. `seek` (only for a non-seekable `play`) asks the PET to restart the stream at `pos`. `handoff`: the stream went to a player that reports nothing back (no WMP, or the Mac/Linux client); the PET returns to its list and leaves the Jellyfin session and transcode running |
| `bye` | `why` | The app is closing |

## PET → PC

| `t` | Fields | Meaning |
|---|---|---|
| `ask` | `what`: `full` / `admin` / `net` | Highlights that button on the PC; nothing changes until it is clicked |
| `drop` | `what`: `full` / `net` | Turn that off (taking access away needs no click) |
| `run` | `id`, `cmd`, optional `agent` (true: from Alice's loop, shown in the chat) | Run one PowerShell command, as the access mode allows (above) |
| `pet` | `operator`, `control`, `terminal` | Sent after `hello` and whenever a grant changes: who runs the PET (`[jackin] operator=`, default `Holden`; the app shows it as OPERATOR and on chat lines, never the Windows user), and what the PET lets this PC do. `control`: the PC may open and drive chips ([K] on the PET, or `[jackin] pc_control=on`). `terminal`: it may open Terminal (always its own [K], every session). The PC's own `input` flag is never enough |
| `denied` | `what`: `control` / `terminal`, `chip` | An `open` was refused until [K] is pressed on the PET. When it is, the PET opens `chip` itself and sends `pet` with the new grants |
| `agent_more` | `steps`, `next` | Alice hit the step cap; ask Holden "Keep going?" and reply `agent_more` |
| `agent_stop` | `why` | Alice's command loop ended early |
| `scan` | | Scan again |
| `net_open` | `id`, `host`, `port` | Sharing: dial `host:port`, then dial PET:7342 and send `tunnel <id>\n`; then pipe bytes both ways |
| `jackout` | `why` | Drop everything and close the app |
| `alice` | `ok`, `text`, `heard` (voice transcript), `cmd` (the command she is running next, or empty), `step` (1..8, 0 = none) | Alice's answer to `chat` / `voice`; a `run` with `agent: true` follows when `cmd` is set |
| `chips` | `list`: [`{id, name, number, code, desc, art}`] | The PC-capable chips, sent once after `hello`. `art` is the small label PNG, base64 |
| `play` | `kind`: `video` / `game`, `title`, `url`; video: `item`, `start`, `base`, `length`, `seekable`, `direct_url` (empty unless `[jellyfin] pc_direct=on`: Jellyfin's own URL with an API key made for this play, which is an administrator key); game: `direct` | Play here instead of on the PET. `url` is on the PET's relay (below). Video: open it in the NOW PLAYING player, seek to `start`; show time = `base` + player position. Game: open `url` in the default browser (`direct` is RomM's own URL, for the log) |
| `stop_play` | `item` | The PET stopped the video (Esc on the PET, or it left the chip); no `progress` needed |
| `player` | `cmd`: `seek` / `volume` / `pause`, `by` | The PET's arrows and space while its video plays here |
| `frame` | `screen`, `w`, `h`, `y`, `n`, `rle` | Changed rows of the PET screen: `n` rows from row `y`. `rle` is base64 of run-length RGB565 (run `1..255` as one byte, then the pixel as two bytes, big-endian), row-major. A new `screen`, size or link starts with a full frame. Sent about 12 times a second while mirroring |

When the control line closes (unplugged, PET left the chip), the app drops
Full Access, stops sharing and closes.

## Media relay (PET → PC playback)

While a PC is linked, Jellyfin and Retro play in the app, not on the PET. The
PET serves them on the link only (`[jackin] listen`), adding the server
credentials itself; the PC never sees the Jellyfin API key or the RomM token.

- `http://10.55.0.1:7344/m/<token>/<name>`: one upstream file (a Jellyfin direct
  stream for H.264 MP4/MKV with AAC/MP3 audio, else an H.264/AAC MP4 transcode).
  GET and HEAD, `Range` passed through. Unknown tokens get 404.
- `http://10.55.0.1:7345/__pet/<token>?go=<path>`: sets an HttpOnly cookie and
  redirects to `<path>`; every request with that cookie is forwarded to RomM's
  origin (Bearer token added, `Secure`/`Domain` stripped from cookies, `Location`
  made relative). WebSockets are not relayed (RomM shows a harmless banner).
- Tokens are 128-bit random and die with the link. Ports: `[jackin] media_port`
  (default 7344) and `media_port + 1`.

While the PC operates a chip, the PET's panel shows a Jack In card
("OPERATING <chip> from <PC>") on LVGL's top layer; the chip itself keeps
rendering on the screen the mirror captures. The PET's own keys are ignored
then, except [X] (jack out).

