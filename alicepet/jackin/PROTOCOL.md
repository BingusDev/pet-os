# JACK IN protocol (v1)

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
the app's live log and on the PET.

## PC → PET

| `t` | Fields | Meaning |
|---|---|---|
| `hello` | `host`, `os`, `user`, `admin`, `full`, `app` | Session start (also after an elevated relaunch) |
| `scan` | `cpu`, `ram` (% used), `disk`, `disk_free` (%), `startup`, `defender` (`on`/`off`/`?`), `defender_since`, `top` [`{name, mb}`] | Read-only scan results |
| `state` | `full`, `admin`, `net`, optional `denied` (`"admin"` when UAC was cancelled) | Something changed on the PC |
| `log` | `text`, `ok` | A command the app ran itself (scan steps) |
| `result` | `id`, `ok`, `out`, optional `reason` (`"read-only"`) | Result of a PET `run` |
| `net_fail` | `id` | The PC could not reach the host of `net_open` `id` |
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

When the control line closes (unplugged, PET left the chip), the app drops
Full Access, stops sharing and closes.
