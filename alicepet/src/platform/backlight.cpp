/*
 * SPDX-License-Identifier: MIT
 */

#include "backlight.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace platform {

#if USE_DESKTOP

bool set_screen_level(ScreenLevel, const std::string&, int) {
    return false;
}

#else

namespace {

std::filesystem::path find_backlight(const std::string& dir) {
    if (!dir.empty() && dir != "auto") {
        return dir;
    }
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator("/sys/class/backlight", ec)) {
        return entry.path();
    }
    return {};
}

bool write_value(const std::filesystem::path& path, const std::string& value) {
    std::ofstream out(path);
    if (!out) return false;
    out << value;
    return static_cast<bool>(out);
}

long read_number(const std::filesystem::path& path) {
    std::ifstream in(path);
    long value = -1;
    in >> value;
    return in ? value : -1;
}

// Brightness the panel had before AlicePET first dimmed or switched it off.
long normal_brightness = -1;

} // namespace

// TODO(device): confirm the CardputerZero backlight node name and that the app
// user can write it (may need a udev rule granting group video write access).
bool set_screen_level(ScreenLevel level, const std::string& dir, int dim_percent) {
    const auto node = find_backlight(dir);
    if (node.empty()) {
        return false;
    }
    const long max = read_number(node / "max_brightness");
    if (normal_brightness <= 0) {
        const long current = read_number(node / "brightness");
        normal_brightness = current > 0 ? current : (max > 0 ? max : 1);
    }
    const bool has_power = std::filesystem::exists(node / "bl_power");
    if (level == ScreenLevel::Off) {
        const bool ok = write_value(node / "brightness", "0");
        if (has_power) write_value(node / "bl_power", "4");
        return ok;
    }
    if (has_power) write_value(node / "bl_power", "0");
    long value = normal_brightness;
    if (level == ScreenLevel::Dim && max > 0) {
        const long dim = std::max(1L, max * std::clamp(dim_percent, 1, 100) / 100);
        value = std::min(normal_brightness, dim);
    }
    return write_value(node / "brightness", std::to_string(value));
}

#endif

} // namespace platform
