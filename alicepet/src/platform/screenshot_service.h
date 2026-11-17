/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <string>

namespace platform::screenshot {

struct CaptureResult {
    bool success{false};
    std::string path;
    std::string error;
};

CaptureResult capture_active_screen();

// Writes the active LVGL screen to an exact path (used by snapshot mode).
bool save_active_screen_png(const std::string& path, std::string& error);

} // namespace platform::screenshot
