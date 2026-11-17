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
    out = parse_pet_config(buffer.str(), warnings);
    return true;
}

} // namespace model
