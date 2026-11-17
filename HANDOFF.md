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

In progress / not done:
- v0.5 step 3c: **LAN TECH chip is WIP and may not compile** (last commit). Finish or stub it first.
- JACK IN, REMOTE, RETRO chip screens: still "Coming soon".
- Nothing has run on real hardware yet. The device arrives 2026-11-18. Expect to fix evdev key mapping (`src/platform/linux_input.cpp`, check with `evtest`), font sizes, backlight path, and ALSA device names.
- v0.5 device build/package not re-verified after steps 2-3.

## Secrets
None in the repo. Device secrets go in `/etc/alicepet.conf` (`token=`, Jellyfin `api_key=`) and are set on the device only. Never commit them.

## Desktop build
See `alicepet/README.md` and `CMakePresets.json` (desktop preset). Screenshots are in `alicepet/screenshots/`.
