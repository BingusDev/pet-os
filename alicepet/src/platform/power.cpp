/*
 * SPDX-License-Identifier: MIT
 */

#include "platform/power.h"

#include "logger.h"
#include "process.h"

#include <dirent.h>

#include <fstream>
#include <thread>

namespace platform {
namespace {

constexpr const char* kCpuDir = "/sys/devices/system/cpu";
constexpr int kMaxCores = 4;

} // namespace

PowerManager::PowerManager(std::string sysroot) : root_(std::move(sysroot)) {}

std::string PowerManager::read(const std::string& path) const {
    std::ifstream in(root_ + path);
    std::string value;
    std::getline(in, value);
    return value;
}

bool PowerManager::write(const std::string& path, const std::string& value) {
    std::ofstream out(root_ + path);
    if (out) out << value << '\n';
    out.flush();
    const bool ok = static_cast<bool>(out);
    if (!ok && !warned_) {
        warned_ = true;  // usually "not root": say it once, not every nap
        LOG_WARN("power: can't write {} (run as root or add a udev rule); power profiles are partly off", path);
    }
    return ok;
}

void PowerManager::init() {
    // Bluetooth off unless something needs it later (no chip does yet).
    if (DIR* dir = opendir((root_ + "/sys/class/rfkill").c_str())) {
        while (auto* entry = readdir(dir)) {
            const std::string name = entry->d_name;
            if (name.rfind("rfkill", 0) != 0) continue;
            const std::string base = "/sys/class/rfkill/" + name;
            if (read(base + "/type") == "bluetooth") write(base + "/soft", "1");
        }
        closedir(dir);
    }
    // The green ACT LED blinks on every SD access.
    for (const char* led : {"ACT", "led0"}) {
        const std::string base = std::string("/sys/class/leds/") + led;
        if (read(base + "/brightness").empty()) continue;
        write(base + "/trigger", "none");
        write(base + "/brightness", "0");
        break;
    }
}

void PowerManager::set_cores(int cores) {
    for (int cpu = 1; cpu < kMaxCores; ++cpu) {
        const std::string path = std::string(kCpuDir) + "/cpu" + std::to_string(cpu) + "/online";
        if (read(path).empty()) continue;  // no such core
        write(path, cpu < cores ? "1" : "0");
    }
}

void PowerManager::set_slow_cpu(bool slow) {
    const std::string policy = std::string(kCpuDir) + "/cpufreq/policy0/";
    const std::string min_freq = read(policy + "cpuinfo_min_freq");
    if (min_freq.empty()) return;
    if (normal_max_freq_.empty()) normal_max_freq_ = read(policy + "scaling_max_freq");
    if (normal_max_freq_.empty()) normal_max_freq_ = read(policy + "cpuinfo_max_freq");
    write(policy + "scaling_max_freq", slow ? min_freq : normal_max_freq_);
}

std::string PowerManager::wifi_interface() const {
    std::string found;
    if (DIR* dir = opendir((root_ + "/sys/class/net").c_str())) {
        while (auto* entry = readdir(dir)) {
            const std::string name = entry->d_name;
            if (name == "." || name == "..") continue;
            if (DIR* wireless = opendir((root_ + "/sys/class/net/" + name + "/wireless").c_str())) {
                closedir(wireless);
                found = name;
                break;
            }
        }
        closedir(dir);
    }
    return found;
}

void PowerManager::set_wifi_power_save(bool on) {
    const std::string iface = wifi_interface();
    if (iface.empty() || !root_.empty()) return;  // no Wi-Fi, or a test tree
    // `iw` forks and waits; keep it off the UI thread.
    std::thread([iface, on]() {
        std::string out;
        const int rc = proc::run_capture({"iw", "dev", iface, "set", "power_save", on ? "on" : "off"}, out, 3000);
        if (rc != 0) LOG_WARN("power: iw power_save {} on {} failed ({}): {}", on ? "on" : "off", iface, rc, out);
    }).detach();
}

void PowerManager::apply(const model::PowerProfile& profile) {
    if (applied_ && profile == current_) return;
    const model::PowerProfile before = current_;
    const bool first = !applied_;
    current_ = profile;
    applied_ = true;
    if (first || profile.cores != before.cores) set_cores(profile.cores);
    if (first || profile.slow_cpu != before.slow_cpu) set_slow_cpu(profile.slow_cpu);
    if (first || profile.wifi != before.wifi) set_wifi_power_save(!profile.wifi);
    LOG_INFO("power: cores={} cpu={} wifi={} (audio={} usb={} ir={} hdmi={} not switched yet)", profile.cores,
             profile.slow_cpu ? "min" : "normal", profile.wifi ? "on" : "power-save", profile.audio, profile.usb_hub,
             profile.ir, profile.hdmi);
}

void PowerManager::restore() {
    if (!applied_) return;
    set_cores(kMaxCores);
    set_slow_cpu(false);
    set_wifi_power_save(false);
    applied_ = false;
}

} // namespace platform
