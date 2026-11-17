/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <string>

// Blocking POSIX network helpers. Call them from worker threads only; the UI
// thread must never wait on the network.
namespace platform::net {

struct HttpResponse {
    bool ok{false};       // transport succeeded and a status line was read
    int status{0};
    std::string body;
    std::string error;    // transport error when !ok
};

// Plain http:// only (the bridge runs over Tailscale, which already encrypts).
// `bearer_token` is sent as "Authorization: Bearer <token>" when non-empty.
HttpResponse http_post_json(const std::string& url, const std::string& json_body,
                            const std::string& bearer_token, int timeout_ms);

enum class ProbeState { Online, Rest, Offline };

struct ProbeResult {
    ProbeState state{ProbeState::Offline};
    int latency_ms{-1};
    std::string detail;   // e.g. "connected", "refused", "timeout", "standby"
};

// ONLINE when a TCP connection to address:port completes within timeout_ms.
// A refused connection still proves the host is up, so it also counts as ONLINE.
ProbeResult probe_tcp(const std::string& address, int port, int timeout_ms);

// PlayStation device discovery (UDP 9302, "SRCH" request): 200 = ONLINE,
// 620 Server Standby = REST, no answer = OFFLINE.
ProbeResult probe_ps5(const std::string& address, int timeout_ms);

// Minimal JSON helpers for the bridge protocol.
std::string json_escape(const std::string& value);
// Extracts a top-level-ish string field ("key": "value") and unescapes it, including \uXXXX.
bool json_string_field(const std::string& json, const std::string& key, std::string& out);
// True if the JSON contains "key": true.
bool json_bool_field(const std::string& json, const std::string& key, bool& out);

} // namespace platform::net
