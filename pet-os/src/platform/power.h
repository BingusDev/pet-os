/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "model/power_profile.h"

#include <string>

namespace platform {

// Applies a model::PowerProfile to the hardware (BUILD-PLAN "Power plan").
// Knobs it drives today, all through sysfs or `iw`:
//   - CPU cores 1-3 online/offline (cpu0 always stays up)
//   - CPU max frequency capped at the lowest step while napping
//   - Wi-Fi power save while nothing needs the network
//   - once at start: Bluetooth soft-blocked (rfkill), the ACT LED off
// Not yet (needs the real board to find the switch): USB hub/Ethernet power,
// audio codec standby, HDMI off, the IR receiver. Those fields are tracked so
// chips declare them now, and only logged.
// Every write needs root (or a udev rule); failures are logged once and ignored.
// `sysroot` prefixes every path so tests can point it at a fake tree.
class PowerManager {
public:
    explicit PowerManager(std::string sysroot = "");

    // One-time knobs (Bluetooth, ACT LED). Call once at start.
    void init();
    // Applies `profile` if it differs from the last one. Cheap to call every frame.
    void apply(const model::PowerProfile& profile);
    const model::PowerProfile& current() const { return current_; }
    // Puts everything back (all cores, full clock, Wi-Fi power save off). Called on exit.
    void restore();

private:
    bool write(const std::string& path, const std::string& value);
    std::string read(const std::string& path) const;
    void set_cores(int cores);
    void set_slow_cpu(bool slow);
    void set_wifi_power_save(bool on);
    std::string wifi_interface() const;

    std::string root_;
    model::PowerProfile current_{};
    bool applied_{false};
    std::string normal_max_freq_;
    bool warned_{false};
};

} // namespace platform
