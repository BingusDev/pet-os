/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstddef>
#include <string>
#include <vector>

// Blocking POSIX network helpers. Call them from worker threads only; the UI
// thread must never wait on the network.
namespace platform::net {

struct HttpResponse {
    bool ok{false};       // transport succeeded and a status line was read
    int status{0};
    std::string body;
    std::string error;    // transport error when !ok
};

// http:// is handled in-process (the bridge runs over Tailscale, which already
// encrypts); https:// is handed to the system curl. `bearer_token` is sent as "Authorization: Bearer <token>" when non-empty.
HttpResponse http_post_json(const std::string& url, const std::string& json_body,
                            const std::string& bearer_token, int timeout_ms);
// Generic request; `headers` are full "Name: value" lines. Replies over max_reply bytes fail.
HttpResponse http_request(const std::string& method, const std::string& url, const std::string& body,
                          const std::vector<std::string>& headers, int timeout_ms,
                          size_t max_reply = 4 * 1024 * 1024);
HttpResponse http_get(const std::string& url, const std::vector<std::string>& headers, int timeout_ms);
// Percent-encodes a query parameter value.
std::string url_encode(const std::string& value);
// Status code from an HTTP/1.x status line at the start of `raw`; 0 when the line is malformed.
int parse_http_status(const std::string& raw);

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

// Escapes a string for a JSON string literal (request bodies).
std::string json_escape(const std::string& value);
// Responses are parsed with platform/json.h.

} // namespace platform::net
