/*
 * SPDX-License-Identifier: MIT
 */

#include "lan_info.h"

#include "process.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <fstream>
#include <sstream>

namespace platform::lan {
namespace {

std::string read_line(const std::string& path) {
    std::ifstream in(path);
    std::string line;
    std::getline(in, line);
    return line;
}

std::string ntop(const sockaddr* addr) {
    char buffer[INET_ADDRSTRLEN] = {};
    if (addr && addr->sa_family == AF_INET) {
        inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in*>(addr)->sin_addr, buffer, sizeof(buffer));
    }
    return buffer;
}

bool looks_wired(const std::string& name) {
#if defined(__APPLE__)
    return name == "en0" || name.rfind("en", 0) == 0;  // refined by `networksetup` only if needed
#else
    return name.rfind("eth", 0) == 0 || name.rfind("en", 0) == 0 || name.rfind("usb", 0) == 0;
#endif
}

std::vector<std::string> words(const std::string& line) {
    std::istringstream in(line);
    std::vector<std::string> out;
    std::string w;
    while (in >> w) out.push_back(w);
    return out;
}

} // namespace

bool pick_link(const std::string& preferred, Link& out) {
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return false;
    Link best;
    bool found = false;
    for (ifaddrs* it = list; it; it = it->ifa_next) {
        if (!it->ifa_addr || it->ifa_addr->sa_family != AF_INET) continue;
        if (it->ifa_flags & IFF_LOOPBACK) continue;
        const std::string name = it->ifa_name;
        if (!preferred.empty() && name != preferred) continue;
        // Skip tunnels (Tailscale utun / tailscale0) unless asked for explicitly.
        if (preferred.empty() && (name.rfind("utun", 0) == 0 || name.rfind("tailscale", 0) == 0 || name.rfind("docker", 0) == 0)) continue;
        Link link;
        link.name = name;
        link.ipv4 = ntop(it->ifa_addr);
        link.netmask = ntop(it->ifa_netmask);
        link.carrier = (it->ifa_flags & IFF_RUNNING) != 0;
        link.wired = looks_wired(name);
#if !defined(__APPLE__)
        const std::string sys = "/sys/class/net/" + name;
        link.wired = !std::ifstream(sys + "/wireless") && looks_wired(name);
        link.carrier = read_line(sys + "/carrier") == "1";
        const std::string speed = read_line(sys + "/speed");
        link.speed_mbps = speed.empty() ? -1 : std::atoi(speed.c_str());
        link.duplex = read_line(sys + "/duplex");
        link.mac = read_line(sys + "/address");
#endif
        if (!found || (link.wired && !best.wired)) {
            best = link;
            found = true;
        }
    }
    freeifaddrs(list);
#if defined(__APPLE__)
    if (found) {
        std::string out;
        if (proc::run_capture({"ifconfig", best.name}, out, 2000) == 0) {
            const size_t ether = out.find("ether ");
            if (ether != std::string::npos) best.mac = out.substr(ether + 6, 17);
            const size_t media = out.find("media: ");
            if (media != std::string::npos) {
                const std::string m = out.substr(media, out.find('\n', media) - media);
                if (m.find("1000baseT") != std::string::npos) best.speed_mbps = 1000;
                else if (m.find("100baseTX") != std::string::npos) best.speed_mbps = 100;
                else if (m.find("10Gbase") != std::string::npos) best.speed_mbps = 10000;
                else if (m.find("2500Base") != std::string::npos) best.speed_mbps = 2500;
                if (m.find("full-duplex") != std::string::npos) best.duplex = "full";
                else if (m.find("half-duplex") != std::string::npos) best.duplex = "half";
                best.wired = best.speed_mbps > 0;
            }
        }
    }
#endif
    out = best;
    return found;
}

std::string default_gateway(const std::string& interface_name) {
#if defined(__APPLE__)
    std::string out;
    proc::run_capture({"route", "-n", "get", "default"}, out, 2000);
    const size_t at = out.find("gateway: ");
    if (at == std::string::npos) return {};
    return out.substr(at + 9, out.find('\n', at) - at - 9);
#else
    std::ifstream in("/proc/net/route");
    std::string line;
    std::getline(in, line);
    while (std::getline(in, line)) {
        const auto w = words(line);
        if (w.size() < 3 || w[1] != "00000000") continue;
        if (!interface_name.empty() && w[0] != interface_name) continue;
        const unsigned long gw = std::stoul(w[2], nullptr, 16);
        in_addr addr{};
        addr.s_addr = static_cast<in_addr_t>(gw);
        return inet_ntoa(addr);
    }
    return {};
#endif
}

std::string dhcp_state(const std::string& interface_name, [[maybe_unused]] std::string& server) {
    std::string out;
#if defined(__APPLE__)
    if (proc::run_capture({"ipconfig", "getpacket", interface_name}, out, 2000) != 0 || out.empty()) return "static";
    const size_t at = out.find("server_identifier (ip): ");
    if (at != std::string::npos) server = out.substr(at + 24, out.find('\n', at) - at - 24);
    return "dynamic";
#else
    if (proc::run_capture({"ip", "-4", "addr", "show", "dev", interface_name}, out, 2000) != 0) return {};
    return out.find(" dynamic") != std::string::npos ? "dynamic" : "static";
    // TODO(device): read the DHCP server from NetworkManager (`nmcli -t -f DHCP4 dev show`).
#endif
}

std::vector<std::string> dns_servers() {
    std::vector<std::string> out;
    std::ifstream in("/etc/resolv.conf");
    std::string line;
    while (std::getline(in, line)) {
        const auto w = words(line);
        if (w.size() >= 2 && w[0] == "nameserver") out.push_back(w[1]);
    }
    return out;
}

std::string lldp_neighbor(const std::string& interface_name) {
    std::string out;
    if (proc::run_capture({"lldpctl", "-f", "keyvalue", interface_name}, out, 2500, false) != 0) return {};
    std::string sys;
    std::string port;
    std::istringstream lines(out);
    std::string line;
    while (std::getline(lines, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        if (key.find(".chassis.name") != std::string::npos) sys = line.substr(eq + 1);
        if (key.find(".port.descr") != std::string::npos || (port.empty() && key.find(".port.ifname") != std::string::npos)) {
            port = line.substr(eq + 1);
        }
    }
    if (sys.empty() && port.empty()) return {};
    return sys + (port.empty() ? "" : " · " + port);
}

std::vector<Neighbor> neighbors(const std::string& interface_name) {
    std::vector<Neighbor> out;
#if defined(__APPLE__)
    std::string text;
    proc::run_capture({"arp", "-an", "-i", interface_name}, text, 3000);
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        // ? (192.168.50.1) at 0:11:22:33:44:55 on en0 ifscope [ethernet]
        const size_t l = line.find('(');
        const size_t r = line.find(')');
        const size_t at = line.find(" at ");
        if (l == std::string::npos || r == std::string::npos || at == std::string::npos) continue;
        const std::string mac = words(line.substr(at + 4))[0];
        if (mac == "(incomplete)" || mac == "ff:ff:ff:ff:ff:ff") continue;
        const std::string ip = line.substr(l + 1, r - l - 1);
        if (ip.rfind("224.", 0) == 0 || ip.rfind("239.", 0) == 0 || ip.find(".255") == ip.size() - 4) continue;
        out.push_back({ip, mac, ""});
    }
#else
    std::ifstream in("/proc/net/arp");
    std::string line;
    std::getline(in, line);
    while (std::getline(in, line)) {
        const auto w = words(line);
        // IP HWtype Flags HWaddress Mask Device
        if (w.size() < 6 || w[2] == "0x0" || w[3] == "00:00:00:00:00:00") continue;
        if (!interface_name.empty() && w[5] != interface_name) continue;
        out.push_back({w[0], w[3], ""});
    }
#endif
    return out;
}

std::string reverse_name(const std::string& ip) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) return {};
    char host[NI_MAXHOST] = {};
    if (getnameinfo(reinterpret_cast<sockaddr*>(&addr), sizeof(addr), host, sizeof(host), nullptr, 0, NI_NAMEREQD) != 0) {
        return {};
    }
    return host;
}

} // namespace platform::lan
