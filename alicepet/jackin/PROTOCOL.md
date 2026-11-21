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
only runs while Full Access is on. Every command, from either side, is shown in
the app's live log and on the PET. The PC's microphone records only while
the person holds the app's talk button; the PET can't start it.

## PC → PET

| `t` | Fields | Meaning |
|---|---|---|
| `hello` | `host`, `os`, `user`, `admin`, `full`, `input`, `app` | Session start (also after an elevated relaunch). `input`: true in Ask or Full (keys may be forwarded) |
| `scan` | `cpu`, `ram` (% used), `disk`, `disk_free` (%), `startup`, `defender` (`on`/`off`/`?`), `defender_since`, `top` [`{name, mb}`] | Read-only scan results |
| `state` | `full`, `admin`, `net`, `input`, optional `denied` (`"admin"` when UAC was cancelled) | Something changed on the PC |
| `open` | `chip` | Open a PC-capable chip (`terminal`, `jellyfin`, `retro`, `lan-tech`) on the PET. Ignored for any other id |
| `mirror` | `on` | Start (`true`) or stop (`false`) the PET screen mirror. The PET stops when no PC is linked |
| `key` | `k` | One key: `Left`, `Right`, `Up`, `Down`, `Enter`, `Escape`, `Backspace`, `Tab`. Dropped unless `input` is true |
| `char` | `c` | One UTF-8 character (1 to 4 bytes). Dropped unless `input` is true |
| `log` | `text`, `ok` | A command the app ran itself (scan steps) |
| `result` | `id`, `ok`, `out`, optional `reason` (`"read-only"`) | Result of a PET `run` |
| `net_fail` | `id` | The PC could not reach the host of `net_open` `id` |
| `chat` | `text` | Typed to Alice in the app; the PET asks the bridge |
| `voice` | `wav` (base64, 16 kHz mono 16-bit WAV, at most 15 s) | Spoken to Alice; the PET sends it to the bridge's voice route |
| `bye` | `why` | The app is closing |

## PET → PC

| `t` | Fields | Meaning |
|---|---|---|
| `ask` | `what`: `full` / `admin` / `net` | Highlights that button on the PC; nothing changes until it is clicked |
| `drop` | `what`: `full` / `net` | Turn that off (taking access away needs no click) |
| `run` | `id`, `cmd` | Run one PowerShell command (refused with `reason: "read-only"` unless Full Access is on) |
| `scan` | | Scan again |
| `net_open` | `id`, `host`, `port` | Sharing: dial `host:port`, then dial PET:7342 and send `tunnel <id>\n`; then pipe bytes both ways |
| `jackout` | `why` | Drop everything and close the app |
| `alice` | `ok`, `text`, `heard` (voice transcript), `cmd` (a suggested fix, or empty) | Alice's answer to `chat` / `voice` |
| `chips` | `list`: [`{id, name, number, code, desc, art}`] | The PC-capable chips, sent once after `hello`. `art` is the small label PNG, base64 |
| `frame` | `screen`, `w`, `h`, `y`, `n`, `rle` | Changed rows of the PET screen: `n` rows from row `y`. `rle` is base64 of run-length RGB565 (run `1..255` as one byte, then the pixel as two bytes, big-endian), row-major. A new `screen`, size or link starts with a full frame. Sent about 12 times a second while mirroring |

When the control line closes (unplugged, PET left the chip), the app drops
Full Access, stops sharing and closes.
