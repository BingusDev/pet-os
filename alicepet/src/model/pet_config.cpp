/*
 * SPDX-License-Identifier: MIT
 */

#include "model/pet_config.h"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace model {
namespace {

std::string trim(const std::string& value) {
    size_t begin = 0;
    size_t end = value.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(value[begin]))) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    return value.substr(begin, end - begin);
}

std::string lowercase(std::string value) {
    for (char& c : value) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return value;
}

int to_int(const std::string& value, int fallback) {
    char* end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    return (end && *end == '\0' && !value.empty()) ? static_cast<int>(parsed) : fallback;
}

// "<name>|<address>|<probe>"
bool parse_host(const std::string& value, HostConfig& host) {
    const size_t first = value.find('|');
    const size_t second = first == std::string::npos ? std::string::npos : value.find('|', first + 1);
    if (second == std::string::npos) {
        return false;
    }
    host.name = trim(value.substr(0, first));
    host.address = trim(value.substr(first + 1, second - first - 1));
    const std::string probe = lowercase(trim(value.substr(second + 1)));
    if (host.name.empty() || host.address.empty()) {
        return false;
    }
    if (probe == "ps5") {
        host.probe = ProbeKind::Ps5;
        host.port = 9302;
        return true;
    }
    if (probe.rfind("tcp:", 0) == 0) {
        host.probe = ProbeKind::Tcp;
        host.port = to_int(probe.substr(4), -1);
        return host.port > 0 && host.port < 65536;
    }
    return false;
}

// "<label>|<ssh target>|<note>[|<port>]"
bool parse_bookmark(const std::string& value, SshBookmark& out) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (true) {
        const size_t bar = value.find('|', start);
        parts.push_back(trim(value.substr(start, bar == std::string::npos ? std::string::npos : bar - start)));
        if (bar == std::string::npos) break;
        start = bar + 1;
    }
    if (parts.size() < 2 || parts[0].empty() || parts[1].empty()) {
        return false;
    }
    out.label = parts[0];
    out.target = parts[1];
    out.note = parts.size() > 2 ? parts[2] : "";
    out.port = parts.size() > 3 ? to_int(parts[3], 0) : 0;
    return true;
}

} // namespace

std::string HostConfig::probe_label() const {
    return probe == ProbeKind::Ps5 ? "ps5 discovery" : "tcp:" + std::to_string(port);
}

PetConfig parse_pet_config(const std::string& text, std::vector<std::string>* warnings) {
    PetConfig config;
    std::istringstream input(text);
    std::string section;
    std::string line;
    int line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        line = trim(line);
        if (line.empty() || line.front() == '#' || line.front() == ';') {
            continue;
        }
        if (line.front() == '[' && line.back() == ']') {
            section = lowercase(trim(line.substr(1, line.size() - 2)));
            continue;
        }
        const size_t separator = line.find('=');
        if (separator == std::string::npos) {
            continue;
        }
        const std::string key = lowercase(trim(line.substr(0, separator)));
        const std::string value = trim(line.substr(separator + 1));

        if (section == "bridge") {
            if (key == "url") {
                config.bridge_url = value;
            } else if (key == "token") {
                config.bridge_token = value;
            } else if (key == "timeout_s") {
                config.bridge_timeout_s = to_int(value, config.bridge_timeout_s);
            }
        } else if (section == "netscan") {
            if (key == "timeout_ms") {
                config.netscan_timeout_ms = to_int(value, config.netscan_timeout_ms);
            } else if (key == "host") {
                HostConfig host;
                if (parse_host(value, host)) {
                    config.hosts.push_back(std::move(host));
                } else if (warnings) {
                    warnings->push_back("bad host line " + std::to_string(line_number) + ": " + value);
                }
            }
        } else if (section == "power") {
            if (key == "sleep_after_s") config.sleep_after_s = to_int(value, config.sleep_after_s);
            else if (key == "backlight") config.backlight = value;
            else if (key == "dim_after_s") config.dim_after_s = to_int(value, config.dim_after_s);
            else if (key == "dim_percent") config.dim_percent = to_int(value, config.dim_percent);
            else if (key == "profiles") config.power_profiles = value == "on" || value == "1" || value == "true";
        } else if (section == "voice") {
            if (key == "record_cmd") config.voice_record_cmd = value;
            else if (key == "max_s") config.voice_max_s = to_int(value, config.voice_max_s);
            else if (key == "tts_cmd") config.voice_tts_cmd = value;
            else if (key == "desktop_wav") config.voice_desktop_wav = value;
        } else if (section == "terminal") {
            if (key == "ssh_cmd") {
                config.ssh_cmd = value;
            } else if (key == "ssh_config_import") {
                config.ssh_config_import = value;
            } else if (key == "host") {
                SshBookmark bookmark;
                if (parse_bookmark(value, bookmark)) {
                    config.bookmarks.push_back(std::move(bookmark));
                } else if (warnings) {
                    warnings->push_back("bad terminal host line " + std::to_string(line_number) + ": " + value);
                }
            }
        } else if (section == "jellyfin") {
            if (key == "url") config.jellyfin_url = value;
            else if (key == "api_key") config.jellyfin_api_key = value;
            else if (key == "user") config.jellyfin_user = value;
            else if (key == "player") config.jellyfin_player = value;
            else if (key == "max_bitrate") config.jellyfin_max_bitrate = to_int(value, config.jellyfin_max_bitrate);
            else if (key == "width") config.jellyfin_width = to_int(value, config.jellyfin_width);
        } else if (section == "romm") {
            if (key == "url") config.romm_url = value;
            else if (key == "token") config.romm_token = value;
        } else if (section == "retro") {
            if (key == "player") config.retro_player = value;
            else if (key == "dir") config.retro_dir = value;
            else if (key == "sync_saves") config.retro_sync_saves = value == "on" || value == "1" || value == "true";
            else if (key.rfind("core_", 0) == 0 && key.size() > 5) config.retro_cores[key.substr(5)] = value;
        } else if (section == "remote") {
            if (key == "send_device") config.ir_send_device = value;
            else if (key == "recv_device") config.ir_recv_device = value;
            else if (key == "dir") config.remote_dir = value;
        } else if (section == "lantech") {
            if (key == "interface") config.lan_interface = value;
        }
    }
    return config;
}

bool load_pet_config(const std::string& path, PetConfig& out, std::vector<std::string>* warnings) {
    std::ifstream file(path);
    if (!file) {
        return false;
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    // Personal settings live next to it in <name>.local.conf (not packaged, not
    // in git): read after the main file, so its values win and its host= lines add on.
    std::ifstream local(local_config_path(path));
    if (local) buffer << "\n" << local.rdbuf();
    out = parse_pet_config(buffer.str(), warnings);
    return true;
}

std::string local_config_path(const std::string& path) {
    const std::string ext = ".conf";
    if (path.size() > ext.size() && path.compare(path.size() - ext.size(), ext.size(), ext) == 0) {
        return path.substr(0, path.size() - ext.size()) + ".local.conf";
    }
    return path + ".local";
}

} // namespace model
