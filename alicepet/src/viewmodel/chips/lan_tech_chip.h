/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "model/pet_config.h"
#include "viewmodel/chip_module.h"
#include "viewmodel/chips/bridge_client.h"

#include <map>
#include <string>
#include <vector>

namespace viewmodel::chips {

// LAN TECH (chip 004), mockups/tools.html screens 5-7. Real checks:
//   link/speed, DHCP (address + dynamic/static), gateway reachability, DNS
//   lookup time, internet reachability plus a down/up speed test, LLDP switch
//   port (only if lldpd runs), and a who's-on-this-network list (/24 connect
//   sweep + ARP + reverse DNS) that remembers devices per network so new ones
//   stand out ([W]). [↵] on a device pings it and checks common ports.
// [A] sends the results to Alice, [S] saves a text report, [V] tries a
// temporary static address when DHCP fails (removed on retest or leaving).
class LanTechChip : public ChipModule {
public:
    enum class View { Test, Devices, Alice, Device };
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
        bool is_new{false};  // first seen on this network in the last 24 h
    };

    struct Port {
        int port{0};
        const char* service{""};
    };
    // Ports [↵] checks on a device, with what usually listens there.
    static const std::vector<Port>& common_ports();

    explicit LanTechChip(model::PetConfig config);
    ~LanTechChip() override;

    const char* id() const override { return "lan-tech"; }
    void enter() override;
    void leave() override;
    bool handle_key(Key key) override;
    void handle_char(const std::string& utf8) override;
    void tick(uint32_t elapsed_ms) override;
    bool keep_awake() const override { return running_ || sweeping_ || speed_running_ || probing_; }
    bool wants_text() const override { return true; }  // letter hotkeys (R/D/A/S/V/W)

    View view() const { return view_; }
    const std::vector<Check>& checks() const { return checks_; }
    bool running() const { return running_; }
    bool speed_running() const { return speed_running_; }
    const std::string& verdict() const { return verdict_; }
    bool verdict_ok() const { return verdict_ok_; }
    const std::string& headline() const { return headline_; }
    const std::string& interface_name() const { return iface_; }
    const std::string& status() const { return status_; }
    // DHCP failed and a static address could be tried.
    bool can_try_static() const;
    const std::string& static_cidr() const { return static_cidr_; }

    // Devices, filtered to new ones while new_only().
    std::vector<const Device*> devices() const;
    size_t device_count() const { return devices_.size(); }
    size_t new_count() const;
    bool new_only() const { return new_only_; }
    bool sweeping() const { return sweeping_; }
    size_t scroll() const { return scroll_; }
    bool baseline() const { return baseline_; }  // first sweep of this network: nothing is "new" yet

    // [↵] device detail.
    const Device& detail() const { return detail_; }
    bool probing() const { return probing_; }
    double detail_ping_ms() const { return detail_ping_ms_; }
    const std::vector<int>& detail_open() const { return detail_open_; }

    const std::string& alice_text() const { return alice_text_; }
    bool alice_waiting() const { return alice_waiting_; }
    // Alice's diagnosis arrived (called from tick(); public for snapshots and tests).
    void on_alice_reply(const BridgeReply& reply);

    // The report [S] writes (public for tests).
    std::string report_text(const std::string& when) const;
    // "Who's new": merges `devices` into the known-devices file text (mac first_seen ip name lines),
    // marks those first seen within 24 h of `now` as new, and returns the updated file text.
    // A missing file (`known` empty and `existed` false) makes a baseline: nothing is new.
    static std::string merge_known(const std::string& known, bool existed, long long now, std::vector<Device>& devices);
    static std::string speed_text(double down_mbps, double up_mbps);

private:
    struct Result {
        enum class Kind { Test, Sweep, Speed, Static, Detail } kind{Kind::Test};
        std::vector<Check> checks;
        std::string own_ip;
        std::string iface;
        std::vector<Device> devices;
        bool baseline{false};
        double down{-1};
        double up{-1};
        std::string text;   // errors, the static address that worked
        std::string extra;  // its gateway
        double ping_ms{-1};
        std::vector<int> open;
        size_t generation{0};
    };

    void run_tests();
    void sweep();
    void finish();
    void ask_alice();
    void run_speed();
    void try_static();
    void drop_static();
    void probe_device(const Device& device);
    void save_report();

    model::PetConfig config_;
    View view_{View::Test};
    std::vector<Check> checks_;
    bool running_{false};
    bool tested_{false};
    std::string iface_;
    std::string own_ip_;
    std::string gateway_;
    std::string verdict_;
    bool verdict_ok_{false};
    std::string headline_;
    std::string status_;
    size_t generation_{0};
    Mailbox<Result> results_;

    bool speed_running_{false};
    std::string internet_value_;  // Internet row before the speed is appended

    std::string static_cidr_;
    std::string static_gw_;
    bool static_running_{false};

    std::vector<Device> devices_;
    bool sweeping_{false};
    bool baseline_{false};
    bool new_only_{false};
    size_t scroll_{0};

    Device detail_;
    bool probing_{false};
    double detail_ping_ms_{-1};
    std::vector<int> detail_open_;

    std::string alice_text_;
    bool alice_waiting_{false};
    Mailbox<BridgeReply> alice_;
};

} // namespace viewmodel::chips
