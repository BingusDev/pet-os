/*
 * SPDX-License-Identifier: MIT
 */

#include "mpv_ipc.h"

#include "json.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace platform::mpv {

bool command(const std::string& socket_path, const std::string& json, std::string* reply, int timeout_ms) {
    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close(fd);
        return false;
    }
    const std::string line = json + "\n";
    if (send(fd, line.data(), line.size(), 0) != static_cast<ssize_t>(line.size())) {
        close(fd);
        return false;
    }
    std::string buffer;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    bool ok = false;
    while (true) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0) break;
        pollfd pfd{fd, POLLIN, 0};
        if (poll(&pfd, 1, static_cast<int>(left)) <= 0) break;
        char chunk[1024];
        const ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
        if (n <= 0) break;
        buffer.append(chunk, static_cast<size_t>(n));
        // mpv interleaves event lines; the command's reply is the line with "error".
        size_t start = 0;
        size_t nl;
        while ((nl = buffer.find('\n', start)) != std::string::npos) {
            const std::string candidate = buffer.substr(start, nl - start);
            start = nl + 1;
            if (candidate.find("\"error\"") != std::string::npos) {
                if (reply) *reply = candidate;
                ok = candidate.find("\"success\"") != std::string::npos;
                close(fd);
                return ok;
            }
        }
        buffer.erase(0, start);
    }
    close(fd);
    return ok;
}

bool time_pos(const std::string& socket_path, double& seconds) {
    std::string reply;
    if (!command(socket_path, R"({"command":["get_property","time-pos"]})", &reply)) return false;
    json::Value value;
    if (!json::parse(reply, value) || value["data"].type() != json::Value::Type::Number) return false;
    seconds = value["data"].num();
    return true;
}

} // namespace platform::mpv
