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
- Tests: `alicepet/tests/viewmodel_tests.cpp` (no framework) builds with every desktop configure; run `ctest --test-dir alicepet/build/<preset> -C Debug`. App code builds with `-Wall -Wextra` and no warnings.
- Linux desktop build works too (`linux-x86-64` preset). LVGL's draw thread now has a 128 KB stack: FreeType overflowed the old 32 KB one on Ubuntu.

In progress / not done:
- v0.5 step 3c: LAN TECH chip logic (`src/viewmodel/chips/lan_tech_chip.cpp`) compiles, but it has no view yet (`lan_tech_view.cpp` is missing) and `install_chips()` in `src/app/app.cpp` doesn't add it.
- JACK IN, REMOTE, RETRO chip screens: still "Coming soon".
- Nothing has run on real hardware yet. The device arrives 2026-11-18. Expect to fix evdev key mapping, the backlight dim level (`dim_percent`), (`src/platform/linux_input.cpp`, check with `evtest`), font sizes, backlight path, and ALSA device names. Also check: which user APPLaunch runs the app as (needs `/dev/input`, backlight write, and read access to `/etc/alicepet.conf`); whether mpv's `--vo=drm` picks the panel or HDMI (may need `--drm-device=`); `--hwdec=v4l2m2m-copy` for hardware H.264; the panel's color depth vs LVGL's 32-bit config; and that Jellyfin playback works with the key sent as a header.
- v0.5 device build/package not re-verified after steps 2-3.

## Secrets
None in the repo. Device secrets go in `/etc/alicepet.conf` (`token=`, Jellyfin `api_key=`) and are set on the device only. Never commit them.

## Desktop build
See `alicepet/README.md` and `CMakePresets.json` (desktop preset). Screenshots are in `alicepet/screenshots/`.
