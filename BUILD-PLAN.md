# AlicePET OS — Build Plan (v1, 2026-11-16)

Device: M5Stack CardputerZero (Pi CM0, 512 MB RAM, 320×170 ST7789, H.264 HW decode, IR, mic/speaker, Ethernet behind USB hub).
Mockups: `mockups/AlicePET-master-v1.png` (+ per-area HTML in `mockups/`). Chips: `CHIPS.md`.

## Architecture
- **PET** runs a single full-screen app (C++ / LVGL 9.5 from the M5 CardputerZero app template, fbdev), plus small helpers (mpv, emulator cores, IR tools).
- **Alice stays on the Mac mini.** PET talks to her over Tailscale (OpenClaw node pairing or a small HTTPS bridge). Heavy work (STT/TTS, transcodes, LLM) never runs on the PET.
- **Every chip = one module** with the same contract: `screen()`, `keys()`, `status()`. Folder/launcher just loads modules.

## Phase 0 — Device day (≈1 evening)
1. Boot the stock image, update, set hostname `alice-pet`, Wi-Fi (home + store + S23 hotspot).
2. Tailscale + SSH key from the Mac. Add to `~/.ssh/config`.
3. Prove hardware: screen fps, keyboard events, speaker/mic, IR TX/RX loopback, Ethernet in host mode, **USB device mode** (mass storage + ECM/RNDIS to a PC). Record results in NOTES.md.
✅ Done when every item above has a pass/fail with evidence.

## Phase 1 — Shell + quick wins
- **Launcher:** Boot → Navi Home → Chip Folder (cartridge chips + sprites from `sprites.js`).
- **VOICE:** push-to-talk record → send to Mac → Alice reply as text + TTS audio back. Mic only while key held.
- **TERMINAL:** host list from a config file, SSH via keys, Ask-Alice suggest → user presses Enter.
- **Net Scan** home widget (ping list).

## Phase 2 — Media
- **JELLYFIN:** browse via Jellyfin API; request H.264 ≤ 360p transcode; play with mpv (HW decode) fullscreen; report progress so resume syncs.
- **RETRO:** RomM API browse; download ROM + save; run libretro cores (GB/GBC/GBA 1:1, NES/SNES/GEN scaled); push save back to RomM on exit.

## Phase 3 — Field tools
- **LAN TECH:** link/DHCP/gateway/DNS/speed checks, LLDP neighbor, ARP/nmap-lite device list, plain-English verdict from Alice.
- **REMOTE:** IR learn (record raw/NEC), device profiles, test-fire, macros.

## Phase 4 — JACK IN
- PET in USB device mode: composite gadget (mass storage with portable apps + USB network).
- **Win app first** (then Mac/Linux): read-only scan → Full Access toggle → Request Admin (UAC/sudo) → Share PC Internet → Log → Jack Out. Nothing installed; session ends on close/unplug.

## Rules carried from design
- Mic never always-on. Jack In starts read-only; every write shown live; admin only via OS prompt the user clicks.
- No secrets on the PET beyond its own SSH key + Tailscale identity.
- Test on Holden's own machines before store machines.

## Power plan (added 2026-11-16 20:39)
Pi CM0 has no real suspend; manage power per subsystem.
- **Per-chip power profiles:** each chip module declares the hardware it needs (`needs: [wifi, usb_hub, ir, audio, hdmi, cores:n]`); launcher powers everything else down on chip slot-in, restores on exit.
- Knobs: backlight PWM (biggest), cpufreq governor + max-freq cap, offline cores 1-3 (`/sys/devices/system/cpu/cpuN/online`), HDMI off, Bluetooth off (rfkill) unless controller paired, Wi-Fi `iw power_save on` when idle, USB hub + Ethernet chip power-off when LAN/USB-A unused (check if hub VBUS/reset is GPIO-switchable), audio codec standby, ACT LED off, camera driver not loaded, trim services.
- **Nap mode:** screen off, 1 core @ lowest freq, Wi-Fi powersave, hub off; any key wakes. Optional **Deep nap = full poweroff + RTC alarm wake** if RX8130CE alarm can trigger power-on (verify on hardware).
- Measure with BQ27220 fuel gauge (real mA); show live draw + per-chip estimate in status bar. Phase 0 task: baseline current for idle / Wi-Fi / video / each knob.

## Device-day findings (2026-11-18, first boot on CardputerZero trixie stable)
- [x] **Volume OSD:** show a volume bar when fn+D / fn+S / fn+A (KEY_VOLUMEUP/DOWN/MUTE on tca8418c) are pressed. A user service (`~/.local/bin/volkeys.py`, `volkeys.service`) already changes PipeWire volume in 5% steps; AlicePET only needs to read the keys or poll `wpctl get-volume` and draw the overlay.
- [x] **Jellyfin auth for mpv:** `write_mpv_auth_config()` sends `X-Emby-Token`, which this server rejects (401). Send `Authorization: MediaBrowser Token="<key>"` instead. Until then the device uses `player=/usr/bin/env mpv ...` so the key goes in the URL, which disables mpv IPC (pause, seek, resume).
- [x] **Default player audio:** use `--ao=pipewire` in the shipped `[jellyfin] player=`; `--ao=alsa` goes to HDMI on this board.
- [x] **pet-setup.sh audio:** install the WirePlumber rule forcing S16LE/48 kHz on `alsa_output.platform-sound.*`; the ES8389 plays silence at PipeWire's default s32le.
