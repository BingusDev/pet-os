/*
 * SPDX-License-Identifier: MIT
 */

#include "net.h"
#include "platform/clock_sync.h"

#include "process.h"

#include <fstream>
#include <filesystem>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0  // macOS desktop build
#endif

namespace platform::net {
namespace {

using Clock = std::chrono::steady_clock;

int elapsed_ms(Clock::time_point since) {
    return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - since).count());
}

int remaining_ms(Clock::time_point deadline) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    return left > 0 ? static_cast<int>(left) : 0;
}

struct AddrList {
    addrinfo* head{nullptr};
    ~AddrList() {
        if (head) {
            freeaddrinfo(head);
        }
    }
};

bool resolve(const std::string& host, int port, int socktype, AddrList& out, std::string& error) {
    addrinfo hints{};
    hints.ai_family = AF_INET;  // tailnet + home LAN are IPv4; avoids slow AAAA fallbacks
    hints.ai_socktype = socktype;
    const std::string service = std::to_string(port);
    const int rc = getaddrinfo(host.c_str(), service.c_str(), &hints, &out.head);
    if (rc != 0 || !out.head) {
        error = std::string("dns: ") + gai_strerror(rc);
        return false;
    }
    return true;
}

// Non-blocking connect bounded by the deadline. Returns the fd (blocking mode
// restored off) or -1 with errno-ish text in `error`. `refused` is set when the
// peer actively rejected the connection.
int connect_with_deadline(const addrinfo* addr, Clock::time_point deadline, std::string& error, bool& refused) {
    refused = false;
    const int fd = socket(addr->ai_family, addr->ai_socktype | SOCK_CLOEXEC, addr->ai_protocol);
    if (fd < 0) {
        error = std::strerror(errno);
        return -1;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
#ifdef SO_NOSIGPIPE
    const int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    int rc = connect(fd, addr->ai_addr, addr->ai_addrlen);
    if (rc != 0 && errno != EINPROGRESS) {
        refused = errno == ECONNREFUSED;
        error = std::strerror(errno);
        close(fd);
        return -1;
    }
    if (rc != 0) {
        pollfd pfd{fd, POLLOUT, 0};
        rc = poll(&pfd, 1, remaining_ms(deadline));
        if (rc <= 0) {
            error = "timeout";
            close(fd);
            return -1;
        }
        int so_error = 0;
        socklen_t len = sizeof(so_error);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len);
        if (so_error != 0) {
            refused = so_error == ECONNREFUSED;
            error = std::strerror(so_error);
            close(fd);
            return -1;
        }
    }
    return fd;
}

bool send_all(int fd, const std::string& data, Clock::time_point deadline, std::string& error) {
    size_t sent = 0;
    while (sent < data.size()) {
        pollfd pfd{fd, POLLOUT, 0};
        if (poll(&pfd, 1, remaining_ms(deadline)) <= 0) {
            error = "send timeout";
            return false;
        }
        const ssize_t n = send(fd, data.data() + sent, data.size() - sent,
#ifdef MSG_NOSIGNAL
                               MSG_NOSIGNAL
#else
                               0
#endif
        );
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
                continue;
            }
            error = std::strerror(errno);
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool parse_url(const std::string& url, std::string& host, int& port, std::string& path) {
    const std::string scheme = "http://";
    if (url.rfind(scheme, 0) != 0) {
        return false;
    }
    std::string rest = url.substr(scheme.size());
    const size_t slash = rest.find('/');
    path = slash == std::string::npos ? "" : rest.substr(slash);
    std::string authority = slash == std::string::npos ? rest : rest.substr(0, slash);
    port = 80;
    const size_t colon = authority.rfind(':');
    if (colon != std::string::npos) {
        port = std::atoi(authority.c_str() + colon + 1);
        authority = authority.substr(0, colon);
    }
    host = authority;
    return !host.empty() && port > 0;
}

std::string decode_chunked_impl(const std::string& body) {
    std::string out;
    size_t pos = 0;
    while (pos < body.size()) {
        const size_t eol = body.find("\r\n", pos);
        if (eol == std::string::npos) {
            break;
        }
        const size_t size = std::strtoull(body.substr(pos, eol - pos).c_str(), nullptr, 16);
        if (size == 0) {
            break;
        }
        // A chunk can't be longer than what is left of the body. Without this a
        // huge size wrapped `pos` back to the start and the loop never ended.
        const size_t data = eol + 2;
        if (size > body.size() - data) {
            out.append(body, data, std::string::npos);  // cut-off reply: keep what arrived
            break;
        }
        out.append(body, data, size);
        pos = data + size + 2;
    }
    return out;
}

} // namespace

HttpResponse http_post_json(const std::string& url, const std::string& json_body,
                            const std::string& bearer_token, int timeout_ms) {
    std::vector<std::string> headers{"Content-Type: application/json"};
    if (!bearer_token.empty()) {
        headers.push_back("Authorization: Bearer " + bearer_token);
    }
    return http_request("POST", url, json_body, headers, timeout_ms);
}

HttpResponse http_get(const std::string& url, const std::vector<std::string>& headers, int timeout_ms) {
    return http_request("GET", url, "", headers, timeout_ms);
}

namespace {

// curl config files quote values; escape backslashes and double quotes.
std::string curl_quote(const std::string& value) {
    std::string out = "\"";
    for (const char c : value) {
        // A line break would end the value and start a new curl option.
        if (c == '\n' || c == '\r' || c == '\0') continue;
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out + "\"";
}

// https:// has no in-process TLS client: hand the request to the system curl.
// Headers (tokens) go through a 0600 temp config file, not argv, so they stay
// out of `ps`; the body goes through a 0600 temp file too.
HttpResponse https_via_curl(const std::string& method, const std::string& url, const std::string& body,
                            const std::vector<std::string>& headers, int timeout_ms, size_t max_reply,
                            const std::string& output = "") {
    HttpResponse response;
    char path[] = "/tmp/pet-os-curl-XXXXXX";
    const int fd = mkstemp(path);
    if (fd < 0) {
        response.error = "temp file";
        return response;
    }
    // The body goes in its own 0600 file (it can be binary: save-file uploads).
    char body_path[] = "/tmp/pet-os-body-XXXXXX";
    bool body_file = false;
    if (!body.empty()) {
        const int body_fd = mkstemp(body_path);
        body_file = body_fd >= 0 &&
                    write(body_fd, body.data(), body.size()) == static_cast<ssize_t>(body.size());
        if (body_fd >= 0) close(body_fd);
        if (!body_file) {
            if (body_fd >= 0) unlink(body_path);
            close(fd);
            unlink(path);
            response.error = "temp write";
            return response;
        }
    }
    std::string config = "url = " + curl_quote(url) + "\nrequest = " + curl_quote(method) + "\n";
    for (const auto& header : headers) config += "header = " + curl_quote(header) + "\n";
    config += "compressed\n";  // RomM's JSON lists gzip ~2x smaller over slow links
    config += "globoff\n";     // [ ] { } in a URL (ROM file names) are literal, not curl ranges
    if (body_file) config += "data-binary = " + curl_quote(std::string("@") + body_path) + "\n";
    // A download goes straight to its file: the body never sits in this process's memory.
    if (!output.empty()) {
        config += "output = " + curl_quote(output) + "\nmax-filesize = " + std::to_string(max_reply) + "\n";
    }
    const ssize_t written = write(fd, config.data(), config.size());
    close(fd);
    if (written != static_cast<ssize_t>(config.size())) {
        unlink(path);
        if (body_file) unlink(body_path);
        response.error = "temp write";
        return response;
    }
    std::string out;
    // Room for the largest reply the caller accepts plus the status marker;
    // anything that fills it is reported as too large, not as a curl failure.
    const size_t capture_cap = (output.empty() ? max_reply : 4096) + 64;
    const int rc = proc::run_capture({"curl", "-sS", "--max-time", std::to_string(std::max(1, timeout_ms / 1000)),
                                      "-K", path, "-w", "\n@@HTTP@@%{http_code} %header{date}"},
                                     out, timeout_ms + 2000, false, capture_cap);
    unlink(path);
    if (body_file) unlink(body_path);
    if (rc == 0 && out.size() >= capture_cap) {
        response.error = "reply too large";
        return response;
    }
    static constexpr char kMarker[] = "\n@@HTTP@@";
    const size_t marker = out.rfind(kMarker);
    if (rc != 0 || marker == std::string::npos) {
        response.error = rc == -1 ? "curl missing" : rc == -2 ? "timeout" : rc == 63 ? "reply too large"
                                                                          : "curl exit " + std::to_string(rc);
        return response;
    }
    response.status = std::atoi(out.c_str() + marker + sizeof(kMarker) - 1);  // was +10: "200" read as 0
    // After the status: the server's Date header (an older curl prints the format text, which parses as 0).
    if (const size_t space = out.find(' ', marker + sizeof(kMarker) - 1); space != std::string::npos) {
        response.date = clock_sync::parse_http_date(out.substr(space + 1));
    }
    out.resize(marker);
    response.body = std::move(out);  // no second copy of a large download
    if (response.body.size() > max_reply) {
        response.error = "reply too large";
        return response;
    }
    response.ok = response.status > 0;
    if (!response.ok) response.error = "no http status";
    return response;
}

} // namespace

int parse_http_status(const std::string& raw) {
    if (raw.rfind("HTTP/", 0) != 0) return 0;
    const size_t line_end = raw.find_first_of("\r\n");
    const size_t space = raw.find(' ');
    // The space must sit on the status line; find(' ') alone can land in a later header or body.
    if (space == std::string::npos || space > line_end) return 0;
    return std::atoi(raw.c_str() + space + 1);
}

HttpResponse http_download(const std::string& url, const std::vector<std::string>& headers, const std::string& path,
                           int timeout_ms, size_t max_bytes) {
    HttpResponse response = https_via_curl("GET", url, "", headers, timeout_ms, max_bytes, path);
    std::error_code ec;
    if (!response.ok && response.error == "curl missing" && url.rfind("http://", 0) == 0) {
        // No curl on this system: the in-process client, which holds the file in memory once.
        response = http_request("GET", url, "", headers, timeout_ms, max_bytes);
        if (response.ok && response.status == 200) {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out.write(response.body.data(), static_cast<std::streamsize>(response.body.size()));
            out.flush();
            if (!out) {
                response.ok = false;
                response.error = "can't write " + path;
            }
        }
        response.body.clear();
    }
    // curl's max-filesize only stops a reply that says how long it is: check what arrived.
    if (response.ok && response.status == 200) {
        const auto size = std::filesystem::file_size(path, ec);
        if (ec || size > max_bytes) {
            response.ok = false;
            response.error = ec ? "nothing was saved" : "reply too large";
        }
    }
    if (!response.ok || response.status != 200) std::filesystem::remove(path, ec);
    return response;
}

HttpResponse http_request(const std::string& method, const std::string& url, const std::string& body,
                          const std::vector<std::string>& headers, int timeout_ms, size_t max_reply) {
    if (url.rfind("https://", 0) == 0) {
        return https_via_curl(method, url, body, headers, timeout_ms, max_reply);
    }
    HttpResponse response;
    std::string host;
    std::string path;
    int port = 0;
    if (!parse_url(url, host, port, path)) {
        response.error = "bad url (http:// only)";
        return response;
    }
    if (path.empty()) {
        path = "/";
    }

    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    AddrList addrs;
    if (!resolve(host, port, SOCK_STREAM, addrs, response.error)) {
        return response;
    }
    int fd = -1;
    bool refused = false;
    for (const addrinfo* a = addrs.head; a && fd < 0; a = a->ai_next) {
        fd = connect_with_deadline(a, deadline, response.error, refused);
    }
    if (fd < 0) {
        response.error = "connect: " + response.error;
        return response;
    }

    std::string request = method + " " + path + " HTTP/1.1\r\nHost: " + host + ":" + std::to_string(port) +
                          "\r\nUser-Agent: PET-os\r\nAccept: application/json\r\nConnection: close\r\n";
    if (method != "GET" || !body.empty()) {
        request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    for (const auto& header : headers) {
        request += header + "\r\n";
    }
    request += "\r\n" + body;
    if (!send_all(fd, request, deadline, response.error)) {
        close(fd);
        return response;
    }

    std::string raw;
    char buffer[4096];
    while (true) {
        pollfd pfd{fd, POLLIN, 0};
        const int rc = poll(&pfd, 1, remaining_ms(deadline));
        if (rc <= 0) {
            response.error = "reply timeout";
            close(fd);
            return response;
        }
        const ssize_t n = recv(fd, buffer, sizeof(buffer), 0);
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
            continue;
        }
        if (n <= 0) {
            break;
        }
        raw.append(buffer, static_cast<size_t>(n));
        if (raw.size() > max_reply) {
            response.error = "reply too large";
            close(fd);
            return response;
        }
    }
    close(fd);

    const size_t header_end = raw.find("\r\n\r\n");
    if (raw.rfind("HTTP/", 0) != 0 || header_end == std::string::npos) {
        response.error = raw.empty() ? "empty reply" : "bad http reply";
        return response;
    }
    response.status = parse_http_status(raw);
    if (response.status <= 0) {
        response.error = "no http status";
        return response;
    }
    std::string reply_headers = raw.substr(0, header_end);
    for (char& c : reply_headers) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (const size_t at = reply_headers.find("\r\ndate:"); at != std::string::npos) {
        const size_t from = at + 7;
        // The original text: the lower-cased copy has "nov" where the parser wants "Nov".
        response.date = clock_sync::parse_http_date(raw.substr(from, raw.find("\r\n", from) - from));
    }
    response.body = raw.substr(header_end + 4);
    if (reply_headers.find("transfer-encoding: chunked") != std::string::npos) {
        response.body = decode_chunked_impl(response.body);
    }
    response.ok = true;
    return response;
}

ProbeResult probe_tcp(const std::string& address, int port, int timeout_ms) {
    ProbeResult result;
    const auto start = Clock::now();
    const auto deadline = start + std::chrono::milliseconds(timeout_ms);
    AddrList addrs;
    std::string error;
    if (!resolve(address, port, SOCK_STREAM, addrs, error)) {
        result.detail = error;
        return result;
    }
    bool refused = false;
    const int fd = connect_with_deadline(addrs.head, deadline, error, refused);
    if (fd >= 0) {
        close(fd);
        result.state = ProbeState::Online;
        result.latency_ms = elapsed_ms(start);
        result.detail = "port " + std::to_string(port) + " open";
    } else if (refused) {
        result.state = ProbeState::Online;
        result.latency_ms = elapsed_ms(start);
        result.detail = "up, port " + std::to_string(port) + " refused";
    } else {
        result.detail = error;
    }
    return result;
}

ProbeResult probe_ps5(const std::string& address, int timeout_ms) {
    ProbeResult result;
    const auto start = Clock::now();
    const auto deadline = start + std::chrono::milliseconds(timeout_ms);
    AddrList addrs;
    std::string error;
    if (!resolve(address, 9302, SOCK_DGRAM, addrs, error)) {
        result.detail = error;
        return result;
    }
    const int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        result.detail = std::strerror(errno);
        return result;
    }
    static const char kSearch[] = "SRCH * HTTP/1.1\ndevice-discovery-protocol-version:00030010\n";
    std::string reply;
    // UDP can drop; resend a few times within the timeout.
    for (int attempt = 0; attempt < 3 && reply.empty() && remaining_ms(deadline) > 0; ++attempt) {
        sendto(fd, kSearch, sizeof(kSearch) - 1, 0, addrs.head->ai_addr, addrs.head->ai_addrlen);
        pollfd pfd{fd, POLLIN, 0};
        const int wait = std::min(remaining_ms(deadline), timeout_ms / 3 + 1);
        if (poll(&pfd, 1, wait) > 0) {
            char buffer[1024];
            const ssize_t n = recv(fd, buffer, sizeof(buffer) - 1, 0);
            if (n > 0) {
                reply.assign(buffer, static_cast<size_t>(n));
            }
        }
    }
    close(fd);
    if (reply.empty()) {
        result.detail = "no discovery reply";
        return result;
    }
    result.latency_ms = elapsed_ms(start);
    if (reply.find(" 620") != std::string::npos) {
        result.state = ProbeState::Rest;
        result.detail = "rest mode (standby)";
    } else {
        result.state = ProbeState::Online;
        result.detail = "awake";
    }
    const size_t name = reply.find("host-name:");
    if (name != std::string::npos) {
        const size_t eol = reply.find('\n', name);
        result.detail += ", " + reply.substr(name + 10, eol == std::string::npos ? std::string::npos : eol - name - 10);
    }
    return result;
}

std::string json_escape(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (const char ch : value) {
        const auto c = static_cast<unsigned char>(ch);
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    return out;
}

std::string url_encode(const std::string& value) {
    static const char kHex[] = "0123456789ABCDEF";
    std::string out;
    for (const unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 15];
        }
    }
    return out;
}

std::string decode_chunked(const std::string& body) { return decode_chunked_impl(body); }

} // namespace platform::net
