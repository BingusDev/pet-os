/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <string>
#include <vector>

// Network facts for the LAN TECH chip. Linux reads sysfs / procfs / `ip`;
// macOS (desktop simulator) uses getifaddrs, `route`, `ipconfig` and `arp`.
// Blocking: worker threads only.
namespace platform::lan {

struct Link {
    std::string name;        // eth0, end0, en0, wlan0
    bool wired{false};
    bool carrier{false};
    int speed_mbps{-1};      // -1 unknown (Wi-Fi, macOS)
    std::string duplex;      // "full" / "half" / ""
    std::string ipv4;
    std::string netmask;
    std::string mac;
};

// The interface to test: `preferred` if set, else the first up, non-loopback
// interface with IPv4 (wired preferred).
bool pick_link(const std::string& preferred, Link& out);
std::string default_gateway(const std::string& interface_name);
// "dynamic" / "static" / "" (unknown) plus the DHCP server if the OS tells us.
std::string dhcp_state(const std::string& interface_name, std::string& server);
std::vector<std::string> dns_servers();
// LLDP neighbour ("sw-backroom · port 7") via lldpctl when lldpd runs; "" otherwise.
std::string lldp_neighbor(const std::string& interface_name);

struct Neighbor {
    std::string ip;
    std::string mac;
    std::string name;  // reverse DNS, may be empty
};
// ARP / neighbour table for the interface (complete entries only).
std::vector<Neighbor> neighbors(const std::string& interface_name);
// Reverse lookup with a short timeout ("" when none).
std::string reverse_name(const std::string& ip);

// Throughput through the system curl: downloads from `down_url` and uploads
// `up_bytes` to `up_url` (POST), each capped at `seconds`. Mb/s; -1 = failed.
struct Speed {
    double down_mbps{-1};
    double up_mbps{-1};
    std::string error;
};
Speed speed_test(const std::string& down_url, const std::string& up_url, size_t up_bytes, int seconds);

// `ping -c count`: average round trip in ms, -1 if nothing answered (or no ping).
double ping_ms(const std::string& ip, int count);

// A temporary static address for the "no DHCP" fallback ([V]). Needs root or
// CAP_NET_ADMIN. `cidr` is "192.168.1.250/24". Returns "" or the error.
std::string add_static(const std::string& interface_name, const std::string& cidr, const std::string& gateway);
void remove_static(const std::string& interface_name, const std::string& cidr, const std::string& gateway);

} // namespace platform::lan
