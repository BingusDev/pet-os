/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "model/pet_config.h"
#include "platform/net.h"

#include <memory>
#include <string>
#include <vector>

namespace app {

// Background network work for the UI thread. Each request runs on a detached
// worker that owns a shared inbox, so quitting the app never waits on a slow
// socket. The UI thread drains results with poll() once per frame.

struct ChatResult {
    bool ok{false};
    std::string text;  // Alice's reply, or a short error for the bubble
};

class ChatClient {
public:
    explicit ChatClient(model::PetConfig config);

    bool configured() const { return !config_.bridge_url.empty(); }
    // Starts one ask; ignored while a previous one is in flight.
    void send(const std::string& text);
    bool busy() const;
    bool poll(ChatResult& out);

private:
    struct Inbox;
    model::PetConfig config_;
    std::shared_ptr<Inbox> inbox_;
};

struct HostResult {
    size_t index{0};
    platform::net::ProbeResult probe;
};

class NetScanner {
public:
    explicit NetScanner(model::PetConfig config);

    const std::vector<model::HostConfig>& hosts() const { return config_.hosts; }
    // Probes every host in parallel; ignored while a scan is running.
    void start();
    bool running() const;
    bool poll(HostResult& out);

private:
    struct Inbox;
    model::PetConfig config_;
    std::shared_ptr<Inbox> inbox_;
};

} // namespace app
