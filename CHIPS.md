# Battle Chip folder (v0.5, 2026-11-16 20:26: POSTS dropped, work-only + VOICE covers it)
| # | Chip | Element | Shell | Sprite | What it does |
|---|---|---|---|---|---|
| 002 | JELLYFIN | ♪ | Purple | Clapperboard | On-screen player: server transcodes to 320x170-ish H.264 low bitrate; PET just decodes. No TV/HDMI path needed |
| 003 | JACK IN | ⚡ | Black/red | USB plug | PC sidekick app (read-only -> Full Access, Request Admin, Share PC Internet, Log, Jack Out) |
| 004 | REMOTE | ◎ | Red | IR remote | Learn + replay IR remotes (store TVs, projector, AC) |
| 005 | LAN TECH | ◆ | Green | RJ45 plug | Jack test: link/speed, DHCP, gateway/DNS, ping/speed, device list, LLDP switch port |
| 006 | VOICE | ♥ | Pink | Microphone | Push-to-talk to Alice, spoken + on-screen reply; never always-listening |
| 007 | RETRO | ✦ | Gray | Gamepad | RomM library played ON THE PET SCREEN (default): GB/GBC 1:1 160x144, GBA 1:1 240x160, NES/SNES/Genesis/SMS scaled to 170px tall; keyboard or BT controller; saves sync to RomM. HDMI optional |
| 008 | TERMINAL | ■ | Dark | >_ monitor | SSH bookmarks over Tailscale (Pi, media, Mox, VPS, gaming PC, MBA), Ask-Alice command help |
Dropped: CARD SCAN (camera too weak; Holden said no, do NOT re-suggest), PS5, BACKUP, TV IR (-> REMOTE).

## Maybe-later chips (hardware-backed, not committed)
- VOICE MEMO: mic field recorder, saves clips, syncs to Mac mini. Holden likes it.
- ALARM: RTC alarms/timers, wakes from deep sleep. Holden likes it.
- DICE: shake (IMU) to roll any dice set, crit animations. Holden likes it.
- ALERTS: "!" badge on Navi Home when something needs Holden (backup fail, Mox offline, etc.). Holden likes it.
- WAKE-ON-LAN: wake gaming PC etc. Holden unsure of use case; low priority.
- NAVI MAIL (Holden's idea, 2026-11-16): Navi-to-Navi inbox. MMBN email screen; Alice carries/reads mail between Navis. First pen pal: Reese's Navi Imurrs (agentId imurrs, same gateway, agent-to-agent main<->imurrs already enabled). Later: friends' Navis on other gateways need a mail protocol. Design TBD.
Rejected 2026-11-16 (don't re-pitch): CARB CHECK, LOCALS, POWER CUT, ADGUARD, C64, PRINTER.
- RFID: no built-in radio; needs M5 Grove RFID add-on. Encrypted 13.56MHz hotel keys often won't clone. Park.
- CAMERA (Harinezumi): uses built-in 8MP cam (full model only). Emulate Digital Harinezumi 2 look in software. Recipe prototype: bin/harinezumi.py (downsample-blend soft, per-channel crushed-black/blown-white curves, green/cool cast, ~1.7x sat, highlight halation/bloom, vignette, shadow-weighted grain). Refs: camera-ref/01-04 (Holden's real DH2 shots, 2011). Tune per reference; offer film presets (DH2 photo / DH2 1/8 video / B&W).
