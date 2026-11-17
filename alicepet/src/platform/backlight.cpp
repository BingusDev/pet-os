/*
 * SPDX-License-Identifier: MIT
 */

#include "backlight.h"

#include <filesystem>
#include <fstream>

namespace platform {

#if USE_DESKTOP

bool set_screen_power(bool, const std::string&) {
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

std::string read_value(const std::filesystem::path& path) {
    std::ifstream in(path);
    std::string value;
    in >> value;
    return value;
}

std::string saved_brightness;

} // namespace

// TODO(device): confirm the CardputerZero backlight node name and that the app
// user can write it (may need a udev rule granting group video write access).
bool set_screen_power(bool on, const std::string& dir) {
    const auto node = find_backlight(dir);
    if (node.empty()) {
        return false;
    }
    bool ok = true;
    if (!on) {
        const std::string current = read_value(node / "brightness");
        if (!current.empty() && current != "0") saved_brightness = current;
        ok = write_value(node / "brightness", "0");
        if (std::filesystem::exists(node / "bl_power")) write_value(node / "bl_power", "4");
    } else {
        if (std::filesystem::exists(node / "bl_power")) write_value(node / "bl_power", "0");
        std::string level = saved_brightness;
        if (level.empty()) level = read_value(node / "max_brightness");
        ok = write_value(node / "brightness", level.empty() ? "1" : level);
    }
    return ok;
}

#endif

} // namespace platform
