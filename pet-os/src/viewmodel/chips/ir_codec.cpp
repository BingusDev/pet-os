/*
 * SPDX-License-Identifier: MIT
 */

#include "viewmodel/chips/ir_codec.h"

#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace viewmodel::chips::ir {
namespace {

bool near(int value, int target, int tolerance_pct = 30) {
    const int tol = target * tolerance_pct / 100;
    return value >= target - tol && value <= target + tol;
}

} // namespace

Raw parse_ir_ctl(const std::string& text) {
    Raw raw;
    std::istringstream in(text);
    std::string token;
    bool expect_pulse = true;
    while (in >> token) {
        if (token == "timeout" || token[0] == '#') break;
        int value = 0;
        bool pulse = false;
        if (token == "pulse" || token == "space") {
            std::string number;
            if (!(in >> number)) break;
            pulse = token == "pulse";
            value = std::atoi(number.c_str());
        } else if (token[0] == '+' || token[0] == '-') {
            pulse = token[0] == '+';
            value = std::atoi(token.c_str() + 1);
        } else {
            continue;
        }
        if (value <= 0) continue;
        if (raw.empty() && !pulse) continue;  // leading space
        if (pulse != expect_pulse) {
            // Two of the same kind in a row: merge them.
            if (!raw.empty()) raw.back() += value;
            continue;
        }
        raw.push_back(value);
        expect_pulse = !expect_pulse;
    }
    if (!raw.empty() && raw.size() % 2 == 0) raw.pop_back();  // trailing space
    return raw;
}

std::string to_send_file(const Raw& raw) {
    std::string out;
    for (size_t i = 0; i < raw.size(); ++i) {
        out += (i % 2 == 0 ? "pulse " : "space ") + std::to_string(raw[i]) + "\n";
    }
    return out;
}

std::string to_text(const Raw& raw) {
    std::string out;
    for (size_t i = 0; i < raw.size(); ++i) {
        if (i) out += ' ';
        out += (i % 2 == 0 ? '+' : '-') + std::to_string(raw[i]);
    }
    return out;
}

bool decode_nec(const Raw& raw, uint32_t& code) {
    // Leader 9 ms pulse + 4.5 ms space, then 32 bits (562 us pulse, then a
    // 562 us space for 0 or 1687 us for 1), LSB first, then a stop pulse.
    if (raw.size() < 67 || !near(raw[0], 9000, 25) || !near(raw[1], 4500, 25)) return false;
    uint32_t bits = 0;
    for (int bit = 0; bit < 32; ++bit) {
        const int pulse = raw[2 + bit * 2];
        const int space = raw[3 + bit * 2];
        if (!near(pulse, 562, 45)) return false;
        if (near(space, 1687, 35)) bits |= 1u << bit;
        else if (!near(space, 562, 45)) return false;
    }
    // Bytes arrive LSB first: print them in arrival order, each bit-reversed.
    code = 0;
    for (int byte = 0; byte < 4; ++byte) {
        uint8_t b = static_cast<uint8_t>(bits >> (byte * 8));
        uint8_t r = 0;
        for (int i = 0; i < 8; ++i) r |= static_cast<uint8_t>(((b >> i) & 1) << (7 - i));
        code = (code << 8) | r;
    }
    return true;
}

std::string describe(const Raw& raw) {
    uint32_t code = 0;
    char text[32];
    if (decode_nec(raw, code)) {
        std::snprintf(text, sizeof(text), "NEC 0x%08X", code);
    } else {
        std::snprintf(text, sizeof(text), "raw · %zu edges", raw.size());
    }
    return text;
}

} // namespace viewmodel::chips::ir
