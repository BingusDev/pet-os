/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <string>

namespace platform {

// Panel backlight for dim and sleep. On the device this drives the backlight
// sysfs node (`dir` = /sys/class/backlight/<name>, or "auto" for the first one).
// The brightness found on the first call is remembered as "normal": On restores
// it, Dim sets `dim_percent` of max_brightness (never above normal), Off writes 0
// (plus bl_power=4 when present). Desktop builds have no backlight and return
// false; the app shows the dark "zZ" overlay instead (it is drawn on the device too).
enum class ScreenLevel { On, Dim, Off };
bool set_screen_level(ScreenLevel level, const std::string& dir = "auto", int dim_percent = 15);

// Old two-state entry point: On / Off.
inline bool set_screen_power(bool on, const std::string& dir = "auto") {
    return set_screen_level(on ? ScreenLevel::On : ScreenLevel::Off, dir);
}

} // namespace platform
