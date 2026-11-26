/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>

namespace platform {

struct DeviceStatus {
    bool wifi_present{false};
    bool wifi_connected{false};
    int32_t wifi_strength_percent{-1};
    bool ethernet_present{false};
    bool ethernet_connected{false};
    bool battery_present{false};
    bool battery_charging{false};
    int32_t battery_percent{-1};
};

DeviceStatus read_device_status();

// How the PET reaches the internet right now, for the status bar: JACK IN's
// "Share PC Internet" wins (the app's programs go through the PC), then a wired
// link, then Wi-Fi.
enum class NetRoute { PcNet, Lan, WiFi, Offline };
NetRoute net_route(const DeviceStatus& status, bool pc_sharing);
const char* net_route_label(NetRoute route);

} // namespace platform
