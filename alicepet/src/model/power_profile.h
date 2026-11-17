/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "model/chip_registry.h"

#include <cstring>

namespace model {

// What the PET needs powered right now (BUILD-PLAN "Power plan"). Everything not
// needed is turned down by platform::PowerManager.
struct PowerProfile {
    bool wifi{true};    // false = Wi-Fi power save on (the link stays up)
    bool audio{false};
    bool usb_hub{false};
    bool ir{false};
    bool hdmi{false};
    int cores{1};       // CPU cores online
    bool slow_cpu{false};  // cap the CPU at its lowest frequency (nap)

    bool operator==(const PowerProfile& o) const {
        return wifi == o.wifi && audio == o.audio && usb_hub == o.usb_hub && ir == o.ir && hdmi == o.hdmi &&
               cores == o.cores && slow_cpu == o.slow_cpu;
    }
    bool operator!=(const PowerProfile& o) const { return !(*this == o); }
};

// Home, folder and launcher screens: Wi-Fi for chat and NET SCAN, one core.
inline PowerProfile launcher_profile() { return {}; }

// Napping: screen off, one core at the lowest clock, Wi-Fi in power save.
inline PowerProfile nap_profile() {
    PowerProfile p;
    p.wifi = false;
    p.slow_cpu = true;
    return p;
}

// A slotted chip: exactly what its `needs` list declares ("wifi", "audio",
// "usb_hub", "ir", "hdmi", "cores:N"). Chips get two cores by default so the UI
// stays smooth next to a worker thread.
inline PowerProfile chip_profile(const ChipInfo& chip) {
    PowerProfile p;
    p.wifi = false;
    p.cores = 2;
    for (const char* need : chip.needs) {
        if (!need) break;
        if (std::strcmp(need, "wifi") == 0) p.wifi = true;
        else if (std::strcmp(need, "audio") == 0) p.audio = true;
        else if (std::strcmp(need, "usb_hub") == 0) p.usb_hub = true;
        else if (std::strcmp(need, "ir") == 0) p.ir = true;
        else if (std::strcmp(need, "hdmi") == 0) p.hdmi = true;
        else if (std::strncmp(need, "cores:", 6) == 0 && need[6] >= '1' && need[6] <= '4') p.cores = need[6] - '0';
    }
    return p;
}

} // namespace model
