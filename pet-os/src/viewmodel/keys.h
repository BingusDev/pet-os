/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>

namespace viewmodel {

// Keys the view models understand, decoupled from LVGL / SDL / evdev codes.
// Printable characters go through handle_char() instead.
enum class Key {
    Left,
    Right,
    Up,
    Down,
    Enter,
    Escape,
    Backspace,
    Tab,
    Digit1,
    Digit2,
    Digit3,
    PushToTalk,         // SPACE held (long press on Home, plain press inside VOICE)
    PushToTalkRelease,  // SPACE released after PushToTalk
    HoldEscape,         // ESC held ~0.7 s: leave a chip that captures ESC (terminal shell)
    Ctrl,               // Ctrl+<letter>; the letter is in the accompanying char (see handle_ctrl)
};

} // namespace viewmodel
