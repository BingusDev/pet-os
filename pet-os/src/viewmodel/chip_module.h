/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "viewmodel/keys.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace viewmodel {

// One battle chip's screen logic (BUILD-PLAN: "every chip = one module").
// LVGL-free: the matching view in view/chips/ renders it from the accessors of
// the concrete class. Network / process work runs on detached workers and is
// drained in tick() on the UI thread, so a chip never blocks the frame loop.
class ChipModule {
public:
    virtual ~ChipModule() = default;

    // Chip id from model/chip_registry ("voice", "terminal", ...).
    virtual const char* id() const = 0;
    // Called when the chip is slotted in / left.
    virtual void enter() {}
    virtual void leave() {}
    // Returns false when the chip wants to go back to the folder (usually ESC at its top level).
    virtual bool handle_key(Key key) = 0;
    virtual void handle_char(const std::string& utf8) { (void)utf8; }
    // Ctrl+letter ('a'..'z').
    virtual void handle_ctrl(char letter) { (void)letter; }
    virtual void tick(uint32_t elapsed_ms) { (void)elapsed_ms; }
    // True while auto-sleep must not trigger (recording, video, live shell output).
    virtual bool keep_awake() const { return false; }
    // True if the chip uses printable keys itself, so the letter-navigation aliases (F/X/Z/C) are off.
    virtual bool wants_text() const { return false; }
    // True while an external program (the video player) draws on the panel; the
    // app then stops redrawing LVGL and repaints everything when it returns.
    virtual bool owns_display() const { return false; }
    // True once when the chip asks to go straight to Home (instead of the folder).
    virtual bool take_go_home() { return false; }

    uint32_t revision() const { return revision_; }

protected:
    void touch() { ++revision_; }

private:
    uint32_t revision_{0};
};

// Results posted by detached workers, drained on the UI thread. Workers hold a
// shared_ptr, so destroying the owner never waits on a slow socket.
template <typename T>
class Mailbox {
public:
    Mailbox() : inbox_(std::make_shared<Inbox>()) {}

    // Runs `job` on a detached thread; its return value is posted to the mailbox.
    void run(std::function<T()> job) {
        ++inbox_->pending;
        std::thread([inbox = inbox_, job = std::move(job)]() {
            T value = job();
            std::lock_guard<std::mutex> lock(inbox->mutex);
            inbox->items.push_back(std::move(value));
            --inbox->pending;
        }).detach();
    }

    bool poll(T& out) {
        std::lock_guard<std::mutex> lock(inbox_->mutex);
        if (inbox_->items.empty()) return false;
        out = std::move(inbox_->items.front());
        inbox_->items.pop_front();
        return true;
    }

    bool busy() const { return inbox_->pending.load() > 0; }

private:
    struct Inbox {
        std::mutex mutex;
        std::deque<T> items;
        std::atomic<int> pending{0};
    };
    std::shared_ptr<Inbox> inbox_;
};

} // namespace viewmodel
