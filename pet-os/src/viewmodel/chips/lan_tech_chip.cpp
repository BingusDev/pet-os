/*
 * SPDX-License-Identifier: MIT
 */

#include "viewmodel/chips/lan_tech_chip.h"
#include "model/navi.h"

#include "platform/lan_info.h"
#include "platform/net.h"
#include "platform/process.h"
#include "viewmodel/alice_viewmodel.h"

#include <arpa/inet.h>
#include <netdb.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
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

LanTechChip::~LanTechChip() { drop_static(); }

void LanTechChip::enter() {
    if (!tested_) run_tests();
}

void LanTechChip::leave() {
    // A [V] static address is only for this test session.
    if (!static_cidr_.empty()) {
        drop_static();
        tested_ = false;
    }
    view_ = View::Test;
}

const std::vector<LanTechChip::Port>& LanTechChip::common_ports() {
    static const std::vector<Port> ports = {
        {22, "ssh"},   {53, "dns"},   {80, "web"},      {139, "netbios"}, {443, "https"}, {445, "smb"},
        {515, "lpd"},  {554, "rtsp"}, {631, "ipp"},     {3389, "rdp"},    {5900, "vnc"},  {8080, "web-alt"},
        {8096, "jellyfin"}, {9100, "printer"}, {1883, "mqtt"}, {62078, "iphone"}};
    return ports;
}

std::string LanTechChip::speed_text(double down, double up) {
    char text[48];
    if (down < 0 && up < 0) return "speed test failed";
    std::snprintf(text, sizeof(text), "%.0f↓ / %.0f↑ Mb/s", std::max(0.0, down), std::max(0.0, up));
    return text;
}

std::string LanTechChip::merge_known(const std::string& known, bool existed, long long now, std::vector<Device>& devices) {
    std::map<std::string, std::string> lines;  // mac -> line
    std::map<std::string, long long> first_seen;
    std::istringstream in(known);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream words(line);
        std::string mac;
        long long seen = 0;
        if (!(words >> mac >> seen) || mac.empty() || mac[0] == '#') continue;
        first_seen[mac] = seen;
        lines[mac] = line;
    }
    for (auto& d : devices) {
        if (d.mac.empty()) continue;
        auto it = first_seen.find(d.mac);
        if (it == first_seen.end()) {
            first_seen[d.mac] = now;
            d.is_new = existed;
        } else {
            d.is_new = existed && now - it->second < 86400;
        }
        lines[d.mac] = d.mac + " " + std::to_string(first_seen[d.mac]) + " " + d.ip + " " + (d.name.empty() ? "-" : d.name);
    }
    std::string out = "# PET-os LAN TECH: devices seen on this network (mac first_seen ip name)\n";
    for (const auto& [mac, text] : lines) out += text + "\n";
    return out;
}

void LanTechChip::run_tests() {
    tested_ = true;
    running_ = true;
    view_ = View::Test;
    verdict_.clear();
    status_.clear();
    speed_running_ = false;
    headline_ = "testing";
    for (auto& check : checks_) {
        check.state = State::Pending;
        check.value = "...";
    }
    const size_t gen = ++generation_;
    results_.run([preferred = config_.lan_interface, gen]() {
        Result r;
        r.kind = Result::Kind::Test;
        r.generation = gen;
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
        r.extra = gateway;
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
        if (config_.lan_speed_test) run_speed();
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
    std::string known_path;
    if (!own_ip_.empty()) {
        known_path = platform::proc::expand_home(config_.lan_known_dir) + "/" +
                     own_ip_.substr(0, own_ip_.rfind('.')) + ".x-" + iface_ + ".txt";
    }
    results_.run([iface = iface_, own = own_ip_, gen, known_path]() {
        Result r;
        r.kind = Result::Kind::Sweep;
        r.own_ip = own;
        r.iface = iface;
        r.generation = gen;
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
            r.devices.push_back({n.ip, n.mac, platform::lan::reverse_name(n.ip), false, false});
        }
        // Who's new: devices first seen on this network in the last day.
        std::error_code ec;
        std::ifstream in(known_path);
        const bool existed = static_cast<bool>(in);
        std::stringstream known;
        if (existed) known << in.rdbuf();
        const std::string updated = merge_known(known.str(), existed, static_cast<long long>(std::time(nullptr)), r.devices);
        r.baseline = !existed;
        std::filesystem::create_directories(std::filesystem::path(known_path).parent_path(), ec);
        std::ofstream(known_path) << updated;
        r.devices.push_back({own, "", "this PET", true, false});
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
        "PET LAN TECH jack test. In 2-3 short sentences for a tiny screen, tell " + model::owner_or("the tech") +
        " what is wrong and what to try next. Results on " + iface_ + ":\n";
    for (const auto& c : checks_) {
        const char* mark = c.state == State::Ok ? "OK" : c.state == State::Warn ? "WARN" : c.state == State::Fail ? "FAIL" : "?";
        report += "- " + c.label + ": " + mark + " " + c.value + "\n";
    }
    report = clip_for_bridge(report);
    alice_.run([config = config_, report]() { return bridge_ask(config, report); });
    touch();
}

void LanTechChip::run_speed() {
    speed_running_ = true;
    internet_value_ = checks_[kInternet].value;
    checks_[kInternet].value = internet_value_ + " · speed...";
    const size_t gen = generation_;
    results_.run([down = config_.lan_speed_down_url, up = config_.lan_speed_up_url, gen]() {
        Result r;
        r.kind = Result::Kind::Speed;
        r.generation = gen;
        const auto speed = platform::lan::speed_test(down, up, 10u * 1024 * 1024, 8);
        r.down = speed.down_mbps;
        r.up = speed.up_mbps;
        r.text = speed.error;
        return r;
    });
}

bool LanTechChip::can_try_static() const {
    return !running_ && !static_running_ && static_cidr_.empty() && !iface_.empty() &&
           checks_[kLink].state != State::Fail && checks_[kDhcp].state == State::Fail;
}

void LanTechChip::try_static() {
    if (!can_try_static()) return;
    static_running_ = true;
    status_ = "trying static addresses...";
    results_.run([iface = iface_, candidates = config_.lan_static, gen = generation_]() {
        Result r;
        r.kind = Result::Kind::Static;
        r.generation = gen;
        std::string last_error;
        for (const auto& candidate : candidates) {
            const auto bar = candidate.find('|');
            const std::string cidr = candidate.substr(0, bar);
            const std::string gw = bar == std::string::npos ? "" : candidate.substr(bar + 1);
            last_error = platform::lan::add_static(iface, cidr, gw);
            if (!last_error.empty()) break;  // usually "not root": no point trying the rest
            // Does anything answer there? The gateway (DNS or web port), else a ping.
            const bool answers = !gw.empty() &&
                                 (platform::net::probe_tcp(gw, 53, 800).state != platform::net::ProbeState::Offline ||
                                  platform::net::probe_tcp(gw, 80, 800).state != platform::net::ProbeState::Offline ||
                                  platform::lan::ping_ms(gw, 1) >= 0);
            if (answers) {
                r.text = cidr;
                r.extra = gw;
                return r;
            }
            platform::lan::remove_static(iface, cidr, gw);
        }
        r.extra = last_error.empty() ? "no gateway answered on " + std::to_string(candidates.size()) + " guesses"
                                     : "can't set an address: " + last_error;
        return r;
    });
    touch();
}

void LanTechChip::drop_static() {
    if (static_cidr_.empty()) return;
    platform::lan::remove_static(iface_, static_cidr_, static_gw_);
    static_cidr_.clear();
    static_gw_.clear();
}

void LanTechChip::probe_device(const Device& device) {
    detail_ = device;
    view_ = View::Device;
    probing_ = true;
    detail_ping_ms_ = -1;
    detail_open_.clear();
    results_.run([ip = device.ip, gen = generation_]() {
        Result r;
        r.kind = Result::Kind::Detail;
        r.generation = gen;
        r.own_ip = ip;
        r.ping_ms = platform::lan::ping_ms(ip, 3);
        // Connected = open; refused = up but closed; nothing = filtered.
        std::vector<std::thread> pool;
        std::mutex mutex;
        for (const auto& port : common_ports()) {
            pool.emplace_back([&, p = port.port]() {
                const auto probe = platform::net::probe_tcp(ip, p, 700);
                if (probe.state == platform::net::ProbeState::Online && probe.detail.find("open") != std::string::npos) {
                    std::lock_guard<std::mutex> lock(mutex);
                    r.open.push_back(p);
                }
            });
        }
        for (auto& t : pool) t.join();
        std::sort(r.open.begin(), r.open.end());
        return r;
    });
    touch();
}

std::string LanTechChip::report_text(const std::string& when) const {
    std::string out = "PET-os LAN TECH report · " + when + "\n";
    out += "Interface: " + (iface_.empty() ? std::string("?") : iface_) + (own_ip_.empty() ? "" : " · " + own_ip_) + "\n";
    if (!static_cidr_.empty()) out += "Static test address: " + static_cidr_ + " via " + static_gw_ + "\n";
    out += "\n";
    for (const auto& c : checks_) {
        const char* mark = c.state == State::Ok ? "OK  " : c.state == State::Warn ? "WARN" : c.state == State::Fail ? "FAIL" : "... ";
        out += std::string(mark) + "  " + c.label + ": " + c.value + "\n";
    }
    out += "\nVerdict: " + headline_ + ". " + verdict_ + "\n";
    if (!alice_text_.empty()) out += "\n" + model::navi_name() + ": " + alice_text_ + "\n";
    if (!devices_.empty()) {
        out += "\nDevices (" + std::to_string(devices_.size()) + "):\n";
        for (const auto& d : devices_) {
            out += "  " + d.ip + "  " + (d.mac.empty() ? std::string("-") : d.mac) + "  " +
                   (d.self ? std::string("this PET") : d.name.empty() ? std::string("?") : d.name) + (d.is_new ? "  NEW" : "") + "\n";
        }
    }
    return out;
}

void LanTechChip::save_report() {
    char stamp[32];
    char when[32];
    const std::time_t now = std::time(nullptr);
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d-%H%M", std::localtime(&now));
    std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M", std::localtime(&now));
    const std::string dir = platform::proc::expand_home(config_.lan_report_dir);
    const std::string path = dir + "/lan-" + stamp + "-" + (iface_.empty() ? "net" : iface_) + ".txt";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::ofstream out(path);
    out << report_text(when);
    status_ = out ? "saved " + path : "can't write " + dir;
    touch();
}

std::vector<const LanTechChip::Device*> LanTechChip::devices() const {
    std::vector<const Device*> out;
    for (const auto& d : devices_) {
        if (!new_only_ || d.is_new) out.push_back(&d);
    }
    return out;
}

size_t LanTechChip::new_count() const {
    return static_cast<size_t>(std::count_if(devices_.begin(), devices_.end(), [](const Device& d) { return d.is_new; }));
}

bool LanTechChip::handle_key(Key key) {
    if (key == Key::Escape) {
        if (view_ == View::Device) {
            view_ = View::Devices;
            touch();
            return true;
        }
        if (view_ != View::Test) {
            view_ = View::Test;
            touch();
            return true;
        }
        return false;
    }
    if (view_ == View::Devices) {
        const size_t count = devices().size();
        if (key == Key::Up && scroll_ > 0) --scroll_;
        if (key == Key::Down && scroll_ + 1 < count) ++scroll_;
        if (key == Key::Enter && scroll_ < count && !devices()[scroll_]->self) probe_device(*devices()[scroll_]);
        touch();
    } else if (view_ == View::Device && key == Key::Enter && !probing_) {
        probe_device(detail_);
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
        } else if (view_ == View::Device) {
            if (!probing_) probe_device(detail_);
        } else if (!running_) {
            run_tests();
        }
    } else if (c == 'd' && !running_) {
        if (devices_.empty()) sweep();
        else view_ = View::Devices;
        touch();
    } else if (c == 'a' && !running_ && !alice_waiting_ && view_ != View::Devices && view_ != View::Device) {
        ask_alice();
    } else if (c == 's' && !running_) {
        save_report();
    } else if (c == 'v' && view_ == View::Test) {
        try_static();
    } else if (c == 'w' && view_ == View::Devices) {
        new_only_ = !new_only_;
        scroll_ = 0;
        touch();
    }
}

void LanTechChip::tick(uint32_t) {
    Result r;
    while (results_.poll(r)) {
        if (r.kind == Result::Kind::Static) {
            static_running_ = false;
            if (!r.text.empty()) {
                static_cidr_ = r.text;
                static_gw_ = r.extra;
                status_ = "static " + static_cidr_ + " works: the network is there, DHCP isn't";
                if (r.generation == generation_) run_tests();
                status_ = "testing with static " + static_cidr_;
            } else {
                status_ = r.extra;
            }
            touch();
            continue;
        }
        if (r.generation != generation_) {
            // A sweep or speed test that outlived a re-test: stale, but over.
            if (r.kind == Result::Kind::Sweep) sweeping_ = false;
            if (r.kind == Result::Kind::Speed) speed_running_ = false;
            if (r.kind == Result::Kind::Detail) probing_ = false;
            touch();
            continue;
        }
        switch (r.kind) {
        case Result::Kind::Test:
            iface_ = r.iface;
            own_ip_ = r.own_ip;
            gateway_ = r.extra;
            for (size_t i = 0; i < checks_.size() && i < r.checks.size(); ++i) {
                checks_[i].state = r.checks[i].state;
                checks_[i].value = r.checks[i].value;
            }
            finish();
            break;
        case Result::Kind::Sweep:
            devices_ = std::move(r.devices);
            baseline_ = r.baseline;
            sweeping_ = false;
            if (scroll_ >= devices().size()) scroll_ = 0;
            touch();
            break;
        case Result::Kind::Speed:
            speed_running_ = false;
            checks_[kInternet].value = internet_value_ + " · " + speed_text(r.down, r.up);
            if (r.down >= 0 && r.down < 5) checks_[kInternet].state = State::Warn;
            touch();
            break;
        case Result::Kind::Detail:
            if (r.own_ip == detail_.ip) {
                probing_ = false;
                detail_ping_ms_ = r.ping_ms;
                detail_open_ = std::move(r.open);
                touch();
            }
            break;
        case Result::Kind::Static:
            break;
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
