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
#include "viewmodel/pc_input.h"
#include "theme.h"
#include "alice_view.h"
#include "alice_viewmodel.h"
#include "model/chip_registry.h"
#include "model/pet_config.h"
#include "pet_services.h"
#include "view/chips/jack_in_view.h"
#include "view/chips/jellyfin_view.h"
#include "view/chips/lan_tech_view.h"
#include "view/chips/remote_view.h"
#include "view/chips/retro_view.h"
#include "viewmodel/chips/retro_chip.h"
#include "viewmodel/chips/remote_chip.h"
#include "viewmodel/chips/lan_tech_chip.h"
#include "viewmodel/chips/jack_in_chip.h"
#include "view/chips/terminal_view.h"
#include "viewmodel/chips/jellyfin_chip.h"
#include "view/chips/voice_view.h"
#include "view/widgets.h"
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

// True while a PC operates a chip through JACK IN (the PET panel shows the
// session view and the PET's own keys are kept off the PC's chip).
bool g_pc_operating = false;
// While a PC is jacked in, the PET panel turns off after a few seconds without a
// PET key press (the app keeps running at full speed for the link). Any PET key
// turns it back on; that key is swallowed.
constexpr uint32_t kJackBlankAfterMs = 10000;
uint32_t g_last_local_key_ms = 0;
bool g_jack_blanked = false;
viewmodel::chips::JackInChip* g_jack_keys = nullptr;

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
        g_last_local_key_ms = lv_tick_get();
        if (g_jack_blanked) {
            g_jack_blanked = false;
            return true;
        }
        if (!self) {
            return false;
        }
        if (g_pc_operating) {
            // The PC has the controls; [X] on the PET still jacks out.
            if (!long_pressed && (key == 'x' || key == 'X') && g_jack_keys) g_jack_keys->handle_char("x");
            return true;
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

// The PC mouse (Jack In mirror clicks). An LVGL pointer indev carries the
// latest state so any clickable LVGL widget reacts; the chips' lists are drawn
// from view-model state, so a release over a tap_target() row also moves the
// selection there and presses Enter.
struct PcPointer {
    lv_indev_t* indev{nullptr};
    int x{0};
    int y{0};
    bool pressed{false};
};
PcPointer g_pc_pointer;

void pc_pointer_read(lv_indev_t*, lv_indev_data_t* data) {
    data->point.x = g_pc_pointer.x;
    data->point.y = g_pc_pointer.y;
    data->state = g_pc_pointer.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

void pc_pointer_event(viewmodel::AliceViewModel& vm, const viewmodel::chips::JackInChip::Input& input) {
    if (!g_pc_pointer.indev) {
        g_pc_pointer.indev = lv_indev_create();
        lv_indev_set_type(g_pc_pointer.indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(g_pc_pointer.indev, pc_pointer_read);
    }
    if (input.x >= 0 && input.y >= 0) {
        g_pc_pointer.x = input.x;
        g_pc_pointer.y = input.y;
    }
    g_pc_pointer.pressed = input.down;
    if (input.down || input.x < 0 || input.y < 0) return;
    int steps = 0;
    bool horizontal = false;
    lv_obj_update_layout(lv_screen_active());
    if (!view::find_tap_target(lv_screen_active(), input.x, input.y, steps, horizontal)) return;
    const viewmodel::Key forward = horizontal ? viewmodel::Key::Right : viewmodel::Key::Down;
    const viewmodel::Key back = horizontal ? viewmodel::Key::Left : viewmodel::Key::Up;
    for (int i = 0; i < std::abs(steps) && i < 64; ++i) vm.handle_key(steps > 0 ? forward : back);
    vm.handle_key(viewmodel::Key::Enter);
}

// PC requests from the Jack In link, applied on the UI thread (viewmodel/pc_input):
// chip opens, then keys, characters and mouse events for the PC's chip. Each
// batch is logged so a PC input problem shows up in /tmp/alicepet.log.
void drain_pc_requests(viewmodel::AliceViewModel& vm) {
    const auto batch = viewmodel::drain_pc_requests(
        vm, [&vm](const viewmodel::chips::JackInChip::Input& input) { pc_pointer_event(vm, input); });
    if (batch.keys || batch.chars || batch.pointers || batch.dropped) {
        LOG_INFO("pc input: {} keys, {} chars, {} mouse to {}{}", batch.keys, batch.chars, batch.pointers,
                 batch.chip.empty() ? std::string("(no chip)") : batch.chip,
                 batch.dropped ? fmt::format(", {} dropped (no PC chip open)", batch.dropped) : std::string());
    }
}

// Captures the active LVGL screen for the PC mirror (about 12 fps while mirroring).
void mirror_frame(viewmodel::AliceViewModel& vm, uint32_t now_ms) {
    auto* jack = static_cast<viewmodel::chips::JackInChip*>(vm.chip_module(model::chip_index("jack-in")));
    if (!jack || !jack->wants_frame(now_ms)) return;
    lv_draw_buf_t* snapshot = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_ARGB8888);
    if (!snapshot) return;
    const auto* chip = vm.active_chip();
    jack->offer_frame(static_cast<const uint8_t*>(snapshot->data), snapshot->header.stride,
                      static_cast<int>(snapshot->header.w), static_cast<int>(snapshot->header.h),
                      chip ? chip->id() : "launcher");
    lv_draw_buf_destroy(snapshot);
}

// The PET panel while the PC operates a chip: the chip still renders on the LVGL
// screen (that is what the PC's mirror captures with lv_snapshot_take), but a
// full-panel Jack In card sits on lv_layer_top(), which snapshots of the screen
// never include. So the PC sees the chip and the PET shows who is driving it.
struct PcOverlay {
    lv_obj_t* root{nullptr};
    lv_obj_t* name{nullptr};
    lv_obj_t* host{nullptr};
    lv_obj_t* art_obj{nullptr};
    view::ImageAsset art;
    std::string chip;
    bool visible{false};
};
PcOverlay g_overlay;

void no_clicks(lv_obj_t* obj) {
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); ++i) no_clicks(lv_obj_get_child(obj, static_cast<int32_t>(i)));
}

void update_pc_overlay(viewmodel::AliceViewModel& vm, const view::AliceFonts& fonts,
                       const std::filesystem::path& art_dir) {
    using viewmodel::chips::JackInChip;
    auto* jack = static_cast<JackInChip*>(vm.chip_module(model::chip_index("jack-in")));
    const auto* active = vm.active_chip();
    const bool show = jack && jack->connected() && !jack->pc_chip().empty() && active &&
                      jack->pc_chip() == active->id();
    g_pc_operating = show;
    if (!show) {
        if (g_overlay.visible) {
            view::set_visible(g_overlay.root, false);
            g_overlay.visible = false;
            // The PC went away mid-chip: the PET goes back to its Jack In screen.
            if (jack && !jack->connected() && active && JackInChip::pc_capable(active->id())) {
                vm.open_chip(model::chip_index("jack-in"));
            }
        }
        return;
    }
    if (!g_overlay.root) {
        g_overlay.root = view::box(lv_layer_top(), 0, 0, 320, 170, view::kDeepBlue);
        view::add_grid(g_overlay.root);
        lv_obj_t* bar = view::box(g_overlay.root, 0, 0, 320, 16, view::kPanelBlue);
        view::text(bar, "JACK IN", fonts.tiny, view::kGold, 6, 2);
        view::text(bar, "PC SESSION", fonts.tiny, view::kCyan, 244, 2);
        view::text(g_overlay.root, "OPERATING", fonts.tiny, view::kSubtitle, 112, 36);
        g_overlay.name = view::text(g_overlay.root, "", fonts.body, view::kWhite, 112, 50);
        view::text(g_overlay.root, "from", fonts.tiny, view::kSubtitle, 112, 82);
        g_overlay.host = view::text(g_overlay.root, "", fonts.small, view::kCyan, 112, 96);
        lv_obj_t* foot = view::box(g_overlay.root, 0, 156, 320, 14, view::kPanelBlue);
        view::text(foot, "The PC has the controls  [X] jack out", fonts.tiny, view::kHintBlue, 6, 1);
    }
    if (g_overlay.chip != active->id()) {
        g_overlay.chip = active->id();
        if (g_overlay.art_obj) lv_obj_delete(g_overlay.art_obj);
        g_overlay.art_obj = nullptr;
        g_overlay.art = view::ImageAsset{};
        if (view::load_png(art_dir / (g_overlay.chip + "-big.png"), g_overlay.art)) {
            g_overlay.art_obj = view::image(g_overlay.root, g_overlay.art, 14, 26);
        }
        const size_t index = model::chip_index(g_overlay.chip.c_str());
        const std::string name = index < model::kChipCount ? model::chip_at(index).name : g_overlay.chip;
        lv_label_set_text(g_overlay.name, name.c_str());
    }
    const std::string host = jack->host().empty() ? "the PC" : jack->host();
    if (std::string(lv_label_get_text(g_overlay.host)) != host) lv_label_set_text(g_overlay.host, host.c_str());
    no_clicks(g_overlay.root);
    if (!g_overlay.visible) {
        view::set_visible(g_overlay.root, true);
        lv_obj_move_foreground(g_overlay.root);
        g_overlay.visible = true;
    }
    vm.hold_awake();  // napping would slow the PC's mirror to one frame a second
}

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
    add("jack-in", std::make_unique<viewmodel::chips::JackInChip>(config),
        std::make_unique<view::JackInView>(root, fonts));
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
                  lv_display_t* display, ChatClient& chat, NetScanner& scanner, const view::AliceFonts& fonts,
                  const std::filesystem::path& art_dir) {
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
    // JACK IN: waiting screen, then a canned PC session (scan, Alice's fix,
    // Full Access granted on the "PC", the fix run) and the session log.
    if (std::getenv("ALICEPET_SNAPSHOT_JACK")) {
        using viewmodel::chips::JackInChip;
        go_home();
        vm.open_chip(model::chip_index("jack-in"));
        auto* jack = static_cast<JackInChip*>(vm.active_chip());
        advance(virtual_now + kStepMs);
        capture("jack-waiting.png");
        jack->on_line(R"({"t":"hello","host":"LAPTOP-FRONT","os":"Win 11","user":"front-desk","admin":false,"full":false})");
        jack->on_line(R"({"t":"log","text":"Get-Process | sort WS -desc | select -f 5","ok":true})");
        jack->on_line(R"({"t":"log","text":"Get-CimInstance Win32_StartupCommand","ok":true})");
        jack->on_line(R"({"t":"log","text":"Get-MpComputerStatus","ok":true})");
        jack->on_line(R"({"t":"scan","cpu":78,"ram":91,"disk":"C:","disk_free":8,"startup":14,"defender":"off","defender_since":"2026-08-12","top":[{"name":"chrome","mb":3100},{"name":"OneDrive","mb":420}]})");
        viewmodel::chips::BridgeReply reply;
        reply.ok = true;
        reply.reply = "Chrome is using 3.1 GB and Defender has been off since Aug 12. Turn Defender back on first?\n"
                      "CMD: Set-MpPreference -DisableRealtimeMonitoring $false";
        jack->on_alice_reply(reply);
        advance(virtual_now + kStepMs);
        capture("jack-session.png");
        vm.handle_char("a");
        advance(virtual_now + kStepMs);
        capture("jack-admin-asked.png");
        jack->on_line(R"({"t":"state","full":true,"admin":true,"net":false})");
        vm.handle_key(viewmodel::Key::Enter);
        advance(virtual_now + kStepMs);
        capture("jack-running.png");
        jack->on_line(R"({"t":"result","id":1,"ok":true,"out":""})");
        jack->on_line(R"({"t":"state","full":true,"admin":true,"net":true})");
        advance(virtual_now + kStepMs);
        capture("jack-full.png");
        vm.handle_char("l");
        advance(virtual_now + kStepMs);
        capture("jack-log.png");
        vm.handle_char("l");
        // The PC opens JELLYFIN: it renders for the PC's mirror (jack-pc-jellyfin)
        // while the PET panel shows the Jack In card on the top layer (jack-pet-operating).
        jack->on_line(R"({"t":"open","chip":"jellyfin"})");
        drain_pc_requests(vm);
        update_pc_overlay(vm, fonts, art_dir);
        advance(virtual_now + kStepMs);
        capture("jack-pc-jellyfin.png");
        {
            std::string error;
            if (!platform::screenshot::save_object_png(g_overlay.root, dir + "/jack-pet-operating.png", error)) {
                LOG_WARN("overlay snapshot failed: {}", error);
            }
        }
        // PC input reaches the PC-opened chip while the PET shows the OPERATING card
        // (jack-pc-<chip>-before/-after differ).
        jack->on_line(R"({"t":"state","full":true,"admin":true,"net":true,"input":true,"mode":"ask"})");
        for (const char* chip : {"lan-tech", "retro", "terminal", "jellyfin"}) {
            jack->on_line(std::string(R"({"t":"open","chip":")") + chip + "\"}");
            drain_pc_requests(vm);
            update_pc_overlay(vm, fonts, art_dir);
            advance(virtual_now + 400);
            capture((std::string("jack-pc-") + chip + "-before.png").c_str());
            jack->capture_sent = true;
            jack->on_line(R"({"t":"mirror","on":true})");
            mirror_frame(vm, virtual_now);
            const size_t first_bands = jack->sent.size();
            jack->sent.clear();
            for (const char* line : {R"({"t":"key","k":"Down"})", R"({"t":"key","k":"Down"})",
                                     R"({"t":"key","k":"Right"})", R"({"t":"char","c":"a"})",
                                     R"({"t":"pointer","x":160,"y":120,"b":"down"})",
                                     R"({"t":"pointer","x":160,"y":120,"b":"up"})"}) {
                jack->on_line(line);
            }
            drain_pc_requests(vm);
            update_pc_overlay(vm, fonts, art_dir);
            advance(virtual_now + 400);
            capture((std::string("jack-pc-") + chip + "-after.png").c_str());
            mirror_frame(vm, virtual_now);
            LOG_INFO("snapshot mirror {}: {} bands first, {} after PC input", chip, first_bands, jack->sent.size());
            jack->sent.clear();
            jack->capture_sent = false;
        }
        jack->on_line(R"({"t":"home"})");
        drain_pc_requests(vm);
        update_pc_overlay(vm, fonts, art_dir);
        advance(virtual_now + kStepMs);
        vm.handle_char("x");
        advance(virtual_now + kStepMs);
        capture("jack-out.png");
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
#if !USE_DESKTOP
    // stdout goes to APPLaunch (a socket nobody reads): keep a readable copy.
    logger::Logger::set_file("/tmp/alicepet.log", 512 * 1024);
#endif

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
    // The fbdev/DRM drivers give LVGL a clock but no delay, so lv_delay_ms()
    // busy-spun the main loop at 100% of a core. Sleep in the kernel instead.
    lv_delay_set_cb([](uint32_t ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); });
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
    if (auto* jack = static_cast<viewmodel::chips::JackInChip*>(view_model.chip_module(model::chip_index("jack-in")))) {
        jack->set_art_dir(art_dir);
        g_jack_keys = jack;
    }
    KeyRouter keys(view_model);

    LOG_INFO("AlicePET started at {}x{}", lv_display_get_horizontal_resolution(display),
             lv_display_get_vertical_resolution(display));

#if USE_DESKTOP
    if (const char* snapshot_dir = std::getenv("ALICEPET_SNAPSHOT")) {
        std::filesystem::create_directories(snapshot_dir);
        run_snapshot(snapshot_dir, view_model, view, display, chat, scanner, alice_fonts, art_dir);
        return 0;
    }
#endif

    uint32_t last_ms = lv_tick_get();
    // APPLaunch stops the app with SIGTERM: leave the loop so the power profile
    // (offline cores, capped clock) is put back for whatever runs next.
    std::signal(SIGTERM, on_stop_signal);
    std::signal(SIGINT, on_stop_signal);
    std::signal(SIGHUP, on_stop_signal);
    // A run that was killed or crashed while dimmed or napping leaves the panel
    // dark: start from a lit screen whatever the last run left behind.
    platform::set_screen_level(platform::ScreenLevel::On, pet_config.backlight, pet_config.dim_percent);
    bool jack_was_linked = false;
    bool screen_on = true;
    auto screen_level = platform::ScreenLevel::On;
    // Per-chip power profiles ([power] profiles=on): cores, CPU clock, Wi-Fi power save.
    platform::PowerManager power;
    if (pet_config.power_profiles) power.init();
    bool display_lent = false;
    bool jack_mirroring = false;
    while (!g_stop_requested
#if USE_DESKTOP
           && simulator_frame.process_events()
#endif
    ) {
        const uint32_t now_ms = lv_tick_get();
        view_model.tick(now_ms - last_ms);
        last_ms = now_ms;
        pump_services(view_model, chat, scanner);
        drain_pc_requests(view_model);
        update_pc_overlay(view_model, alice_fonts, art_dir);

        // Idle: dim the backlight first, then nap with it off (the overlay covers
        // the desktop). The backlight is the biggest single draw on the pet.
        {
            const auto* jack_chip = static_cast<const viewmodel::chips::JackInChip*>(
                view_model.chip_module(model::chip_index("jack-in")));
            const bool linked = jack_chip && jack_chip->connected();
            if (linked && !jack_was_linked) g_last_local_key_ms = now_ms;  // countdown starts at link
            jack_was_linked = linked;
            jack_mirroring = linked && jack_chip->mirroring();
            g_jack_blanked = linked && now_ms - g_last_local_key_ms >= kJackBlankAfterMs;
        }
        const auto want_level = (view_model.sleeping() || g_jack_blanked) ? platform::ScreenLevel::Off
                                : view_model.dimmed()                      ? platform::ScreenLevel::Dim
                                                                           : platform::ScreenLevel::On;
        if (want_level != screen_level) {
            screen_level = want_level;
            // Blanked for a jack-in link: the panel is off but the loop keeps full speed.
            screen_on = screen_level != platform::ScreenLevel::Off || g_jack_blanked;
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
            view.update_volume_osd(now_ms);
        } else {
            display_lent = true;
        }
        const uint32_t next_timer_ms = lv_timer_handler();
        if (!external) mirror_frame(view_model, now_ms);
        if (screen_on) {
            // Sleep until LVGL's next timer is due (indev read, redraw, animation),
            // between 5 ms and one 30 fps frame, instead of spinning every 5 ms.
            // Sleep in the kernel directly: on the device LVGL's delay hook ended up
            // unset at runtime, so lv_delay_ms() busy-spun a whole core (rev15 fix).
            // A blanked jack-in panel needs no 30 fps: a slower loop still serves
            // the PC link, the mirror (12 fps cap) and input.
            if (g_jack_blanked && !jack_mirroring) {
                // Blanked and the PC isn't watching: 10 Hz is plenty for the link
                // (its socket buffers meanwhile); a key wakes the loop at once.
                platform::wait_for_key_input(100);
            } else {
                const uint32_t floor_ms = g_jack_blanked ? 20 : 5;
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(std::clamp<uint32_t>(next_timer_ms, floor_ms, 33)));
            }
        } else {
            // Napping: wait in the kernel for a key, waking once a second for workers.
            platform::wait_for_key_input(1000);
        }
    }

    // Stopped while dimmed or napping: hand the panel back lit.
    if (screen_level != platform::ScreenLevel::On) {
        platform::set_screen_level(platform::ScreenLevel::On, pet_config.backlight, pet_config.dim_percent);
    }
    if (pet_config.power_profiles) power.restore();
    return 0;
}

} // namespace app
