/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "app.h"

#include "asset_manager.h"
#include "linux_input.h"
#include "logger.h"
#include "screenshot_service.h"
#include "theme.h"
#include "alice_view.h"
#include "alice_viewmodel.h"
#include "model/chip_registry.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

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

// Maps platform keys to the view model's vocabulary. Returns false for keys
// AlicePET does not use so LVGL can still deliver them.
bool map_key(uint32_t key, viewmodel::Key& out) {
    switch (key) {
    case LV_KEY_LEFT:  out = viewmodel::Key::Left; return true;
    case LV_KEY_RIGHT: out = viewmodel::Key::Right; return true;
    case LV_KEY_ENTER: out = viewmodel::Key::Enter; return true;
    case LV_KEY_ESC:   out = viewmodel::Key::Escape; return true;
    case '1':          out = viewmodel::Key::Digit1; return true;
    case '2':          out = viewmodel::Key::Digit2; return true;
    case '3':          out = viewmodel::Key::Digit3; return true;
    default:           return false;
    }
}

// Routes global key presses into the view model.
class KeyRouter {
public:
    explicit KeyRouter(viewmodel::AliceViewModel& vm) : vm_(vm) {
        platform::set_global_key_listener(listener, this);
    }

    ~KeyRouter() {
        platform::clear_global_key_listener(listener, this);
    }

private:
    static bool listener(uint32_t key, const char*, bool long_pressed, void* user_data) {
        auto* self = static_cast<KeyRouter*>(user_data);
        viewmodel::Key mapped{};
        if (!self || long_pressed || !map_key(key, mapped)) {
            return false;
        }
        self->vm_.handle_key(mapped);
        return true;
    }

    viewmodel::AliceViewModel& vm_;
};

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
                  lv_display_t* display) {
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

    viewmodel::AliceViewModel view_model;
    view::AliceView view(root, alice_fonts, assets.resolve("images/chips"));
    KeyRouter keys(view_model);

    LOG_INFO("AlicePET started at {}x{}", lv_display_get_horizontal_resolution(display),
             lv_display_get_vertical_resolution(display));

#if USE_DESKTOP
    if (const char* snapshot_dir = std::getenv("ALICEPET_SNAPSHOT")) {
        std::filesystem::create_directories(snapshot_dir);
        run_snapshot(snapshot_dir, view_model, view, display);
        return 0;
    }
#endif

    uint32_t last_ms = lv_tick_get();
    bool running = true;
    while (running
#if USE_DESKTOP
           && simulator_frame.process_events()
#endif
    ) {
        const uint32_t now_ms = lv_tick_get();
        view_model.tick(now_ms - last_ms);
        last_ms = now_ms;

        view.render(view_model);
        view.update_clock();
        lv_timer_handler();
        lv_delay_ms(5);
    }

    return 0;
}

} // namespace app
