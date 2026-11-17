/*
 * SPDX-License-Identifier: MIT
 */

#include "viewmodel/chips/lan_tech_chip.h"

#include "platform/lan_info.h"
#include "platform/net.h"
#include "viewmodel/alice_viewmodel.h"

#include <arpa/inet.h>
#include <netdb.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>

namespace viewmodel::chips {
namespace {

enum CheckIndex : size_t { kLink, kDhcp, kGateway, kDns, kInternet, kSwitch, kCheckCount };

using Clock = std::chrono::steady_clock;

int ms_since(Clock::time_point start) {
    return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count());
}

std::string ms_text(int ms) {
    return std::to_string(ms) + " ms";
}

} // namespace

LanTechChip::LanTechChip(model::PetConfig config) : config_(std::move(config)) {
    for (const char* label : {"Link", "DHCP", "Gateway", "DNS", "Internet", "Switch port"}) {
        checks_.push_back({label, State::Pending, ""});
    }
}

void LanTechChip::enter() {
    if (!tested_) run_tests();
}

void LanTechChip::run_tests() {
    tested_ = true;
    running_ = true;
    view_ = View::Test;
    verdict_.clear();
    headline_ = "testing";
    for (auto& check : checks_) {
        check.state = State::Pending;
        check.value = "...";
    }
    const size_t gen = ++generation_;
    results_.run([preferred = config_.lan_interface, gen]() {
        Result r{{}, "", "", {}, true, gen};
        r.checks.resize(kCheckCount);
        auto put = [&](size_t index, State state, const std::string& value) {
            r.checks[index].state = state;
            r.checks[index].value = value;
        };

        platform::lan::Link link;
        if (!platform::lan::pick_link(preferred, link)) {
            put(kLink, State::Fail, "no network interface");
            for (size_t i = kDhcp; i < kCheckCount; ++i) put(i, State::Fail, "-");
            return r;
        }
        r.iface = link.name;
        std::string speed = link.wired ? (link.speed_mbps > 0 ? std::to_string(link.speed_mbps) + " Mb/s" : "wired")
                                       : "Wi-Fi";
        if (!link.duplex.empty() && link.duplex != "unknown") speed += " " + link.duplex;
        put(kLink, link.carrier ? State::Ok : State::Fail, link.carrier ? speed + " (" + link.name + ")" : "no link (" + link.name + ")");

        std::string dhcp_server;
        const std::string mode = platform::lan::dhcp_state(link.name, dhcp_server);
        const bool self_assigned = link.ipv4.rfind("169.254.", 0) == 0;
        if (link.ipv4.empty() || self_assigned) {
            put(kDhcp, State::Fail, self_assigned ? "no reply (" + link.ipv4 + ")" : "no address");
        } else {
            put(kDhcp, mode == "static" ? State::Warn : State::Ok, link.ipv4 + (mode == "static" ? " (static)" : ""));
        }
        r.own_ip = link.ipv4;

        const std::string gateway = platform::lan::default_gateway(link.name);
        if (gateway.empty()) {
            put(kGateway, State::Fail, "none");
        } else {
            auto probe = platform::net::probe_tcp(gateway, 53, 1500);
            if (probe.state == platform::net::ProbeState::Offline) probe = platform::net::probe_tcp(gateway, 80, 1500);
            put(kGateway, probe.state == platform::net::ProbeState::Offline ? State::Fail : State::Ok,
                gateway + (probe.latency_ms >= 0 ? " (" + ms_text(probe.latency_ms) + ")" : " (no answer)"));
        }

        const auto dns_start = Clock::now();
        addrinfo hints{};
        hints.ai_family = AF_INET;
        addrinfo* res = nullptr;
        const int rc = getaddrinfo("example.com", "443", &hints, &res);
        const int dns_ms = ms_since(dns_start);
        if (res) freeaddrinfo(res);
        const auto servers = platform::lan::dns_servers();
        put(kDns, rc == 0 ? State::Ok : State::Fail,
            (rc == 0 ? "ok (" + ms_text(dns_ms) + ")" : std::string("lookup failed")) +
                (servers.empty() ? "" : " via " + servers.front()));

        const auto internet = platform::net::probe_tcp("1.1.1.1", 443, 2500);
        put(kInternet, internet.state == platform::net::ProbeState::Online ? State::Ok : State::Fail,
            internet.state == platform::net::ProbeState::Online ? "online (" + ms_text(internet.latency_ms) + ")" : "unreachable");

        const std::string lldp = platform::lan::lldp_neighbor(link.name);
        put(kSwitch, lldp.empty() ? State::Warn : State::Ok, lldp.empty() ? "n/a (no LLDP data)" : lldp);
        return r;
    });
    touch();
}

void LanTechChip::finish() {
    running_ = false;
    const auto state = [&](size_t i) { return checks_[i].state; };
    verdict_ok_ = false;
    if (state(kLink) == State::Fail) {
        headline_ = "NO LINK";
        verdict_ = "No link on " + iface_ + ". Check the cable, try another jack, or the switch port is off.";
    } else if (state(kDhcp) == State::Fail) {
        headline_ = "NO DHCP";
        verdict_ = "Cable and jack are fine (link is up), but nothing hands out an address. Check the switch port's VLAN or the router's DHCP pool.";
    } else if (state(kGateway) == State::Fail) {
        headline_ = "NO GATEWAY";
        verdict_ = "Got an address but the gateway doesn't answer. Wrong VLAN/subnet, or the router is down.";
    } else if (state(kDns) == State::Fail) {
        headline_ = "NO DNS";
        verdict_ = "Gateway answers but names don't resolve. Check the DNS server (AdGuard?) handed out by DHCP.";
    } else if (state(kInternet) == State::Fail) {
        headline_ = "NO INTERNET";
        verdict_ = "Local network is fine; the internet uplink (modem/ISP) isn't reachable.";
    } else {
        headline_ = "LINK UP";
        verdict_ok_ = true;
        verdict_ = checks_[kLink].value.find("Wi-Fi") != std::string::npos ? "Network is good (tested over Wi-Fi, not a jack)."
                                                                             : "jack is good";
    }
    touch();
}

void LanTechChip::sweep() {
    if (sweeping_ || own_ip_.empty()) return;
    sweeping_ = true;
    view_ = View::Devices;
    scroll_ = 0;
    const size_t gen = generation_;
    results_.run([iface = iface_, own = own_ip_, gen]() {
        Result r{{}, own, iface, {}, false, gen};
        // nmap-lite: touch every /24 neighbour (TCP 80, short timeout) so the ARP
        // table fills, then read it. 32 probes at a time.
        const std::string prefix = own.substr(0, own.rfind('.') + 1);
        std::atomic<int> next{1};
        std::vector<std::thread> pool;
        for (int t = 0; t < 32; ++t) {
            pool.emplace_back([&]() {
                for (int host = next++; host < 255; host = next++) {
                    platform::net::probe_tcp(prefix + std::to_string(host), 80, 300);
                }
            });
        }
        for (auto& th : pool) th.join();
        for (const auto& n : platform::lan::neighbors(iface)) {
            if (n.ip.rfind(prefix, 0) != 0) continue;
            r.devices.push_back({n.ip, n.mac, platform::lan::reverse_name(n.ip), false});
        }
        r.devices.push_back({own, "", "this PET", true});
        std::sort(r.devices.begin(), r.devices.end(), [](const Device& a, const Device& b) {
            return std::atoi(a.ip.c_str() + a.ip.rfind('.') + 1) < std::atoi(b.ip.c_str() + b.ip.rfind('.') + 1);
        });
        return r;
    });
    touch();
}

void LanTechChip::ask_alice() {
    view_ = View::Alice;
    alice_waiting_ = true;
    alice_text_.clear();
    // Instructions first: the bridge keeps only the first kBridgeMaxInput characters.
    std::string report =
        "PET LAN TECH jack test. In 2-3 short sentences for a tiny screen, tell Holden (the store IT person) "
        "what is wrong and what to try next. Results on " + iface_ + ":\n";
    for (const auto& c : checks_) {
        const char* mark = c.state == State::Ok ? "OK" : c.state == State::Warn ? "WARN" : c.state == State::Fail ? "FAIL" : "?";
        report += "- " + c.label + ": " + mark + " " + c.value + "\n";
    }
    report = clip_for_bridge(report);
    alice_.run([config = config_, report]() { return bridge_ask(config, report); });
    touch();
}

bool LanTechChip::handle_key(Key key) {
    if (key == Key::Escape) {
        if (view_ != View::Test) {
            view_ = View::Test;
            touch();
            return true;
        }
        return false;
    }
    if (view_ == View::Devices) {
        if (key == Key::Up && scroll_ > 0) --scroll_;
        if (key == Key::Down && scroll_ + 1 < devices_.size()) ++scroll_;
        touch();
    } else if (view_ == View::Alice && key == Key::Enter) {
        view_ = View::Test;
        touch();
    } else if (view_ == View::Test && key == Key::Enter && !running_) {
        run_tests();
    }
    return true;
}

void LanTechChip::handle_char(const std::string& utf8) {
    const char c = utf8.size() == 1 ? static_cast<char>(std::tolower(static_cast<unsigned char>(utf8[0]))) : 0;
    if (c == 'r') {
        if (view_ == View::Devices) {
            sweep();
        } else if (!running_) {
            run_tests();
        }
    } else if (c == 'd' && !running_) {
        if (devices_.empty()) sweep();
        else view_ = View::Devices;
        touch();
    } else if (c == 'a' && !running_ && !alice_waiting_) {
        ask_alice();
    }
}

void LanTechChip::tick(uint32_t) {
    Result r{};
    while (results_.poll(r)) {
        if (r.generation != generation_) {
            // A sweep that outlived a re-test: its devices are stale, but it is over.
            if (!r.done && sweeping_) {
                sweeping_ = false;
                touch();
            }
            continue;
        }
        if (r.done) {
            iface_ = r.iface;
            own_ip_ = r.own_ip;
            for (size_t i = 0; i < checks_.size() && i < r.checks.size(); ++i) {
                checks_[i].state = r.checks[i].state;
                checks_[i].value = r.checks[i].value;
            }
            finish();
        } else {
            devices_ = std::move(r.devices);
            sweeping_ = false;
            touch();
        }
    }
    BridgeReply reply;
    while (alice_.poll(reply)) on_alice_reply(reply);
}

void LanTechChip::on_alice_reply(const BridgeReply& reply) {
    alice_waiting_ = false;
    alice_text_ = AliceViewModel::screen_text(reply.ok ? reply.reply : reply.error);
    touch();
}

} // namespace viewmodel::chips
