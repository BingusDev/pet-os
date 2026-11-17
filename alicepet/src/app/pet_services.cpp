/*
 * SPDX-License-Identifier: MIT
 */

#include "pet_services.h"

#include "viewmodel/chips/bridge_client.h"

#include <atomic>
#include <deque>
#include <mutex>
#include <thread>

namespace app {

struct ChatClient::Inbox {
    std::mutex mutex;
    std::deque<ChatResult> results;
    std::atomic<bool> busy{false};
};

ChatClient::ChatClient(model::PetConfig config)
    : config_(std::move(config)), inbox_(std::make_shared<Inbox>()) {}

bool ChatClient::busy() const {
    return inbox_->busy.load();
}

void ChatClient::send(const std::string& text) {
    if (inbox_->busy.exchange(true)) {
        return;
    }
    std::thread([inbox = inbox_, config = config_, text]() {
        // Same client, JSON parser and error texts as the chips (VOICE, TERMINAL).
        const auto answer = viewmodel::chips::bridge_ask(config, text);
        ChatResult result;
        result.ok = answer.ok;
        result.text = answer.ok ? answer.reply : answer.error;
        std::lock_guard<std::mutex> lock(inbox->mutex);
        inbox->results.push_back(std::move(result));
        inbox->busy = false;
    }).detach();
}

bool ChatClient::poll(ChatResult& out) {
    std::lock_guard<std::mutex> lock(inbox_->mutex);
    if (inbox_->results.empty()) {
        return false;
    }
    out = std::move(inbox_->results.front());
    inbox_->results.pop_front();
    return true;
}

struct NetScanner::Inbox {
    std::mutex mutex;
    std::deque<HostResult> results;
    std::atomic<int> pending{0};
};

NetScanner::NetScanner(model::PetConfig config)
    : config_(std::move(config)), inbox_(std::make_shared<Inbox>()) {}

bool NetScanner::running() const {
    return inbox_->pending.load() > 0;
}

void NetScanner::start() {
    if (running() || config_.hosts.empty()) {
        return;
    }
    inbox_->pending = static_cast<int>(config_.hosts.size());
    const int timeout = config_.netscan_timeout_ms;
    for (size_t i = 0; i < config_.hosts.size(); ++i) {
        std::thread([inbox = inbox_, host = config_.hosts[i], i, timeout]() {
            HostResult result;
            result.index = i;
            result.probe = host.probe == model::ProbeKind::Ps5
                               ? platform::net::probe_ps5(host.address, timeout)
                               : platform::net::probe_tcp(host.address, host.port, timeout);
            std::lock_guard<std::mutex> lock(inbox->mutex);
            inbox->results.push_back(std::move(result));
            --inbox->pending;
        }).detach();
    }
}

bool NetScanner::poll(HostResult& out) {
    std::lock_guard<std::mutex> lock(inbox_->mutex);
    if (inbox_->results.empty()) {
        return false;
    }
    out = std::move(inbox_->results.front());
    inbox_->results.pop_front();
    return true;
}

} // namespace app
