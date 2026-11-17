/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "app.h"

#include "asset_manager.h"
#include "backlight.h"
#include "linux_input.h"
#include "logger.h"
#include "screenshot_service.h"
#include "theme.h"
#include "alice_view.h"
#include "alice_viewmodel.h"
#include "model/chip_registry.h"
#include "model/pet_config.h"
#include "pet_services.h"

#include <algorithm>
#include <chrono>
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
    view_model.set_chat_available(chat.configured());
    view_model.set_hosts(host_rows(pet_config));
    view::AliceView view(root, alice_fonts, assets.resolve("images/chips"));
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
    bool running = true;
    bool screen_on = true;
    while (running
#if USE_DESKTOP
           && simulator_frame.process_events()
#endif
    ) {
        const uint32_t now_ms = lv_tick_get();
        view_model.tick(now_ms - last_ms);
        last_ms = now_ms;
        pump_services(view_model, chat, scanner);

        // Sleep: backlight off on the device (the overlay covers the desktop).
        if (view_model.sleeping() == screen_on) {
            screen_on = !view_model.sleeping();
            const bool switched = platform::set_screen_power(screen_on, pet_config.backlight);
            LOG_INFO("screen {} (backlight {})", screen_on ? "on" : "off", switched ? "switched" : "n/a");
        }

        view.render(view_model);
        view.update_clock();
        lv_timer_handler();
        // Napping: poll keys less often to save power.
        lv_delay_ms(screen_on ? 5 : 40);
    }

    return 0;
}

} // namespace app
