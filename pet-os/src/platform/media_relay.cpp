/*
 * SPDX-License-Identifier: MIT
 */

#include "platform/media_relay.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>

namespace platform::jack {
namespace {

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

int make_pipe(int fds[2]) {
#ifdef __linux__
    return ::pipe2(fds, O_CLOEXEC);
#else
    if (::pipe(fds) != 0) return -1;
    ::fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    ::fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    return 0;
#endif
}

int accept_cloexec(int fd) {
#ifdef __linux__
    return ::accept4(fd, nullptr, nullptr, SOCK_CLOEXEC);
#else
    const int c = ::accept(fd, nullptr, nullptr);
    if (c >= 0) ::fcntl(c, F_SETFD, FD_CLOEXEC);
    return c;
#endif
}

constexpr size_t kMaxHead = 32 * 1024;
constexpr size_t kMaxBody = 16 * 1024 * 1024;
constexpr const char* kCookie = "pet-os_relay";

std::string lower(std::string v) {
    for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return v;
}

std::string trim(const std::string& v) {
    const auto a = v.find_first_not_of(" \t");
    if (a == std::string::npos) return "";
    return v.substr(a, v.find_last_not_of(" \t\r") - a + 1);
}

std::string random_token() {
    unsigned char bytes[16] = {};
    bool ok = false;
    if (FILE* f = std::fopen("/dev/urandom", "rb")) {
        ok = std::fread(bytes, 1, sizeof(bytes), f) == sizeof(bytes);
        std::fclose(f);
    }
    if (!ok) return "";
    static const char* kHex = "0123456789abcdef";
    std::string out;
    for (unsigned char b : bytes) {
        out += kHex[b >> 4];
        out += kHex[b & 15];
    }
    return out;
}

std::string url_decode(const std::string& v) {
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] == '%' && i + 2 < v.size() && std::isxdigit(static_cast<unsigned char>(v[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(v[i + 2]))) {
            out += static_cast<char>(std::stoi(v.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else {
            out += v[i] == '+' ? ' ' : v[i];
        }
    }
    return out;
}

std::string url_encode(const std::string& v) {
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : v) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 15];
        }
    }
    return out;
}

bool send_all(int fd, const char* data, size_t size) {
    while (size > 0) {
        const ssize_t n = ::send(fd, data, size, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        data += n;
        size -= static_cast<size_t>(n);
    }
    return true;
}

void send_status(int fd, int code, const char* reason, const std::string& extra = "") {
    const std::string body = std::to_string(code) + " " + reason + "\n";
    const std::string head = "HTTP/1.1 " + std::to_string(code) + " " + reason +
                             "\r\nContent-Type: text/plain\r\nContent-Length: " + std::to_string(body.size()) + "\r\n" +
                             extra + "Connection: close\r\n\r\n";
    send_all(fd, head.data(), head.size());
    send_all(fd, body.data(), body.size());
}

// A curl config value: quoted, with backslashes and quotes escaped.
std::string config_line(const char* name, const std::string& value) {
    std::string out = std::string(name) + " = \"";
    for (char c : value) {
        if (c == '\\' || c == '"') out += '\\';
        if (c == '\r' || c == '\n') continue;
        out += c;
    }
    return out + "\"\n";
}

struct Request {
    std::string method;
    std::string target;
    std::vector<std::pair<std::string, std::string>> headers;  // name as sent, value
    std::string body;

    std::string header(const std::string& name) const {
        for (const auto& h : headers) {
            if (lower(h.first) == name) return h.second;
        }
        return "";
    }
};

// Reads one request head (and a Content-Length body). False on a bad or slow request.
bool read_request(int fd, Request& req, std::string& rest) {
    std::string buf;
    char chunk[4096];
    size_t end = std::string::npos;
    while ((end = buf.find("\r\n\r\n")) == std::string::npos) {
        if (buf.size() > kMaxHead) return false;
        const ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        buf.append(chunk, static_cast<size_t>(n));
    }
    std::istringstream lines(buf.substr(0, end));
    std::string line;
    if (!std::getline(lines, line)) return false;
    std::istringstream first(line);
    std::string version;
    first >> req.method >> req.target >> version;
    if (req.method.empty() || req.target.empty() || req.target[0] != '/') return false;
    while (std::getline(lines, line)) {
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        req.headers.emplace_back(trim(line.substr(0, colon)), trim(line.substr(colon + 1)));
    }
    rest = buf.substr(end + 4);
    if (!req.header("transfer-encoding").empty()) return false;  // chunked uploads: not needed
    const std::string length = req.header("content-length");
    if (!length.empty()) {
        const long long want = std::atoll(length.c_str());
        if (want < 0 || static_cast<size_t>(want) > kMaxBody) return false;
        req.body = rest.substr(0, static_cast<size_t>(want));
        while (req.body.size() < static_cast<size_t>(want)) {
            const ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return false;
            req.body.append(chunk, static_cast<size_t>(n));
        }
        req.body.resize(static_cast<size_t>(want));
    }
    return true;
}

} // namespace

struct MediaRelay::State {
    struct File {
        std::string url;
        std::vector<std::string> headers;
    };
    std::mutex mutex;
    std::map<std::string, File> files;
    std::string site_origin;
    std::vector<std::string> site_headers;
    std::string site_token;
    std::atomic<bool> stopping{false};
    std::atomic<int> active{0};
    int listen_fd{-1};
    int port{0};
    std::thread thread;

    static void serve(std::shared_ptr<State> self);
    void handle(int fd);
    void forward(int fd, const Request& req, const std::string& url, const std::vector<std::string>& extra,
                 bool site, const std::string& origin);
};

MediaRelay::MediaRelay() : state_(std::make_shared<State>()) {}

MediaRelay::~MediaRelay() { stop(); }

std::string MediaRelay::start(const std::string& address, int port) {
    if (running()) return "";
    stop();
    state_ = std::make_shared<State>();
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0) ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    if (fd < 0) return std::strerror(errno);
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (::inet_pton(AF_INET, address.c_str(), &addr.sin_addr) != 1) {
        ::close(fd);
        return "bad address " + address;
    }
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(fd, 16) != 0) {
        const std::string error = std::strerror(errno);
        ::close(fd);
        return error;
    }
    state_->listen_fd = fd;
    state_->port = port;
    auto state = state_;
    state_->thread = std::thread([state] { State::serve(state); });
    return "";
}

void MediaRelay::stop() {
    auto state = state_;
    if (!state || state->listen_fd < 0) return;
    state->stopping = true;
    if (state->thread.joinable()) state->thread.join();
    ::close(state->listen_fd);
    state->listen_fd = -1;
    std::lock_guard<std::mutex> lock(state->mutex);
    state->files.clear();
    state->site_token.clear();
}

bool MediaRelay::running() const { return state_ && state_->listen_fd >= 0 && !state_->stopping; }
int MediaRelay::port() const { return state_ ? state_->port : 0; }
int MediaRelay::active() const { return state_ ? state_->active.load() : 0; }

std::string MediaRelay::add_file(const std::string& upstream_url, const std::vector<std::string>& headers,
                                 const std::string& name) {
    const std::string token = random_token();
    if (token.empty()) return "";
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->files.size() > 64) state_->files.erase(state_->files.begin());
    state_->files[token] = {upstream_url, headers};
    return "/m/" + token + "/" + url_encode(name);
}

std::string MediaRelay::set_site(const std::string& origin, const std::vector<std::string>& headers,
                                 const std::string& path) {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->site_token.empty() || state_->site_origin != origin) {
        state_->site_token = random_token();
        if (state_->site_token.empty()) return "";
    }
    state_->site_origin = origin;
    state_->site_headers = headers;
    std::string go;
    for (unsigned char c : path) {
        if (std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.') {
            go += static_cast<char>(c);
        } else {
            char hex[4];
            std::snprintf(hex, sizeof(hex), "%%%02X", c);
            go += hex;
        }
    }
    return "/__pet/" + state_->site_token + "?go=" + go;
}

void MediaRelay::State::serve(std::shared_ptr<State> self) {
    const int listen_fd = self->listen_fd;
    while (!self->stopping) {
        pollfd p{listen_fd, POLLIN, 0};
        if (::poll(&p, 1, 300) <= 0) continue;
        const int fd = accept_cloexec(listen_fd);
        if (fd < 0) continue;
        timeval tv{20, 0};
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ++self->active;
        // Each request owns a thread for its whole stream (a video can run for hours).
        std::thread([self, fd] {
            self->handle(fd);
            ::close(fd);
            --self->active;
        }).detach();
    }
}

void MediaRelay::State::handle(int fd) {
    Request req;
    std::string rest;
    if (!read_request(fd, req, rest)) {
        send_status(fd, 400, "Bad Request");
        return;
    }
    const std::string& target = req.target;
    if (target.rfind("/m/", 0) == 0) {
        const auto slash = target.find('/', 3);
        const std::string token = target.substr(3, slash == std::string::npos ? std::string::npos : slash - 3);
        File file;
        {
            std::lock_guard<std::mutex> lock(mutex);
            const auto it = files.find(token);
            if (it == files.end() || token.empty()) {
                send_status(fd, 404, "Not Found");
                return;
            }
            file = it->second;
        }
        if (req.method != "GET" && req.method != "HEAD") {
            send_status(fd, 405, "Method Not Allowed");
            return;
        }
        forward(fd, req, file.url, file.headers, false, "");
        return;
    }
    std::string origin;
    std::string token;
    std::vector<std::string> headers;
    {
        std::lock_guard<std::mutex> lock(mutex);
        origin = site_origin;
        token = site_token;
        headers = site_headers;
    }
    if (token.empty()) {
        send_status(fd, 404, "Not Found");
        return;
    }
    if (target.rfind("/__pet/", 0) == 0) {
        const auto q = target.find('?');
        const std::string given = target.substr(7, q == std::string::npos ? std::string::npos : q - 7);
        std::string go = "/";
        if (q != std::string::npos && target.compare(q, 4, "?go=") == 0) go = url_decode(target.substr(q + 4));
        if (given != token) {
            send_status(fd, 403, "Forbidden");
            return;
        }
        if (go.empty() || go[0] != '/' || go.find("//") == 0 || go.find_first_of("\r\n") != std::string::npos) go = "/";
        send_status(fd, 302, "Found",
                    "Location: " + go + "\r\nSet-Cookie: " + kCookie + "=" + token +
                        "; Path=/; HttpOnly; SameSite=Lax\r\nCache-Control: no-store\r\n");
        return;
    }
    if (req.header("cookie").find(std::string(kCookie) + "=" + token) == std::string::npos) {
        send_status(fd, 403, "Forbidden");
        return;
    }
    forward(fd, req, origin + target, headers, true, origin);
}

void MediaRelay::State::forward(int fd, const Request& req, const std::string& url,
                                const std::vector<std::string>& extra, bool site, const std::string& origin) {
    // globoff: [ ] { } in the PC's URL (ROM file names such as "Game [!].zip")
    // are part of the name, not curl ranges ("bad range specification", a 502).
    std::string config = "globoff\n" + config_line("url", url);
    if (req.method != "GET" && req.method != "HEAD") config += config_line("request", req.method);
    for (const auto& h : extra) config += config_line("header", h);
    config += config_line("header", "Expect:");
    static const char* kAlways[] = {"range", "if-range", "accept", "user-agent", "if-none-match", "if-modified-since"};
    static const char* kNever[] = {"host",   "connection",       "keep-alive", "proxy-connection", "upgrade",
                                   "authorization", "origin",   "referer",    "content-length",   "transfer-encoding",
                                   "expect", "te",              "cookie"};
    for (const auto& h : req.headers) {
        const std::string name = lower(h.first);
        bool pass = std::find(std::begin(kAlways), std::end(kAlways), name) != std::end(kAlways);
        if (site && std::find(std::begin(kNever), std::end(kNever), name) == std::end(kNever)) pass = true;
        if (pass) config += config_line("header", h.first + ": " + h.second);
    }
    if (site) {
        // Pass the site's own cookies (CSRF) but never the relay's gate cookie.
        std::string kept;
        std::istringstream parts(req.header("cookie"));
        std::string part;
        while (std::getline(parts, part, ';')) {
            part = trim(part);
            if (part.empty() || part.rfind(std::string(kCookie) + "=", 0) == 0) continue;
            kept += (kept.empty() ? "" : "; ") + part;
        }
        if (!kept.empty()) config += config_line("header", "Cookie: " + kept);
    }

    int cfg[2], out[2], in[2] = {-1, -1};
    if (make_pipe(cfg) != 0) return send_status(fd, 500, "Internal Error");
    if (make_pipe(out) != 0) {
        ::close(cfg[0]);
        ::close(cfg[1]);
        return send_status(fd, 500, "Internal Error");
    }
    const bool has_body = !req.body.empty();
    if (has_body && make_pipe(in) != 0) {
        for (int f : {cfg[0], cfg[1], out[0], out[1]}) ::close(f);
        return send_status(fd, 500, "Internal Error");
    }
    std::vector<std::string> args = {"curl", "-sS", "--http1.1", "-i", "--raw", "--no-buffer",
                                     "--suppress-connect-headers", "--connect-timeout", "10", "-K", "/dev/fd/3"};
    if (req.method == "HEAD") args.push_back("--head");
    if (has_body) {
        args.push_back("--data-binary");
        args.push_back("@-");
    }
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    const pid_t pid = ::fork();
    if (pid == 0) {
        const int null_fd = ::open("/dev/null", O_RDWR);
        ::dup2(has_body ? in[0] : null_fd, 0);
        ::dup2(out[1], 1);
        ::dup2(null_fd, 2);
        ::dup2(cfg[0], 3);
        ::execvp("curl", argv.data());
        ::_exit(127);
    }
    ::close(cfg[0]);
    ::close(out[1]);
    if (has_body) ::close(in[0]);
    if (pid < 0) {
        ::close(cfg[1]);
        ::close(out[0]);
        if (has_body) ::close(in[1]);
        return send_status(fd, 500, "Internal Error");
    }
    (void)!::write(cfg[1], config.data(), config.size());
    ::close(cfg[1]);
    if (has_body) {
        size_t off = 0;
        while (off < req.body.size()) {
            const ssize_t n = ::write(in[1], req.body.data() + off, req.body.size() - off);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            off += static_cast<size_t>(n);
        }
        ::close(in[1]);
    }

    std::string head;
    bool head_sent = false;
    char buf[64 * 1024];
    bool client_gone = false;
    while (!stopping && !client_gone) {
        pollfd p[2] = {{out[0], POLLIN, 0}, {fd, POLLIN, 0}};
        const int r = ::poll(p, 2, 1000);
        if (r < 0 && errno != EINTR) break;
        if (r <= 0) continue;
        if (p[1].revents & (POLLIN | POLLHUP | POLLERR)) {
            // The PC never sends more after the request: data or EOF here means it left.
            char probe;
            const ssize_t n = ::recv(fd, &probe, 1, MSG_DONTWAIT);
            if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) client_gone = true;
        }
        if (!(p[0].revents & (POLLIN | POLLHUP))) continue;
        const ssize_t n = ::read(out[0], buf, sizeof(buf));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        if (head_sent) {
            if (!send_all(fd, buf, static_cast<size_t>(n))) client_gone = true;
            continue;
        }
        head.append(buf, static_cast<size_t>(n));
        const auto end = head.find("\r\n\r\n");
        if (end == std::string::npos) {
            if (head.size() > kMaxHead) break;
            continue;
        }
        const std::string rewritten = MediaRelay::rewrite_response_head(head.substr(0, end + 4), origin);
        head_sent = true;
        if (!send_all(fd, rewritten.data(), rewritten.size()) ||
            !send_all(fd, head.data() + end + 4, head.size() - end - 4)) {
            client_gone = true;
        }
        head.clear();
    }
    ::close(out[0]);
    ::kill(pid, SIGTERM);
    int status = 0;
    ::waitpid(pid, &status, 0);
    if (!head_sent && !client_gone) send_status(fd, 502, "Bad Gateway");
}

std::string MediaRelay::rewrite_response_head(const std::string& head, const std::string& origin) {
    std::istringstream lines(head);
    std::string line;
    std::string out;
    bool first = true;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (first) {
            out += line + "\r\n";
            first = false;
            continue;
        }
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        const std::string name = lower(trim(line.substr(0, colon)));
        std::string value = trim(line.substr(colon + 1));
        if (name == "connection" || name == "keep-alive" || name == "strict-transport-security" || name == "alt-svc" ||
            name == "content-security-policy") {
            continue;
        }
        if (name == "location" && !origin.empty() && value.rfind(origin, 0) == 0) {
            value = value.substr(origin.size());
            if (value.empty()) value = "/";
        }
        if (name == "set-cookie") {
            // The PC talks plain http to the PET: Secure cookies would be dropped.
            std::string kept;
            std::istringstream attrs(value);
            std::string attr;
            bool first_attr = true;
            while (std::getline(attrs, attr, ';')) {
                attr = trim(attr);
                const std::string l = lower(attr);
                if (!first_attr && (l == "secure" || l == "partitioned" || l.rfind("domain=", 0) == 0)) continue;
                if (!first_attr && l == "samesite=none") attr = "SameSite=Lax";
                kept += (first_attr ? "" : "; ") + attr;
                first_attr = false;
            }
            value = kept;
        }
        out += line.substr(0, colon) + ": " + value + "\r\n";
    }
    return out + "Connection: close\r\n\r\n";
}

} // namespace platform::jack
