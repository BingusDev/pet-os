# AlicePET app

The C++ / LVGL 9.5 app that runs on the M5Stack CardputerZero (320x170). See the
[top-level README](../README.md) for what AlicePET is and [HANDOFF.md](../HANDOFF.md)
for current status. This file covers building, running and the code layout.

## Layout

```text
src/
  main.cpp            entry point -> app::Application::run()
  app/                lifecycle and main loop (app.cpp), asset lookup, Home chat + NET SCAN
                      workers (pet_services), desktop simulator window
  model/              config parsing (pet_config), chip table and art (chip_registry)
  viewmodel/          LVGL-free state machines: AliceViewModel (boot, Home, folder,
                      NET SCAN, sleep) and one ChipModule per chip in chips/
  view/               LVGL screens; one ChipView per chip in chips/
  platform/           Linux and POSIX services: evdev keys, backlight, sysfs status,
                      HTTP client (https via curl), JSON, child processes and ptys, mpv IPC
  config/             LVGL configs (lv_conf_desktop.h, lv_conf_cm0.h)
  logger/             fmt-based logger
config/alicepet.conf  installed as /etc/alicepet.conf (a dpkg conffile)
tests/                view-model unit tests (no framework; run by ctest)
```

Data flow: keys go through `KeyRouter` (app.cpp) into `AliceViewModel`, which
forwards them to the active chip. Network and process work runs on detached
worker threads (`Mailbox<T>` in `viewmodel/chip_module.h`) and is drained on the
UI thread in `tick()`. `AliceView::render()` redraws only when the view model's
revision changes.

## Desktop simulator

Needs CMake 3.31+, Ninja, and SDL2, FreeType, libpng, libjpeg, zlib and fmt.

```sh
# macOS (Apple Silicon)
cmake --preset darwin-arm64
cmake --build --preset darwin-arm64-rel
./build/darwin-arm64/Release/alicepet

# Linux
cmake --preset linux-x86-64
cmake --build --preset linux-x86-64-dbg
./build/linux-x86-64/Debug/alicepet
```

Unit tests build with every desktop configure:

```sh
ctest --test-dir build/linux-x86-64 -C Debug   # or run build/<preset>/<Config>/alicepet_tests
```

### Snapshot mode

`ALICEPET_SNAPSHOT=<dir>` runs a fixed key script on a virtual clock and writes
320x170 PNGs instead of opening the UI loop. Optional extras:

| Variable | Adds |
| --- | --- |
| `ALICEPET_CONFIG=<file>` | use this config instead of `config/alicepet.conf` |
| `ALICEPET_SNAPSHOT_CHAT="hello"` | Home typing, thinking and reply pages (real bridge) |
| `ALICEPET_SNAPSHOT_NETSCAN=1` | NET SCAN results (real network) |
| `ALICEPET_VOICE_WAV=<wav>` | VOICE listening and answer (real bridge) |
| `ALICEPET_SNAPSHOT_SSH=<label>` | a real SSH session to that bookmark |
| `ALICEPET_SNAPSHOT_ASK_CANNED="CMD: ...\nWHY: ..."` | the Ask-Alice suggestion screen with a fixed reply |
| `ALICEPET_SNAPSHOT_JF_SERIES=<search>` | Jellyfin episodes, resume and playing screens |
| `ALICEPET_SNAPSHOT_ALL_CHIPS=1` | every chip in the folder and slotted |
| `ALICEPET_SNAPSHOT_LAN=1` | LAN TECH jack test, diagnosis and device sweep (real network) |
| `ALICEPET_SNAPSHOT_JACK=1` | JACK IN waiting screen and a canned PC session (scan, Alice fix, admin, log, jack out) |
| `ALICEPET_SNAPSHOT_REMOTE=1` | every REMOTE screen (needs `ir-ctl` on PATH and a `[remote] dir`) |
| `ALICEPET_SNAPSHOT_RETRO=1` | RETRO library, details, download and playing (needs a RomM server) |

## Device build and install

```sh
cmake --workflow --preset cp0-cross-package   # on a Linux/arm64 or cross-capable host
# or, from the repo root: bin/build-device.sh alicepet --package
SKIP_BUILD=1 INSTALL=1 REMOTE_USER=<user> REMOTE_HOST=<pet> ./deploy.sh
```

The first configure downloads the CardputerZero BSP/SDK (about 435 MB) and LVGL
into `.cache/`. The .deb installs `/usr/bin/alicepet`, assets under
`/usr/share/alicepet/`, the APPLaunch entry and `/etc/alicepet.conf`. It
recommends `alsa-utils`, `openssh-client`, `mpv` and `curl`, which the VOICE,
TERMINAL and JELLYFIN chips and https requests use.

Useful CMake options: `APP_KEY_INPUT_DEVICE` (evdev path; empty scans
`/dev/input/event*`), `APP_FRAMEBUFFER_DEVICE` (default `/dev/fb0`),
`APP_USE_DRM` and `APP_DRM_DEVICE`, `ALICEPET_BUILD_TESTS`.

## Config

`/etc/alicepet.conf` (override with `ALICEPET_CONFIG=`). Sections: `[navi]`
your Navi's name and picture and your name, `[bridge]`
url/token, `[netscan]` hosts, `[power]` sleep, dim, backlight and power profiles,
`[voice]` recorder and TTS commands, `[terminal]` SSH bookmarks, `[jellyfin]`
server, key and player, `[remote]` IR devices and folder, `[romm]` server,
`[retro]` emulator and cores.

The shipped file has no personal hosts or servers. Put yours in
`alicepet.local.conf` next to it (`/etc/alicepet.local.conf` on the PET,
`config/alicepet.local.conf` for the simulator; git-ignored and not packaged).
It is read after the main file: its values win and its `host=` lines are added.
Secrets (`token=`, `api_key=`) go there too, then
`chmod 640 /etc/alicepet.local.conf`; the app's user must still be able to read it.

## License

MIT (see `LICENSE`). Based on the M5Stack CardputerZero app template.
