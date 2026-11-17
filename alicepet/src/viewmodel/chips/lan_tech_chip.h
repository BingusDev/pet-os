/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "model/pet_config.h"
#include "viewmodel/chip_module.h"
#include "viewmodel/chips/bridge_client.h"

#include <string>
#include <vector>

namespace viewmodel::chips {

// LAN TECH (chip 004), mockups/tools.html screens 5-7. Real checks:
//   link/speed, DHCP (address + dynamic/static), gateway reachability, DNS
//   lookup time, internet reachability, LLDP switch port (only if lldpd runs),
//   plus a who's-on-this-network list (/24 connect sweep + ARP + reverse DNS).
// [A] sends the results to Alice for a plain-English diagnosis.
// Not done yet: throughput test (shows "-"), static-IP fallback ([V] in the mockup).
class LanTechChip : public ChipModule {
public:
    enum class View { Test, Devices, Alice };
    enum class State { Pending, Ok, Warn, Fail };

    struct Check {
        std::string label;
        State state{State::Pending};
        std::string value;
    };

    struct Device {
        std::string ip;
        std::string mac;
        std::string name;
        bool self{false};
    };

    explicit LanTechChip(model::PetConfig config);

    const char* id() const override { return "lan-tech"; }
    void enter() override;
    bool handle_key(Key key) override;
    void handle_char(const std::string& utf8) override;
    void tick(uint32_t elapsed_ms) override;
    bool keep_awake() const override { return running_ || sweeping_; }
    bool wants_text() const override { return true; }  // letter hotkeys (R/D/A) instead of F/X/Z/C nav

    View view() const { return view_; }
    const std::vector<Check>& checks() const { return checks_; }
    bool running() const { return running_; }
    const std::string& verdict() const { return verdict_; }
    bool verdict_ok() const { return verdict_ok_; }
    const std::string& headline() const { return headline_; }
    const std::string& interface_name() const { return iface_; }

    const std::vector<Device>& devices() const { return devices_; }
    bool sweeping() const { return sweeping_; }
    size_t scroll() const { return scroll_; }

    const std::string& alice_text() const { return alice_text_; }
    bool alice_waiting() const { return alice_waiting_; }

private:
    // A finished test run (done=true: checks + interface + own IP) or device sweep.
    struct Result {
        std::vector<Check> checks;
        std::string own_ip;
        std::string iface;
        std::vector<Device> devices;
        bool done{false};
        size_t generation{0};
    };

    void run_tests();
    void sweep();
    void finish();
    void ask_alice();

    model::PetConfig config_;
    View view_{View::Test};
    std::vector<Check> checks_;
    bool running_{false};
    bool tested_{false};
    std::string iface_;
    std::string own_ip_;
    std::string verdict_;
    bool verdict_ok_{false};
    std::string headline_;
    size_t generation_{0};
    Mailbox<Result> results_;

    std::vector<Device> devices_;
    bool sweeping_{false};
    size_t scroll_{0};

    std::string alice_text_;
    bool alice_waiting_{false};
    Mailbox<BridgeReply> alice_;
};

} // namespace viewmodel::chips
