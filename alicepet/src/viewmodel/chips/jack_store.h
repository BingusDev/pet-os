/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <string>
#include <vector>

// What JACK IN remembers between sessions, as plain files in [jackin] state_dir:
// the PCs Holden trusted with [T] (jack-trusted.conf, 0600) and one text report
// per session (jack-reports/), plus the last scan of each PC for "last visit".
namespace viewmodel::chips::jack_store {

struct TrustedPc {
    std::string token;  // 64 hex characters, made on the PET, kept by the PC app
    std::string host;
    std::string added;  // YYYY-MM-DD
};

constexpr size_t kMaxTrusted = 16;
constexpr size_t kMaxReports = 200;

// 32 random bytes as hex, or "" when the system has no random source.
std::string new_token();
std::vector<TrustedPc> load_trusted(const std::string& dir);
bool save_trusted(const std::string& dir, const std::vector<TrustedPc>& list);
// Index of the entry with this token (compared in constant time), or -1.
int find_trusted(const std::vector<TrustedPc>& list, const std::string& token);

// A host name as a file name: letters, digits, '-' and '_' only.
std::string safe_host(const std::string& host);

// The last scan of a PC, kept for the next visit.
struct LastVisit {
    bool have{false};
    std::string when;  // YYYY-MM-DD
    int cpu{-1};
    int ram{-1};
    int disk_free{-1};
    int startup{-1};
    std::string defender;
};
LastVisit load_last(const std::string& dir, const std::string& host);
bool save_last(const std::string& dir, const std::string& host, const LastVisit& visit);
// "last visit 2026-11-20: RAM 91%, C: 8% free, Defender off"
std::string last_text(const LastVisit& visit, const std::string& disk);

// Writes <dir>/jack-reports/<host>-<YYYYMMDD-HHMMSS>.txt and removes the oldest
// reports past kMaxReports. Returns the path, or "" when it couldn't be written.
std::string write_report(const std::string& dir, const std::string& host, const std::string& text);

std::string today();

} // namespace viewmodel::chips::jack_store
