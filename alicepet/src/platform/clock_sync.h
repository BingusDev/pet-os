/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <string>

// The CardputerZero has no battery clock. After time without power or network
// its clock is wrong, which pauses RETRO save sync. A time learned from a
// trusted source (a trusted Jack In PC, the RomM server's Date header) is
// handed to [clock] set= (set-clock.sh through sudo).
namespace platform::clock_sync {

// Before this (September 2026) the clock is certainly wrong.
constexpr long long kSaneEpoch = 1790000000;
// More than this apart is worth a correction.
constexpr long long kSlackS = 120;

// Pure rule (for tests): should a clock reading `now` be set to `offered`?
// A trusted source may correct any clock; an untrusted one (a PC nobody
// trusted) only a clock that is certainly wrong.
bool should_set(long long now, long long offered, bool trusted);

// "Sun, 22 Nov 2026 02:19:22 GMT" -> unix seconds, 0 when it doesn't parse.
long long parse_http_date(const std::string& text);

// Applies should_set() to the system clock and, when it says so, runs
// `command <offered>` on a detached thread (at most once a minute).
// Thread-safe. Returns true when the command was started.
bool offer(const std::string& command, long long offered, bool trusted, const char* source);

} // namespace platform::clock_sync
