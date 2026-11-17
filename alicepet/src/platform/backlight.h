/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <string>

namespace platform {

// Screen power for sleep. On the device this drives the backlight sysfs node
// (`dir` = /sys/class/backlight/<name>, or "auto" for the first one): off saves
// the current brightness and writes 0 (plus bl_power=4 when present), on
// restores it. Desktop builds have no backlight and return false; the app
// shows the dark "zZ" overlay instead (it is drawn on the device too).
bool set_screen_power(bool on, const std::string& dir = "auto");

} // namespace platform
