# AlicePET OS: handoff notes

MMBN-style "PET" launcher for the M5Stack **CardputerZero** (Pi CM0, Linux, 320x170 screen).
Owner: Holden. Navi: Alice (OpenClaw agent on Holden's Mac mini).

## Layout
- `alicepet/`: the app (C++ / LVGL, built from the M5 CardputerZero app Template). MVVM-ish: `src/model`, `src/view`, `src/viewmodel`, `src/platform`, `src/app`. Chip modules live in `src/viewmodel/chips/` + `src/view/chips/`.
- `mockups/`: approved designs. `AlicePET-master-v1.png` is the overview. `pet.html`, `chips.html`, `media.html`, `tools.html`, `comms.html` are the HTML sources. Chip PNGs in the app are rendered from `chips.html`.
- `bin/build-device.sh alicepet`: cross-builds the ARM64 binary + `.deb` on a separate Linux box (`grandworld-pi`, inside `debian:trixie` docker). Prints `BUILD_OK`. Adapt the ssh host for your machine.
- `bin/alicepet-bridge.sh`: starts the Mac-side bridge (vendored in `vendor/cardputer-pet-terminal/bridge/`) that relays PET chat/voice to an OpenClaw session (`agent:main:pet`). Localhost only by default; needs `PET_BRIDGE_TOKEN` to bind elsewhere. `bin/voice-route.mjs` adds `/api/pet/voice` (local whisper transcription).
- `CHIPS.md`, `BUILD-PLAN.md`, `NOTES.md`: intent, roadmap, install/test checklist.

## Status (2026-11-17)
Done and committed, desktop-tested:
- v0.3: Boot / Navi Home / Chip Folder / Battle Chip In screens matching the mockups (Departure Mono 11px, pixel icons, chip art from `chips.html`).
- v0.4: `.deb` packaging (`/etc/alicepet.conf` conffile), Talk-to-Alice chat on Home, NET SCAN (`[1]`).
- v0.5 step 1: sleep (ESC / idle), chip-module framework.
- v0.5 step 2: VOICE chip push-to-talk (arecord -> bridge -> whisper -> pet session), TTS hook off by default.
- v0.5 step 3a: TERMINAL chip (SSH bookmarks, pty ssh + VT100 view, Ask-Alice command suggestions that only run on Enter).
- v0.5 step 3b: JELLYFIN chip (library / seasons / episodes via API, HLS H.264 transcode, mpv IPC). Desktop playback is stubbed.

- Review fixes (2026-11-17): Ask-Alice shows the whole suggested command (up to 3 lines) and refuses Enter on anything longer (E types it only); into a new SSH session it is typed, never run, and only once a shell prompt shows. Prompts sent to the bridge put instructions first and stay under its 600-character cut. Status-bar sysfs reads are throttled to every 10 s and skipped while napping. SIGPIPE is ignored, child processes are spawned fork-safely, key releases can't be lost, the Jellyfin API key reaches mpv as a header from a 0600 file, and the .deb recommends alsa-utils, openssh-client, mpv and curl. Version is 0.5.0.
- Ask-Alice restyle + power pass (2026-11-17): Ask-Alice matches `mockups/comms.html` screen 5 (question, pink Alice line, boxed command, Run/Edit/No pills; red box and "E Type it" when too long). Power: backlight dims after `[power] dim_after_s` (45 s, to `dim_percent` 15%) before the `sleep_after_s` nap; the main loop sleeps until LVGL's next timer (5-33 ms) instead of every 5 ms, and while napping waits in `poll()` on the key devices (1 s timeout) instead of waking every 40 ms. An open SSH shell only holds the pet awake while output arrived in the last 30 s, an Ask screen only while waiting on Alice, and Jellyfin only while playing (not paused). Leaving Jellyfin stops mpv.
- Chips round (2026-11-17, branch feat/next-chips): LAN TECH screens (mockups/tools.html 5-7) wired into the folder; REMOTE chip (tools.html 1-4) learns and sends IR with LIRC `ir-ctl`, stores remotes/macros as text in `[remote] dir`; RETRO chip (media.html) browses RomM, downloads to `[retro] dir` and runs RetroArch with a per-platform libretro core. Per-chip power profiles (`src/platform/power.cpp`, `model/power_profile.h`) drive CPU cores, the CPU clock cap while napping, Wi-Fi power save, Bluetooth and the ACT LED. Personal hosts, bridge and Jellyfin server moved out of the shipped config into `alicepet.local.conf` (git-ignored, read after the main file).
- Device day (branch feat/device-day): `bin/device-day.sh` runs on the PET (`sudo bash device-day.sh`, or `--auto` to skip interactive steps) and writes a Markdown report with PASS/FAIL/CHECK per item plus a fuel-gauge power baseline. RETRO syncs saves with RomM: pulls a newer save before play (old one kept as .bak), uploads a changed one after (`[retro] sync_saves`, RetroArch `-s {savedir}`). https request bodies now go to curl through a temp file, so binary uploads work.
- LAN TECH extras (branch feat/lan-tech-extras): after a good jack test a down/up speed test runs (`[lantech] speed_test`, Cloudflare URLs by default) and is added to the Internet row. [V] tries temporary static addresses when DHCP fails (`static=cidr|gw` lines; needs root for `ip addr add`, removed on retest or leaving). [S] saves a text report to `[lantech] report_dir`. [W] in Devices shows only devices first seen in the last 24 h (known MACs per network in `known_dir`; the first sweep of a network is a baseline). [↵] on a device pings it and checks 16 common ports.
- JACK IN first cut (branch feat/jack-in): slotting the chip runs `[jackin] gadget_up` (`alicepet/jackin/usb-gadget.sh up` via sudo: read-only drive with the Windows app + RNDIS/ECM network, PET 10.55.0.1, PC 10.55.0.2 by dnsmasq with no gateway/DNS). The Windows app (`alicepet/jackin/windows/JackIn.ps1`, PowerShell 5.1 + WinForms, started by JackIn.cmd) dials the PET (nothing listens on the PC), starts read-only and sends a scan; Alice suggests one PowerShell fix that [↵] sends, run only while Full Access is on. Full Access, Request Admin (UAC relaunch) and Share PC Internet are clicked on the PC; the PET can only ask ([F] [A] [N]), take access away, or [X] JACK OUT. Sharing is a reverse tunnel: an HTTP proxy on PET 127.0.0.1:3128 whose upstream sockets the PC dials, with `http(s)_proxy` set for the app's children. Protocol: `alicepet/jackin/PROTOCOL.md`.
- Tests: `alicepet/tests/viewmodel_tests.cpp` (no framework) builds with every desktop configure; run `ctest --test-dir alicepet/build/<preset> -C Debug`. App code builds with `-Wall -Wextra` and no warnings.
- Linux desktop build works too (`linux-x86-64` preset). LVGL's draw thread now has a 128 KB stack: FreeType overflowed the old 32 KB one on Ubuntu.

In progress / not done:
- JACK IN: nothing proven on hardware. Check: the CM0 port does device mode (`dtoverlay=dwc2,dr_mode=otg`), `usb-gadget.sh up` works and the USB-A/Ethernet side comes back after `down`; install `alicepet-jackin.sudoers` for the app user; Windows binds RNDIS from the OS descriptors; the PowerShell app has never been run (no Windows or PowerShell here), so expect fixes on first launch. macOS/Linux apps are not written yet.
- RETRO save sync is written against RomM's current API (POST /api/saves with `saveFile`, GET /api/saves?rom_id=, GET /api/saves/{id}/content) and tested with a stand-in server; check it against Holden's RomM version and that the token has assets.read + assets.write.
- Power profiles: written and unit-tested against a fake sysfs, but off (`[power] profiles=off`) until checked on the board. USB hub/Ethernet, audio codec, HDMI and IR power switches are not found yet.
- Nothing has run on real hardware yet. The device arrives 2026-11-18. Expect to fix evdev key mapping, the backlight dim level (`dim_percent`), (`src/platform/linux_input.cpp`, check with `evtest`), font sizes, backlight path, and ALSA device names. Also check: which user APPLaunch runs the app as (needs `/dev/input`, backlight write, and read access to `/etc/alicepet.conf`); whether mpv's `--vo=drm` picks the panel or HDMI (may need `--drm-device=`); `--hwdec=v4l2m2m-copy` for hardware H.264; the panel's color depth vs LVGL's 32-bit config; and that Jellyfin playback works with the key sent as a header.
- v0.5 device build/package not re-verified after steps 2-3.

## Secrets
None in the repo. Device secrets go in `/etc/alicepet.conf` (`token=`, Jellyfin `api_key=`) and are set on the device only. Never commit them.

## Desktop build
See `alicepet/README.md` and `CMakePresets.json` (desktop preset). Screenshots are in `alicepet/screenshots/`.
