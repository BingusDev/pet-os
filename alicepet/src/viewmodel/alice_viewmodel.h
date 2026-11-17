/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace viewmodel {

enum class Screen { Boot, Home, Folder, Slotted };

// Keys the view model understands, decoupled from LVGL / SDL key codes.
enum class Key { Left, Right, Enter, Escape, Digit1, Digit2, Digit3 };

// Pure state machine for AlicePET: boot timing, screen switching, folder
// carousel selection. The view observes it via accessors after each call.
class AliceViewModel {
public:
    static constexpr uint32_t kBootDurationMs = 2000;

    // Advances timers. Call with the elapsed milliseconds since the last tick.
    void tick(uint32_t elapsed_ms);
    void handle_key(Key key);

    Screen screen() const { return screen_; }
    bool sleeping() const { return sleeping_; }
    // 0..100 boot progress.
    int boot_progress() const { return boot_progress_; }
    size_t selected_chip() const { return selected_chip_; }
    // Changes whenever the visible state changes, so the view can skip redraws.
    uint32_t revision() const { return revision_; }

private:
    void show(Screen next);

    Screen screen_{Screen::Boot};
    uint32_t boot_elapsed_ms_{0};
    int boot_progress_{0};
    size_t selected_chip_{0};
    bool sleeping_{false};
    uint32_t revision_{0};
};

} // namespace viewmodel
