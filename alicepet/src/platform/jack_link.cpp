/*
 * SPDX-License-Identifier: MIT
 */

#include "platform/jack_link.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#ifndef SOCK_CLOEXEC
// macOS (desktop builds): no SOCK_CLOEXEC/SOCK_NONBLOCK flags or accept4().
#define SOCK_CLOEXEC 0x10000000
#define SOCK_NONBLOCK 0x20000000
namespace {
int accept4(int fd, sockaddr* addr, socklen_t* len, int flags) {
    const int client = ::accept(fd, addr, len);
    if (client < 0) return client;
    fcntl(client, F_SETFD, FD_CLOEXEC);
    if (flags & SOCK_NONBLOCK) fcntl(client, F_SETFL, fcntl(client, F_GETFL, 0) | O_NONBLOCK);
    return client;
}
int portable_socket(int domain, int type, int protocol) {
    const int fd = ::socket(domain, type & ~(SOCK_CLOEXEC | SOCK_NONBLOCK), protocol);
    if (fd >= 0) fcntl(fd, F_SETFD, FD_CLOEXEC);
    return fd;
}
} // namespace
#define socket portable_socket
#endif

namespace platform::jack {
namespace {

void set_nonblocking(int fd, bool on) {
    const int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, on ? flags | O_NONBLOCK : flags & ~O_NONBLOCK);
}

// Returns a listening socket or -1 (error in `error`).
int open_listener(const std::string& address, int port, std::string& error) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, address.c_str(), &addr.sin_addr) != 1) {
        error = "bad address " + address;
        return -1;
    }
    const int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        error = std::strerror(errno);
        return -1;
    }
    const int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(fd, 8) != 0) {
        error = address + ":" + std::to_string(port) + ": " + std::strerror(errno);
        ::close(fd);
        return -1;
    }
    set_nonblocking(fd, true);
    return fd;
}

// Waits up to `timeout_ms` for `fd` to be readable; false on timeout or `stop`.
bool wait_readable(int fd, int timeout_ms, const std::atomic<bool>& stop) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (!stop) {
        pollfd p{fd, POLLIN, 0};
        const int left = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count());
        if (left <= 0) return false;
        const int r = ::poll(&p, 1, left < 200 ? left : 200);
        if (r > 0) return true;
        if (r < 0 && errno != EINTR) return false;
    }
    return false;
}

bool send_all(int fd, const char* data, size_t size) {
    while (size > 0) {
        const ssize_t n = ::send(fd, data, size, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                pollfd p{fd, POLLOUT, 0};
                if (::poll(&p, 1, 2000) <= 0) return false;
                continue;
            }
            return false;
        }
        data += n;
        size -= static_cast<size_t>(n);
    }
    return true;
}

} // namespace

// --- LineServer --------------------------------------------------------------

LineServer::~LineServer() { close(); }

std::string LineServer::listen(const std::string& address, int port) {
    close();
    std::string error;
    listen_fd_ = open_listener(address, port, error);
    return error;
}

void LineServer::close() {
    drop_client();
    if (listen_fd_ >= 0) ::close(listen_fd_);
    listen_fd_ = -1;
}

void LineServer::drop_client() {
    if (client_fd_ >= 0) {
        ::shutdown(client_fd_, SHUT_RDWR);
        ::close(client_fd_);
    }
    client_fd_ = -1;
    buffer_.clear();
    out_.clear();
}

LineServer::Poll LineServer::poll() {
    Poll out;
    if (listen_fd_ < 0) return out;
    for (;;) {
        const int fd = accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (fd < 0) break;
        // Newest wins (the app relaunched elevated, or was reopened).
        drop_client();
        client_fd_ = fd;
        const int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
        out.connected = true;
    }
    if (client_fd_ < 0) return out;
    if (!out_.empty() && !flush_some()) {
        out.disconnected = !out.connected;
        out.connected = false;
        return out;
    }
    char chunk[4096];
    for (;;) {
        const ssize_t n = ::recv(client_fd_, chunk, sizeof(chunk), 0);
        if (n > 0) {
            buffer_.append(chunk, static_cast<size_t>(n));
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if (n < 0 && errno == EINTR) continue;
        drop_client();  // closed or failed
        out.disconnected = !out.connected;
        out.connected = false;
        return out;
    }
    size_t start = 0;
    for (size_t nl; (nl = buffer_.find('\n', start)) != std::string::npos; start = nl + 1) {
        std::string line = buffer_.substr(start, nl - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) out.lines.push_back(std::move(line));
    }
    buffer_.erase(0, start);
    if (buffer_.size() > kMaxLine) {
        drop_client();
        out.disconnected = true;
    }
    return out;
}

// Never blocks the UI thread: what the socket won't take now waits in `out_` and
// goes out from poll(). A PC that stops reading (a Yes/No box is open there) only
// costs it mirror frames, not the link, until the backlog passes kMaxBacklog.
bool LineServer::send(const std::string& line, bool droppable) {
    if (client_fd_ < 0) return false;
    if (droppable && out_.size() > kDropBacklog) return false;
    out_.append(line);
    out_.push_back('\n');
    if (!flush_some()) return false;
    if (out_.size() > kMaxBacklog) {
        drop_client();
        return false;
    }
    return true;
}

// Writes as much of the backlog as the socket takes right now. False (and the
// client dropped) when the connection failed.
bool LineServer::flush_some() {
    while (client_fd_ >= 0 && !out_.empty()) {
        const ssize_t n = ::send(client_fd_, out_.data(), out_.size(), MSG_NOSIGNAL);
        if (n > 0) {
            out_.erase(0, static_cast<size_t>(n));
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;
        drop_client();
        return false;
    }
    return client_fd_ >= 0;
}

// Best effort before a deliberate close (the jack-out line): waits up to
// `timeout_ms` in total for the backlog to go out.
void LineServer::flush(int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (client_fd_ >= 0 && !out_.empty()) {
        if (!flush_some() || out_.empty()) return;
        const int left = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count());
        if (left <= 0) return;
        pollfd p{client_fd_, POLLOUT, 0};
        if (::poll(&p, 1, left) <= 0) return;
    }
}

// --- Relay -------------------------------------------------------------------

struct Relay::State {
    std::atomic<bool> stop{false};
    int proxy_fd{-1};
    int tunnel_fd{-1};
    OpenFn open;
    std::mutex mutex;
    std::condition_variable cv;
    std::map<int, int> pending;  // id -> tunnel fd (-1 waiting, -2 failed)
    std::set<int> fds;           // every open piped socket, shut down on stop
    int next_id{1};
    std::atomic<int> active{0};
    std::atomic<int> threads{0};

    void track(int fd) {
        std::lock_guard<std::mutex> lock(mutex);
        fds.insert(fd);
    }
    void untrack_close(int fd) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            fds.erase(fd);
        }
        ::close(fd);
    }
};

namespace {

void pipe_both(int a, int b, const std::atomic<bool>& stop) {
    char buf[16384];
    for (;;) {
        if (stop) return;
        pollfd p[2] = {{a, POLLIN, 0}, {b, POLLIN, 0}};
        const int r = ::poll(p, 2, 500);
        if (r < 0 && errno != EINTR) return;
        if (r <= 0) continue;
        for (int i = 0; i < 2; ++i) {
            if (!(p[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            const int from = i == 0 ? a : b;
            const int to = i == 0 ? b : a;
            const ssize_t n = ::recv(from, buf, sizeof(buf), 0);
            if (n <= 0) return;
            if (!send_all(to, buf, static_cast<size_t>(n))) return;
        }
    }
}

} // namespace

Relay::Relay() = default;
Relay::~Relay() { stop(); }

bool Relay::parse_request(const std::string& head, std::string& host, int& port, bool& is_connect) {
    const auto line_end = head.find("\r\n");
    const std::string line = head.substr(0, line_end);
    const auto sp1 = line.find(' ');
    const auto sp2 = sp1 == std::string::npos ? std::string::npos : line.find(' ', sp1 + 1);
    if (sp2 == std::string::npos) return false;
    const std::string method = line.substr(0, sp1);
    std::string target = line.substr(sp1 + 1, sp2 - sp1 - 1);
    is_connect = method == "CONNECT";
    if (!is_connect) {
        if (target.rfind("http://", 0) != 0) return false;  // only absolute http:// URLs
        target = target.substr(7);
        target = target.substr(0, target.find('/'));
    }
    std::string port_text = is_connect ? "" : "80";
    if (!target.empty() && target[0] == '[') {  // [v6]:port
        const auto close = target.find(']');
        if (close == std::string::npos) return false;
        host = target.substr(1, close - 1);
        if (close + 1 < target.size() && target[close + 1] == ':') port_text = target.substr(close + 2);
    } else {
        const auto colon = target.rfind(':');
        host = target.substr(0, colon);
        if (colon != std::string::npos) port_text = target.substr(colon + 1);
    }
    if (host.empty() || port_text.empty() || port_text.find_first_not_of("0123456789") != std::string::npos) return false;
    port = std::atoi(port_text.c_str());
    return port > 0 && port < 65536;
}

std::string Relay::start(const std::string& proxy_address, int proxy_port, const std::string& tunnel_address,
                         int tunnel_port, OpenFn open) {
    stop();
    auto state = std::make_shared<State>();
    std::string error;
    state->proxy_fd = open_listener(proxy_address, proxy_port, error);
    if (state->proxy_fd < 0) return error;
    state->tunnel_fd = open_listener(tunnel_address, tunnel_port, error);
    if (state->tunnel_fd < 0) {
        ::close(state->proxy_fd);
        return error;
    }
    state->open = std::move(open);
    state_ = state;

    // Tunnels dialled back by the PC: "tunnel <id>\n" (or "tunnel <id> fail\n").
    std::thread([state]() {
        while (wait_readable(state->tunnel_fd, 1 << 30, state->stop)) {
            const int fd = accept4(state->tunnel_fd, nullptr, nullptr, SOCK_CLOEXEC);
            if (fd < 0) continue;
            std::string line;
            char c;
            while (line.size() < 64 && wait_readable(fd, 3000, state->stop) && ::recv(fd, &c, 1, 0) == 1 && c != '\n') {
                line += c;
            }
            int id = 0;
            const bool failed = line.find("fail") != std::string::npos;
            if (line.rfind("tunnel ", 0) == 0) id = std::atoi(line.c_str() + 7);
            std::lock_guard<std::mutex> lock(state->mutex);
            auto it = state->pending.find(id);
            if (it == state->pending.end() || it->second != -1 || failed) {
                if (it != state->pending.end() && it->second == -1) it->second = -2;
                ::close(fd);
            } else {
                it->second = fd;
            }
            state->cv.notify_all();
        }
    }).detach();

    // Proxy clients on the PET.
    std::thread([state]() {
        while (wait_readable(state->proxy_fd, 1 << 30, state->stop)) {
            const int client = accept4(state->proxy_fd, nullptr, nullptr, SOCK_CLOEXEC);
            if (client < 0) continue;
            std::thread([state, client]() {
                state->track(client);
                std::string head;
                char buf[4096];
                while (head.find("\r\n\r\n") == std::string::npos && head.size() < 16384 &&
                       wait_readable(client, 10000, state->stop)) {
                    const ssize_t n = ::recv(client, buf, sizeof(buf), 0);
                    if (n <= 0) break;
                    head.append(buf, static_cast<size_t>(n));
                }
                std::string host;
                int port = 0;
                bool is_connect = false;
                if (head.find("\r\n\r\n") == std::string::npos || !parse_request(head, host, port, is_connect)) {
                    static const char kBad[] = "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n";
                    send_all(client, kBad, sizeof(kBad) - 1);
                    state->untrack_close(client);
                    return;
                }
                int id;
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    id = state->next_id++;
                    state->pending[id] = -1;
                }
                state->open(id, host, port);
                int tunnel = -1;
                {
                    std::unique_lock<std::mutex> lock(state->mutex);
                    state->cv.wait_for(lock, std::chrono::seconds(15),
                                       [&]() { return state->stop || state->pending[id] != -1; });
                    tunnel = state->pending[id];
                    state->pending.erase(id);
                }
                if (tunnel < 0) {
                    static const char kFail[] = "HTTP/1.1 502 Bad Gateway\r\nConnection: close\r\n\r\nPC could not connect\n";
                    send_all(client, kFail, sizeof(kFail) - 1);
                    state->untrack_close(client);
                    return;
                }
                state->track(tunnel);
                ++state->active;
                bool ok = true;
                if (is_connect) {
                    static const char kOk[] = "HTTP/1.1 200 Connection established\r\n\r\n";
                    const size_t end = head.find("\r\n\r\n") + 4;
                    ok = send_all(client, kOk, sizeof(kOk) - 1) &&
                         (end >= head.size() || send_all(tunnel, head.data() + end, head.size() - end));
                } else {
                    ok = send_all(tunnel, head.data(), head.size());  // absolute-form is valid HTTP/1.1
                }
                if (ok) pipe_both(client, tunnel, state->stop);
                --state->active;
                state->untrack_close(tunnel);
                state->untrack_close(client);
            }).detach();
        }
    }).detach();
    return "";
}

void Relay::fail(int id) {
    if (!state_) return;
    std::lock_guard<std::mutex> lock(state_->mutex);
    auto it = state_->pending.find(id);
    if (it != state_->pending.end() && it->second == -1) it->second = -2;
    state_->cv.notify_all();
}

void Relay::stop() {
    if (!state_) return;
    auto state = std::move(state_);
    state->stop = true;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        for (int fd : state->fds) ::shutdown(fd, SHUT_RDWR);
        for (auto& [id, fd] : state->pending) {
            if (fd >= 0) ::close(fd);
            fd = -2;
        }
        state->cv.notify_all();
    }
    // The accept threads notice `stop` within 200 ms; closing their sockets
    // now would race with accept(), so shut them down and let the last owner close.
    ::shutdown(state->proxy_fd, SHUT_RDWR);
    ::shutdown(state->tunnel_fd, SHUT_RDWR);
    std::thread([state]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        ::close(state->proxy_fd);
        ::close(state->tunnel_fd);
    }).detach();
}

bool Relay::running() const { return state_ != nullptr; }
int Relay::active() const { return state_ ? state_->active.load() : 0; }

bool base64_decode(const std::string& text, std::string& out) {
    out.clear();
    out.reserve(text.size() * 3 / 4);
    unsigned value = 0;
    int bits = 0;
    for (const char ch : text) {
        int d;
        if (ch >= 'A' && ch <= 'Z') d = ch - 'A';
        else if (ch >= 'a' && ch <= 'z') d = ch - 'a' + 26;
        else if (ch >= '0' && ch <= '9') d = ch - '0' + 52;
        else if (ch == '+') d = 62;
        else if (ch == '/') d = 63;
        else if (ch == '=') break;
        else if (ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t') continue;
        else return false;
        value = (value << 6) | static_cast<unsigned>(d);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((value >> bits) & 0xff);
        }
    }
    return true;
}

namespace {
std::atomic<bool> g_proxy_active{false};
} // namespace

bool proxy_active() { return g_proxy_active.load(); }

void set_proxy_env(const std::string& proxy_url) {
    g_proxy_active = !proxy_url.empty();
    for (const char* name : {"http_proxy", "https_proxy", "HTTPS_PROXY"}) {
        if (proxy_url.empty()) ::unsetenv(name);
        else ::setenv(name, proxy_url.c_str(), 1);
    }
    // Tailscale, the USB link and the local network stay direct.
    if (proxy_url.empty()) ::unsetenv("no_proxy");
    else ::setenv("no_proxy", "localhost,127.0.0.1,.ts.net,100.64.0.0/10,10.0.0.0/8,172.16.0.0/12,192.168.0.0/16", 1);
}

} // namespace platform::jack
