/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "app.h"

#include "asset_manager.h"
#include "backlight.h"
#include "power.h"
#include "model/power_profile.h"
#include "linux_input.h"
#include "logger.h"
#include "screenshot_service.h"
#include "theme.h"
#include "alice_view.h"
#include "alice_viewmodel.h"
#include "model/chip_registry.h"
#include "model/pet_config.h"
#include "pet_services.h"
#include "view/chips/jellyfin_view.h"
#include "view/chips/lan_tech_view.h"
#include "view/chips/remote_view.h"
#include "view/chips/retro_view.h"
#include "viewmodel/chips/retro_chip.h"
#include "viewmodel/chips/remote_chip.h"
#include "viewmodel/chips/lan_tech_chip.h"
#include "view/chips/terminal_view.h"
#include "viewmodel/chips/jellyfin_chip.h"
#include "view/chips/voice_view.h"
#include "viewmodel/chips/terminal_chip.h"
#include "viewmodel/chips/voice_chip.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>

#if USE_DESKTOP
#include "desktop_simulator_frame.h"
#endif

#if !USE_DESKTOP
#if APP_USE_DRM
#include "src/drivers/display/drm/lv_linux_drm.h"
#else
#include "src/drivers/display/fb/lv_linux_fbdev.h"
#endif
#endif

#ifndef APP_FRAMEBUFFER_DEVICE
#define APP_FRAMEBUFFER_DEVICE "/dev/fb0"
#endif

#ifndef APP_DRM_DEVICE
#define APP_DRM_DEVICE "/dev/dri/card0"
#endif

#ifndef APP_DRM_CONNECTOR_ID
#define APP_DRM_CONNECTOR_ID -1
#endif

namespace app {
namespace {

#ifndef APP_CONFIG_FILE
#define APP_CONFIG_FILE "/etc/alicepet.conf"
#endif

// Maps platform keys to the view model's vocabulary. Returns false for keys
// that are not navigation keys (printable text is handled separately).
bool map_key(uint32_t key, viewmodel::Key& out) {
    switch (key) {
    case LV_KEY_LEFT:      out = viewmodel::Key::Left; return true;
    case LV_KEY_RIGHT:     out = viewmodel::Key::Right; return true;
    case LV_KEY_UP:        out = viewmodel::Key::Up; return true;
    case LV_KEY_DOWN:      out = viewmodel::Key::Down; return true;
    case LV_KEY_ENTER:     out = viewmodel::Key::Enter; return true;
    case LV_KEY_ESC:       out = viewmodel::Key::Escape; return true;
    case LV_KEY_BACKSPACE: out = viewmodel::Key::Backspace; return true;
    case LV_KEY_DEL:       out = viewmodel::Key::Backspace; return true;
    default:               return false;
    }
}

// Printable keys arrive as their UTF-8 bytes packed little-endian into the key code
// (desktop SDL text input) or as plain ASCII (device evdev map).
bool key_to_utf8(uint32_t key, std::string& out) {
    if (key >= 0x20 && key < 0x7f) {
        out.assign(1, static_cast<char>(key));
        return true;
    }
    if (key < 0x80 || (key & platform::kSpecialKeyBase) == platform::kSpecialKeyBase) {
        return false;
    }
    out.clear();
    for (int i = 0; i < 4; ++i) {
        const char byte = static_cast<char>((key >> (8 * i)) & 0xff);
        if (byte == 0) break;
        out += byte;
    }
    return !out.empty();
}

// Routes global key presses into the view model.
class KeyRouter {
public:
    explicit KeyRouter(viewmodel::AliceViewModel& vm) : vm_(vm) {
        platform::set_global_key_listener(listener, this);
        platform::set_key_release_listener(release_listener, this);
    }

    ~KeyRouter() {
        platform::clear_global_key_listener(listener, this);
        platform::clear_key_release_listener(release_listener, this);
    }

private:
    static void release_listener(uint32_t key, const char*, void* user_data) {
        auto* self = static_cast<KeyRouter*>(user_data);
        if (self && key == ' ') {
            self->vm_.handle_key(viewmodel::Key::PushToTalkRelease);
        }
    }

    static bool listener(uint32_t key, const char*, bool long_pressed, void* user_data) {
        auto* self = static_cast<KeyRouter*>(user_data);
        if (!self) {
            return false;
        }
        if (long_pressed) {
            // Holding SPACE on Home = push-to-talk (VOICE takes a plain press itself).
            if (key == ' ') {
                if (self->vm_.screen() == viewmodel::Screen::Home) {
                    self->vm_.handle_key(viewmodel::Key::PushToTalk);
                }
                return true;
            }
            if (key == LV_KEY_ESC) {
                self->vm_.handle_key(viewmodel::Key::HoldEscape);
                return true;
            }
            return false;
        }
        if ((key & ~0xffu) == platform::kKeyCtrlBase) {
            self->vm_.handle_ctrl(static_cast<char>(key & 0xff));
            return true;
        }
        if (key == LV_KEY_NEXT) {
            self->vm_.handle_key(viewmodel::Key::Tab);
            return true;
        }
        viewmodel::Key mapped{};
        if (map_key(key, mapped)) {
            self->vm_.handle_key(mapped);
            return true;
        }
        std::string text;
        if (key_to_utf8(key, text)) {
            self->vm_.handle_char(text);
            return true;
        }
        return false;
    }

    viewmodel::AliceViewModel& vm_;
};

// Moves network results into the view model and starts requested work.
void pump_services(viewmodel::AliceViewModel& vm, ChatClient& chat, NetScanner& scanner) {
    std::string outgoing;
    if (vm.take_outgoing(outgoing)) {
        chat.send(outgoing);
    }
    ChatResult reply;
    while (chat.poll(reply)) {
        vm.on_chat_reply(reply.ok, reply.text);
    }

    if (vm.take_scan_request()) {
        scanner.start();
    }
    HostResult host;
    while (scanner.poll(host)) {
        viewmodel::HostState state = viewmodel::HostState::Offline;
        if (host.probe.state == platform::net::ProbeState::Online) state = viewmodel::HostState::Online;
        if (host.probe.state == platform::net::ProbeState::Rest) state = viewmodel::HostState::Rest;
        vm.on_host_result(host.index, state, host.probe.latency_ms, host.probe.detail);
    }
    if (vm.scanning() && !scanner.running()) {
        vm.on_scan_finished();
    }
}

std::vector<viewmodel::HostRow> host_rows(const model::PetConfig& config) {
    std::vector<viewmodel::HostRow> rows;
    for (const auto& host : config.hosts) {
        viewmodel::HostRow row;
        row.name = host.name;
        row.address = host.address;
        row.probe = host.probe_label();
        rows.push_back(row);
    }
    return rows;
}

// Chip modules + their screens. Chips not listed keep the BATTLE CHIP IN! placeholder.
volatile std::sig_atomic_t g_stop_requested = 0;
void on_stop_signal(int) { g_stop_requested = 1; }

void install_chips(viewmodel::AliceViewModel& vm, view::AliceView& view, const model::PetConfig& config,
                   lv_obj_t* root, const view::AliceFonts& fonts, const std::filesystem::path& art_dir) {
    const auto add = [&](const char* id, std::unique_ptr<viewmodel::ChipModule> module,
                         std::unique_ptr<view::ChipView> chip_view) {
        const size_t index = model::chip_index(id);
        vm.set_chip_module(index, std::move(module));
        view.add_chip_view(index, std::move(chip_view));
    };
    add("voice", std::make_unique<viewmodel::chips::VoiceChip>(config),
        std::make_unique<view::VoiceView>(root, fonts, art_dir));
    add("terminal", std::make_unique<viewmodel::chips::TerminalChip>(config),
        std::make_unique<view::TerminalView>(root, fonts));
    add("jellyfin", std::make_unique<viewmodel::chips::JellyfinChip>(config),
        std::make_unique<view::JellyfinView>(root, fonts, art_dir));
    add("remote", std::make_unique<viewmodel::chips::RemoteChip>(config),
        std::make_unique<view::RemoteView>(root, fonts));
    add("retro", std::make_unique<viewmodel::chips::RetroChip>(config),
        std::make_unique<view::RetroView>(root, fonts));
    add("lan-tech", std::make_unique<viewmodel::chips::LanTechChip>(config),
        std::make_unique<view::LanTechView>(root, fonts, art_dir));
}

#if !USE_DESKTOP
lv_display_t* init_device_display() {
#if APP_USE_DRM
    auto* display = lv_linux_drm_create();
    if (!display) {
        return nullptr;
    }

    if (lv_linux_drm_set_file(display, APP_DRM_DEVICE, APP_DRM_CONNECTOR_ID) != LV_RESULT_OK) {
        lv_display_delete(display);
        return nullptr;
    }

    platform::init_key_input(display);
    return display;
#else
    auto* display = lv_linux_fbdev_create();
    if (!display) {
        return nullptr;
    }

    if (lv_linux_fbdev_set_file(display, APP_FRAMEBUFFER_DEVICE) != LV_RESULT_OK) {
        lv_display_delete(display);
        return nullptr;
    }

    platform::init_key_input(display);
    return display;
#endif
}
#endif

#if USE_DESKTOP
// Headless snapshot mode: drives the view model on a virtual clock with a
// fixed key script and writes 1x PNGs into ALICEPET_SNAPSHOT.
// Each screenshot is taken immediately after a forced refresh, so the output
// does not depend on real time.
void run_snapshot(const std::string& dir, viewmodel::AliceViewModel& vm, view::AliceView& view,
                  lv_display_t* display, ChatClient& chat, NetScanner& scanner) {
    constexpr uint32_t kStepMs = 16;
    uint32_t virtual_now = 0;

    const auto advance = [&](uint32_t until_ms) {
        while (virtual_now < until_ms) {
            vm.tick(kStepMs);
            virtual_now += kStepMs;
        }
        view.render(vm);
        view.update_clock();
        lv_refr_now(display);
    };

    const auto capture = [&](const char* name) {
        std::string error;
        const std::string path = dir + "/" + name;
        if (platform::screenshot::save_active_screen_png(path, error)) {
            LOG_INFO("snapshot saved: {}", path);
        } else {
            LOG_WARN("snapshot {} failed: {}", path, error);
        }
    };

    advance(1000);
    capture("boot.png");

    advance(viewmodel::AliceViewModel::kBootDurationMs + 200);
    capture("home.png");

    vm.handle_key(viewmodel::Key::Digit2);
    advance(virtual_now + kStepMs);
    capture("folder.png");

    vm.handle_key(viewmodel::Key::Right);
    advance(virtual_now + kStepMs);
    capture("folder-next.png");

    vm.handle_key(viewmodel::Key::Enter);
    advance(virtual_now + kStepMs);
    capture("slotted.png");

    // Sleep: ESC from Home, then any key wakes back to Home.
    vm.handle_key(viewmodel::Key::Escape);
    vm.handle_key(viewmodel::Key::Escape);
    vm.handle_key(viewmodel::Key::Escape);
    advance(virtual_now + kStepMs);
    capture("sleep.png");
    vm.handle_char("q");
    advance(virtual_now + kStepMs);
    capture("sleep-woke.png");
    vm.handle_key(viewmodel::Key::Digit2);
    vm.handle_key(viewmodel::Key::Enter);
    advance(virtual_now + kStepMs);

    // Optional live checks (real network): ALICEPET_SNAPSHOT_CHAT="hello" types the
    // message on Home, sends it through the bridge and captures thinking + reply pages;
    // ALICEPET_SNAPSHOT_NETSCAN=1 opens NET SCAN and captures the finished scan.
    const auto wait_real = [&](auto done, int max_ms) {
        for (int waited = 0; waited < max_ms && !done(); waited += 100) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            pump_services(vm, chat, scanner);
            vm.tick(100);
        }
        pump_services(vm, chat, scanner);
        advance(virtual_now + kStepMs);
    };
    if (const char* message = std::getenv("ALICEPET_SNAPSHOT_CHAT")) {
        vm.handle_key(viewmodel::Key::Escape);
        vm.handle_key(viewmodel::Key::Escape);
        advance(virtual_now + kStepMs);
        for (const char* c = message; *c; ++c) {
            vm.handle_char(std::string(1, *c));
        }
        advance(virtual_now + kStepMs);
        capture("home-typing.png");
        vm.handle_key(viewmodel::Key::Enter);
        pump_services(vm, chat, scanner);
        wait_real([] { return false; }, 900);
        capture("home-thinking.png");
        wait_real([&] { return vm.chat_state() != viewmodel::ChatState::Thinking; }, 120000);
        capture("home-reply.png");
        for (size_t page = 1; page < vm.bubble_pages() && page < 4; ++page) {
            vm.handle_key(viewmodel::Key::Down);
            advance(virtual_now + kStepMs);
            char name[40];
            std::snprintf(name, sizeof(name), "home-reply-p%zu.png", page + 1);
            capture(name);
        }
    }
    if (std::getenv("ALICEPET_SNAPSHOT_NETSCAN")) {
        while (vm.screen() != viewmodel::Screen::Home) {
            vm.handle_key(viewmodel::Key::Escape);
        }
        vm.handle_key(viewmodel::Key::Digit1);
        pump_services(vm, chat, scanner);
        advance(virtual_now + kStepMs);
        capture("netscan-scanning.png");
        wait_real([&] { return !vm.scanning(); }, 15000);
        capture("netscan.png");
        vm.handle_key(viewmodel::Key::Down);
        vm.handle_key(viewmodel::Key::Down);
        vm.handle_key(viewmodel::Key::Down);
        vm.handle_key(viewmodel::Key::Down);
        vm.handle_key(viewmodel::Key::Enter);
        advance(virtual_now + kStepMs);
        capture("netscan-details.png");
        vm.handle_key(viewmodel::Key::Escape);
        vm.handle_key(viewmodel::Key::Escape);
    }

    // VOICE: idle, listening (live levels from the WAV), then the real bridge round trip
    // when ALICEPET_VOICE_WAV is set (needs bin/alicepet-bridge.sh running locally).
    const auto go_home = [&] {
        for (int i = 0; i < 8 && vm.screen() != viewmodel::Screen::Home; ++i) {
            vm.handle_key(i < 2 ? viewmodel::Key::HoldEscape : viewmodel::Key::Escape);
        }
    };
    go_home();
    vm.handle_key(viewmodel::Key::Digit3);
    advance(virtual_now + kStepMs);
    capture("voice.png");
    if (std::getenv("ALICEPET_VOICE_WAV")) {
        auto* voice = static_cast<viewmodel::chips::VoiceChip*>(vm.active_chip());
        vm.handle_key(viewmodel::Key::PushToTalk);
        advance(virtual_now + 1400);
        capture("voice-listening.png");
        vm.handle_key(viewmodel::Key::PushToTalkRelease);
        wait_real([] { return false; }, 700);
        capture("voice-sending.png");
        wait_real([&] { return voice->state() != viewmodel::chips::VoiceChip::State::Sending; }, 180000);
        capture("voice-answer.png");
        if (voice->speaking()) {  // [voice] tts_cmd set: let it finish instead of killing it on exit
            wait_real([&] { return !voice->speaking(); }, 30000);
        }
        if (voice->reply_pages() > 1) {
            vm.handle_key(viewmodel::Key::Down);
            advance(virtual_now + kStepMs);
            capture("voice-answer-p2.png");
        }
    }
    // TERMINAL: host list with live SSH-port probes; ALICEPET_SNAPSHOT_SSH=<label> also
    // opens a real ssh session there and runs `uptime`. The Ask-Alice suggestion uses
    // ALICEPET_SNAPSHOT_ASK_CANNED (a fixed reply, UI only) when set.
    go_home();
    using viewmodel::chips::TerminalChip;
    vm.open_chip(model::chip_index("terminal"));
    auto* term = static_cast<TerminalChip*>(vm.active_chip());
    const auto all_probed = [&] {
        for (const auto& h : term->hosts()) {
            if (h.reach == TerminalChip::Reach::Checking) return false;
        }
        return true;
    };
    wait_real(all_probed, 6000);
    capture("terminal-hosts.png");
    const auto type = [&](const std::string& value) {
        for (char c : value) vm.handle_char(std::string(1, c));
    };
    if (const char* ssh_label = std::getenv("ALICEPET_SNAPSHOT_SSH")) {
        for (size_t i = 0; i < term->hosts().size(); ++i) {
            if (term->hosts()[i].label == ssh_label) {
                while (term->selected() != i) vm.handle_key(viewmodel::Key::Down);
            }
        }
        vm.handle_key(viewmodel::Key::Enter);
        wait_real([] { return false; }, 6000);
        type("uptime");
        vm.handle_key(viewmodel::Key::Enter);
        wait_real([] { return false; }, 2500);
        capture("terminal-shell.png");
        vm.handle_key(viewmodel::Key::HoldEscape);
        advance(virtual_now + kStepMs);
        capture("terminal-hosts-connected.png");
        if (const char* canned = std::getenv("ALICEPET_SNAPSHOT_ASK_CANNED")) {
            vm.handle_char("a");
            type("how full is the disk");
            advance(virtual_now + kStepMs);
            capture("terminal-ask-typing.png");
            vm.handle_key(viewmodel::Key::Enter);
            viewmodel::chips::BridgeReply reply;
            reply.ok = true;
            reply.reply = canned;
            term->on_ask_reply(reply);
            advance(virtual_now + kStepMs);
            capture("terminal-ask.png");
            vm.handle_key(viewmodel::Key::Enter);  // Run (the user's confirmation)
            wait_real([] { return false; }, 3000);
            capture("terminal-shell-ran.png");
            vm.handle_key(viewmodel::Key::HoldEscape);
        }
        vm.handle_char("d");
    } else if (const char* canned = std::getenv("ALICEPET_SNAPSHOT_ASK_CANNED")) {
        // No SSH host: still capture the suggestion screen (UI only, nothing runs).
        vm.handle_char("a");
        type("how full is the disk");
        advance(virtual_now + kStepMs);
        capture("terminal-ask-typing.png");
        vm.handle_key(viewmodel::Key::Enter);
        advance(virtual_now + kStepMs);
        capture("terminal-ask-waiting.png");
        viewmodel::chips::BridgeReply reply;
        reply.ok = true;
        reply.reply = canned;
        term->on_ask_reply(reply);
        advance(virtual_now + kStepMs);
        capture("terminal-ask.png");
        vm.handle_key(viewmodel::Key::Escape);
    }
    // JELLYFIN (real server when [jellyfin] url/api_key are set): library, then
    // ALICEPET_SNAPSHOT_JF_SERIES=<search> opens that show's episodes, the resume
    // prompt and the (desktop-stubbed) playing screen.
    go_home();
    using viewmodel::chips::JellyfinChip;
    vm.open_chip(model::chip_index("jellyfin"));
    auto* jf = static_cast<JellyfinChip*>(vm.active_chip());
    wait_real([&] { return jf->view() != JellyfinChip::View::Loading; }, 15000);
    vm.handle_key(viewmodel::Key::Right);
    vm.handle_key(viewmodel::Key::Right);
    advance(virtual_now + kStepMs);
    capture("jellyfin.png");
    if (const char* series = std::getenv("ALICEPET_SNAPSHOT_JF_SERIES")) {
        vm.handle_char("/");
        type(series);
        vm.handle_key(viewmodel::Key::Enter);
        advance(virtual_now + kStepMs);
        capture("jellyfin-search.png");
        vm.handle_key(viewmodel::Key::Enter);
        wait_real([&] { return jf->view() == JellyfinChip::View::Episodes && !jf->episodes().empty(); }, 15000);
        for (size_t i = 0; i < jf->episodes().size(); ++i) {
            if (jf->episodes()[i].position_ticks > 0) {
                while (jf->episode() != i) vm.handle_key(viewmodel::Key::Down);
                break;
            }
        }
        advance(virtual_now + kStepMs);
        capture("jellyfin-episodes.png");
        vm.handle_key(viewmodel::Key::Enter);
        advance(virtual_now + kStepMs);
        if (jf->view() == JellyfinChip::View::Resume) capture("jellyfin-resume.png");
        if (jf->view() == JellyfinChip::View::Resume) vm.handle_key(viewmodel::Key::Enter);
        wait_real([&] { return jf->stream_info() != "requesting transcode..."; }, 20000);
        capture("jellyfin-playing.png");
        vm.handle_key(viewmodel::Key::Escape);
        wait_real([] { return false; }, 1500);  // let the transcode cleanup request go out
    }
    // REMOTE: needs `ir-ctl` on PATH (a stand-in script works on the desktop) and
    // a [remote] dir with saved remotes/macros; captures every REMOTE screen.
    if (std::getenv("ALICEPET_SNAPSHOT_REMOTE")) {
        using viewmodel::chips::RemoteChip;
        go_home();
        vm.open_chip(model::chip_index("remote"));
        auto* remote = static_cast<RemoteChip*>(vm.active_chip());
        advance(virtual_now + kStepMs);
        capture("remote-devices.png");
        vm.handle_key(viewmodel::Key::Enter);
        vm.handle_key(viewmodel::Key::Right);
        vm.handle_key(viewmodel::Key::Enter);
        wait_real([&] { return !remote->sending(); }, 6000);
        capture("remote-pad.png");
        vm.handle_char("l");
        advance(virtual_now + 500);
        capture("remote-learn-listening.png");
        wait_real([&] { return remote->learn_state() != RemoteChip::LearnState::Listening; }, 10000);
        capture("remote-learn-captured.png");
        vm.handle_key(viewmodel::Key::Escape);
        vm.handle_key(viewmodel::Key::Escape);
        vm.handle_char("m");
        advance(virtual_now + kStepMs);
        capture("remote-macros.png");
        vm.handle_key(viewmodel::Key::Enter);
        wait_real([&] { return remote->macro_step() >= 1; }, 6000);
        advance(virtual_now + 5000);
        capture("remote-macro-run.png");
        vm.handle_key(viewmodel::Key::Escape);
        vm.handle_key(viewmodel::Key::Escape);
        for (size_t i = 0; i < remote->devices().size(); ++i) vm.handle_key(viewmodel::Key::Down);
        vm.handle_key(viewmodel::Key::Enter);
        for (const char* c : {"P", "r", "o", "j", "e", "c", "t", "o", "r"}) vm.handle_char(c);
        advance(virtual_now + kStepMs);
        capture("remote-name.png");
        vm.handle_key(viewmodel::Key::Escape);
    }
    // RETRO against a RomM server ([romm] url/token): library, a system tab,
    // details, keep-offline download and the playing screen ([retro] player).
    if (std::getenv("ALICEPET_SNAPSHOT_RETRO")) {
        using viewmodel::chips::RetroChip;
        go_home();
        vm.open_chip(model::chip_index("retro"));
        auto* retro = static_cast<RetroChip*>(vm.active_chip());
        wait_real([&] { return retro->view() != RetroChip::View::Loading; }, 15000);
        vm.handle_key(viewmodel::Key::Right);
        advance(virtual_now + kStepMs);
        capture("retro-library.png");
        for (size_t i = 0; i < retro->platforms().size() && retro->platforms()[retro->platform()].label != "GBA"; ++i) {
            vm.handle_key(viewmodel::Key::Tab);
            wait_real([&] { return retro->view() != RetroChip::View::Loading; }, 15000);
        }
        vm.handle_key(viewmodel::Key::Right);
        advance(virtual_now + kStepMs);
        capture("retro-system.png");
        vm.handle_key(viewmodel::Key::Enter);
        wait_real([&] { bool good = false; return retro->save_text(good) != "checking RomM..."; }, 5000);
        capture("retro-details.png");
        vm.handle_char("o");
        advance(virtual_now + kStepMs);
        capture("retro-downloading.png");
        wait_real([&] { return retro->view() != RetroChip::View::Downloading; }, 30000);
        capture("retro-details-offline.png");
        vm.handle_key(viewmodel::Key::Enter);
        wait_real([&] { return retro->view() != RetroChip::View::Details && retro->view() != RetroChip::View::Downloading; }, 10000);
        capture("retro-playing.png");
        wait_real([&] { return retro->view() != RetroChip::View::Playing; }, 15000);
        wait_real([&] { bool good = false; return retro->save_text(good) != "uploading save..."; }, 5000);
        capture("retro-after.png");
        vm.handle_key(viewmodel::Key::Escape);
        vm.handle_key(viewmodel::Key::Escape);
    }
    // LAN TECH (real network): jack test, Alice's diagnosis (canned text) and the
    // device sweep of this /24.
    if (std::getenv("ALICEPET_SNAPSHOT_LAN")) {
        using viewmodel::chips::LanTechChip;
        go_home();
        vm.open_chip(model::chip_index("lan-tech"));
        auto* lan = static_cast<LanTechChip*>(vm.active_chip());
        advance(virtual_now + kStepMs);
        capture("lan-testing.png");
        wait_real([&] { return !lan->running(); }, 20000);
        capture("lan-test.png");
        wait_real([&] { return !lan->speed_running(); }, 25000);
        vm.handle_char("s");
        advance(virtual_now + kStepMs);
        capture("lan-test-speed.png");
        viewmodel::chips::BridgeReply reply;
        reply.ok = true;
        reply.reply = "Link and address look fine, so the cable and jack are good. If something still feels slow, "
                      "it's past this jack: check the switch uplink or the router.";
        vm.handle_char("a");
        lan->on_alice_reply(reply);
        advance(virtual_now + kStepMs);
        capture("lan-alice.png");
        vm.handle_key(viewmodel::Key::Escape);
        vm.handle_char("d");
        advance(virtual_now + kStepMs);
        capture("lan-scanning.png");
        wait_real([&] { return !lan->sweeping(); }, 30000);
        capture("lan-devices.png");
        vm.handle_char("w");
        advance(virtual_now + kStepMs);
        capture("lan-whos-new.png");
        vm.handle_char("w");
        vm.handle_key(viewmodel::Key::Enter);
        wait_real([&] { return !lan->probing(); }, 15000);
        capture("lan-device.png");
        vm.handle_key(viewmodel::Key::Escape);
        vm.handle_key(viewmodel::Key::Escape);
    }
    go_home();
    vm.handle_key(viewmodel::Key::Digit2);
    vm.handle_key(viewmodel::Key::Enter);

    // Optional: every chip in the folder and slotted, for review sheets.
    if (std::getenv("ALICEPET_SNAPSHOT_ALL_CHIPS")) {
        std::filesystem::create_directories(dir + "/chips");
        vm.handle_key(viewmodel::Key::Escape);
        vm.handle_key(viewmodel::Key::Left);
        for (size_t i = 0; i < model::chip_count(); ++i) {
            char name[48];
            advance(virtual_now + kStepMs);
            std::snprintf(name, sizeof(name), "chips/folder-%02zu.png", i + 1);
            capture(name);
            vm.handle_key(viewmodel::Key::Enter);
            advance(virtual_now + kStepMs);
            std::snprintf(name, sizeof(name), "chips/slotted-%02zu.png", i + 1);
            capture(name);
            vm.handle_key(viewmodel::Key::Escape);
            vm.handle_key(viewmodel::Key::Right);
        }
    }
}
#endif

} // namespace

int Application::run() {
    // Writing to a pipe or socket whose reader is gone (TTS, mpv IPC, a dropped
    // connection) must fail with EPIPE, not kill the whole PET.
    std::signal(SIGPIPE, SIG_IGN);

    logger::Logger::init();
    logger::Logger::set_tag("alicepet");

    lv_init();

    AssetManager assets;
    for (const auto& root : assets.roots()) {
        LOG_INFO("asset root: {}", root.string());
    }

#if USE_DESKTOP
    DesktopSimulatorFrame simulator_frame(assets);
    auto* display = simulator_frame.display();
#else
    auto* display = init_device_display();
#endif
    if (!display) {
        LOG_ERROR("failed to initialize display");
        return 1;
    }

    auto* standard_font = assets.load_standard_font(14);
    view::apply_lvgl_theme(display, false, standard_font ? standard_font : LV_FONT_DEFAULT);
    // Departure Mono is drawn on an 11 px grid; only 11/22/33 px render without blur.
    view::AliceFonts alice_fonts{
        assets.load_standard_font(11),
        assets.load_standard_font(22),
        assets.load_standard_font(33),
        assets.load_standard_font(11),
    };
    if (!alice_fonts.tiny) {
        alice_fonts = {LV_FONT_DEFAULT, LV_FONT_DEFAULT, LV_FONT_DEFAULT, LV_FONT_DEFAULT};
    }

    lv_obj_t* root = lv_screen_active();
    lv_obj_set_style_bg_color(root, lv_color_hex(0x061633), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(root, 0, 0);

    // Config: ALICEPET_CONFIG overrides the build-time path (/etc/alicepet.conf on device).
    std::string config_path = APP_CONFIG_FILE;
    if (const char* override_path = std::getenv("ALICEPET_CONFIG")) {
        config_path = override_path;
    }
    model::PetConfig pet_config;
    std::vector<std::string> config_warnings;
    if (model::load_pet_config(config_path, pet_config, &config_warnings)) {
        LOG_INFO("config: {} (bridge {}, {} hosts)", config_path,
                 pet_config.bridge_url.empty() ? "unset" : pet_config.bridge_url, pet_config.hosts.size());
    } else {
        LOG_WARN("config not found: {}", config_path);
    }
    for (const auto& warning : config_warnings) {
        LOG_WARN("config: {}", warning);
    }
    ChatClient chat(pet_config);
    NetScanner scanner(pet_config);

    viewmodel::AliceViewModel view_model;
    view_model.set_sleep_after_ms(static_cast<uint32_t>(std::max(0, pet_config.sleep_after_s)) * 1000u);
    view_model.set_dim_after_ms(static_cast<uint32_t>(std::max(0, pet_config.dim_after_s)) * 1000u);
    view_model.set_chat_available(chat.configured());
    view_model.set_hosts(host_rows(pet_config));
    const auto art_dir = assets.resolve("images/chips");
    view::AliceView view(root, alice_fonts, art_dir);
    install_chips(view_model, view, pet_config, root, alice_fonts, art_dir);
    KeyRouter keys(view_model);

    LOG_INFO("AlicePET started at {}x{}", lv_display_get_horizontal_resolution(display),
             lv_display_get_vertical_resolution(display));

#if USE_DESKTOP
    if (const char* snapshot_dir = std::getenv("ALICEPET_SNAPSHOT")) {
        std::filesystem::create_directories(snapshot_dir);
        run_snapshot(snapshot_dir, view_model, view, display, chat, scanner);
        return 0;
    }
#endif

    uint32_t last_ms = lv_tick_get();
    // APPLaunch stops the app with SIGTERM: leave the loop so the power profile
    // (offline cores, capped clock) is put back for whatever runs next.
    std::signal(SIGTERM, on_stop_signal);
    std::signal(SIGINT, on_stop_signal);
    bool screen_on = true;
    auto screen_level = platform::ScreenLevel::On;
    // Per-chip power profiles ([power] profiles=on): cores, CPU clock, Wi-Fi power save.
    platform::PowerManager power;
    if (pet_config.power_profiles) power.init();
    bool display_lent = false;
    while (!g_stop_requested
#if USE_DESKTOP
           && simulator_frame.process_events()
#endif
    ) {
        const uint32_t now_ms = lv_tick_get();
        view_model.tick(now_ms - last_ms);
        last_ms = now_ms;
        pump_services(view_model, chat, scanner);

        // Idle: dim the backlight first, then nap with it off (the overlay covers
        // the desktop). The backlight is the biggest single draw on the pet.
        const auto want_level = view_model.sleeping() ? platform::ScreenLevel::Off
                                : view_model.dimmed()  ? platform::ScreenLevel::Dim
                                                       : platform::ScreenLevel::On;
        if (want_level != screen_level) {
            screen_level = want_level;
            screen_on = screen_level != platform::ScreenLevel::Off;
            const bool switched =
                platform::set_screen_level(screen_level, pet_config.backlight, pet_config.dim_percent);
            LOG_INFO("screen {} (backlight {})",
                     screen_level == platform::ScreenLevel::On    ? "on"
                     : screen_level == platform::ScreenLevel::Dim ? "dim"
                                                                  : "off",
                     switched ? "switched" : "n/a");
        }

        if (pet_config.power_profiles) {
            const auto* open_chip = view_model.active_chip();
            const size_t open_index = open_chip ? model::chip_index(open_chip->id()) : model::kChipCount;
            power.apply(view_model.sleeping()                ? model::nap_profile()
                        : open_index < model::kChipCount     ? model::chip_profile(model::chip_at(open_index))
                                                             : model::launcher_profile());
        }

        // While the video player owns the panel, keep reading keys (lv_timer_handler)
        // but stop drawing; repaint everything when it hands the panel back.
        const auto* chip = view_model.active_chip();
        const bool external = chip && chip->owns_display();
        if (!external) {
            if (display_lent) {
                display_lent = false;
                lv_obj_invalidate(lv_screen_active());
            }
            view.render(view_model);
            // Napping: the backlight is off, so skip the status bar (and its sysfs reads).
            if (screen_on) view.update_clock();
        } else {
            display_lent = true;
        }
        const uint32_t next_timer_ms = lv_timer_handler();
        if (screen_on) {
            // Sleep until LVGL's next timer is due (indev read, redraw, animation),
            // between 5 ms and one 30 fps frame, instead of spinning every 5 ms.
            lv_delay_ms(std::clamp<uint32_t>(next_timer_ms, 5, 33));
        } else {
            // Napping: wait in the kernel for a key, waking once a second for workers.
            platform::wait_for_key_input(1000);
        }
    }

    if (pet_config.power_profiles) power.restore();
    return 0;
}

} // namespace app
