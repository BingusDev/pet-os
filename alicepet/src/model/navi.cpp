/*
 * SPDX-License-Identifier: MIT
 */

#include "model/navi.h"

#include <cctype>
#include <utility>

namespace model {

namespace {

NaviIdentity& identity() {
    static NaviIdentity value;
    return value;
}

} // namespace

std::string clean_display_name(const std::string& value, size_t max_len) {
    std::string out;
    for (const char c : value) {
        const auto u = static_cast<unsigned char>(c);
        if (u >= 0x20 && u < 0x7f) out += c;
    }
    const size_t first = out.find_first_not_of(' ');
    if (first == std::string::npos) return "";
    out = out.substr(first, out.find_last_not_of(' ') - first + 1);
    if (out.size() > max_len) {
        out.resize(max_len);
        while (!out.empty() && out.back() == ' ') out.pop_back();
    }
    return out;
}

const NaviIdentity& navi() {
    return identity();
}

void set_navi(NaviIdentity value) {
    value.name = clean_display_name(value.name, kMaxNaviName);
    if (value.name.empty()) value.name = "Alice";
    value.owner = clean_display_name(value.owner, kMaxOwnerName);
    identity() = std::move(value);
}

std::string navi_name() {
    return identity().name.empty() ? "Alice" : identity().name;
}

std::string navi_exe() {
    std::string out = navi_name();
    for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    for (char& c : out) if (c == ' ') c = '_';
    return out + ".EXE";
}

std::string owner_or(const std::string& fallback) {
    return identity().owner.empty() ? fallback : identity().owner;
}

} // namespace model
