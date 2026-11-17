/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

// JACK IN transport on the PET side. The PC app always dials out to the PET
// over the USB network link, so nothing on the PC listens (no firewall prompt,
// nothing to clean up). See alicepet/jackin/PROTOCOL.md.
namespace platform::jack {

// The control line: JSON objects, one per line. One PC at a time; a newer
// connection replaces the old one (the app relaunching itself elevated).
class LineServer {
public:
    LineServer() = default;
    ~LineServer();
    LineServer(const LineServer&) = delete;
    LineServer& operator=(const LineServer&) = delete;

    // Binds address:port. Returns "" or a short error.
    std::string listen(const std::string& address, int port);
    void close();
    bool listening() const { return listen_fd_ >= 0; }
    bool connected() const { return client_fd_ >= 0; }

    struct Poll {
        bool connected{false};     // a PC connected (or replaced the old one)
        bool disconnected{false};  // the PC went away
        std::vector<std::string> lines;
    };
    // Never blocks.
    Poll poll();
    bool send(const std::string& line);  // adds the newline
    void drop_client();

    // A line longer than this drops the connection (a 15 s voice clip is ~650 KB of base64).
    static constexpr size_t kMaxLine = 2 * 1024 * 1024;

private:
    int listen_fd_{-1};
    int client_fd_{-1};
    std::string buffer_;
};

// "Share PC Internet": an HTTP proxy on the PET (CONNECT and plain http://)
// whose upstream connections are made by the PC. For each proxy request the
// relay asks the PC (through `open`) to dial host:port and then connect back
// to the tunnel port with "tunnel <id>\n"; bytes are piped both ways.
class Relay {
public:
    // Called from relay threads: send {"t":"net_open","id":..,"host":..,"port":..} to the PC.
    using OpenFn = std::function<void(int id, const std::string& host, int port)>;

    Relay();
    ~Relay();
    Relay(const Relay&) = delete;
    Relay& operator=(const Relay&) = delete;

    // proxy_address:proxy_port is what PET programs use (127.0.0.1:3128);
    // tunnel_address:tunnel_port is where the PC dials back. Returns "" or an error.
    std::string start(const std::string& proxy_address, int proxy_port, const std::string& tunnel_address,
                      int tunnel_port, OpenFn open);
    void stop();
    // The PC couldn't reach the host for request `id` (answers 502).
    void fail(int id);
    bool running() const;
    int active() const;  // open tunnels

    // Parses a proxy request head. CONNECT host:port, or an absolute http:// URL
    // (port 80 by default). Public for tests.
    static bool parse_request(const std::string& head, std::string& host, int& port, bool& is_connect);

private:
    struct State;
    std::shared_ptr<State> state_;
};

// Decodes standard base64 (padding optional, whitespace skipped); false on bad input.
bool base64_decode(const std::string& text, std::string& out);

// Sets / clears http(s)_proxy for this process, so curl in every chip uses the relay.
void set_proxy_env(const std::string& proxy_url);
// True while set_proxy_env has a proxy set (the status bar shows "PC NET").
bool proxy_active();

} // namespace platform::jack
