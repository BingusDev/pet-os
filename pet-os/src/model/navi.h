/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <string>

namespace model {

// Who the PET belongs to: the Navi's name and picture and the owner's name,
// from [navi] in pet-os.conf. Set once at startup (set_navi) and read by the
// screens and the prompts the chips send to the Navi.
struct NaviIdentity {
    std::string name{"Alice"};  // "Alice"; shown as "ALICE.EXE", "Ask Alice", "Alice: ..."
    std::string owner;          // "Holden"; empty = the greeting and prompts don't name anyone
    std::string picture;        // PNG path; empty = the packaged art
};

const NaviIdentity& navi();
void set_navi(NaviIdentity identity);

// "Alice" (never empty, at most kMaxNaviName characters, printable ASCII only).
std::string navi_name();
// "ALICE.EXE"
std::string navi_exe();
// "Holden", or `fallback` when no owner is set.
std::string owner_or(const std::string& fallback);

// Cleans a configured name for the small screen: printable ASCII, trimmed,
// clipped to `max_len`. Returns "" when nothing is left.
std::string clean_display_name(const std::string& value, size_t max_len);

constexpr size_t kMaxNaviName = 12;
constexpr size_t kMaxOwnerName = 16;

} // namespace model
