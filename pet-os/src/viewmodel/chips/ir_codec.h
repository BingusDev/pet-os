/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

// IR signal helpers for the REMOTE chip. A signal is a list of microsecond
// durations, alternating pulse, space, pulse, ... (always starting and ending
// with a pulse), which is what LIRC's `ir-ctl` records and sends.
namespace viewmodel::chips::ir {

using Raw = std::vector<int>;

// Parses `ir-ctl -r` output: "+9024 -4473 +598 ..." or "pulse 9024" / "space 4473"
// lines; "timeout" and "#" comments end or are skipped. Leading spaces and a
// trailing space are dropped so the result starts and ends with a pulse.
Raw parse_ir_ctl(const std::string& text);
// "pulse N\nspace N\n..." for `ir-ctl --send=<file>`.
std::string to_send_file(const Raw& raw);
// Compact storage form: "+9024 -4473 +598 ...".
std::string to_text(const Raw& raw);

// NEC (most TVs, projectors): returns true and the 32-bit code (first byte
// received in the top byte, as remotes and LIRC tables print it: 0x20DF10EF).
bool decode_nec(const Raw& raw, uint32_t& code);
// "NEC 0x20DF10EF" or "raw · 67 edges".
std::string describe(const Raw& raw);

} // namespace viewmodel::chips::ir
