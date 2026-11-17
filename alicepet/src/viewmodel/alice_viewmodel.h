/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace viewmodel {

enum class Screen { Boot, Home, Folder, Slotted, NetScan };

// Keys the view model understands, decoupled from LVGL / SDL key codes.
// Printable characters go through handle_char() instead.
enum class Key { Left, Right, Up, Down, Enter, Escape, Backspace, Digit1, Digit2, Digit3, PushToTalk };

// Home speech bubble state.
enum class ChatState { Greeting, Thinking, Reply, Error };

enum class HostState { Unknown, Checking, Online, Rest, Offline };

struct HostRow {
    std::string name;
    std::string address;
    std::string probe;   // "tcp:22", "ps5 discovery"
    HostState state{HostState::Unknown};
    int latency_ms{-1};
    std::string detail;
};

// Pure state machine for AlicePET: boot timing, screen switching, folder
// carousel, Home chat prompt/bubble, NET SCAN list. Network work is requested
// through take_*() and answered through on_*(); the view observes accessors
// after each call.
class AliceViewModel {
public:
    static constexpr uint32_t kBootDurationMs = 2000;
    // Bubble text grid: Departure Mono 11 px is 7 px wide, the bubble text area is 170 px.
    static constexpr size_t kBubbleCols = 24;
    static constexpr size_t kBubbleRows = 4;
    static constexpr size_t kMaxInput = 240;

    // Advances timers. Call with the elapsed milliseconds since the last tick.
    void tick(uint32_t elapsed_ms);
    void handle_key(Key key);
    // A printable character (UTF-8 encoded, one code point).
    void handle_char(const std::string& utf8);

    Screen screen() const { return screen_; }
    bool sleeping() const { return sleeping_; }
    // 0..100 boot progress.
    int boot_progress() const { return boot_progress_; }
    size_t selected_chip() const { return selected_chip_; }
    // Changes whenever the visible state changes, so the view can skip redraws.
    uint32_t revision() const { return revision_; }

    // --- Talk to Alice (Home) ---
    const std::string& input() const { return input_; }
    ChatState chat_state() const { return chat_state_; }
    // Lines of the current bubble page (already wrapped to kBubbleCols).
    std::vector<std::string> bubble_page_lines() const;
    size_t bubble_page() const { return bubble_page_; }
    size_t bubble_pages() const;
    // "thinking", "thinking.", ... animated while waiting.
    int thinking_dots() const { return thinking_dots_; }
    // Outgoing message for the chat client; true once per ENTER.
    bool take_outgoing(std::string& out);
    void on_chat_reply(bool ok, const std::string& text);
    void set_chat_available(bool available) { chat_available_ = available; }

    // --- NET SCAN ---
    void set_hosts(std::vector<HostRow> hosts);
    const std::vector<HostRow>& hosts() const { return hosts_; }
    size_t selected_host() const { return selected_host_; }
    bool host_details_open() const { return host_details_open_; }
    bool scanning() const { return scanning_; }
    size_t online_count() const;
    bool take_scan_request();
    void on_host_result(size_t index, HostState state, int latency_ms, const std::string& detail);
    void on_scan_finished();

    // Wraps text into lines of at most `cols` code points (word wrap, hard breaks kept).
    static std::vector<std::string> wrap(const std::string& text, size_t cols);

private:
    void show(Screen next);
    void set_bubble(ChatState state, const std::string& text);
    void request_scan();

    Screen screen_{Screen::Boot};
    uint32_t boot_elapsed_ms_{0};
    int boot_progress_{0};
    size_t selected_chip_{0};
    bool sleeping_{false};
    uint32_t revision_{0};

    std::string input_;
    ChatState chat_state_{ChatState::Greeting};
    std::vector<std::string> bubble_lines_;
    size_t bubble_page_{0};
    std::string outgoing_;
    bool has_outgoing_{false};
    bool chat_available_{true};
    uint32_t thinking_elapsed_ms_{0};
    int thinking_dots_{0};

    std::vector<HostRow> hosts_;
    size_t selected_host_{0};
    bool host_details_open_{false};
    bool scanning_{false};
    bool scan_requested_{false};
    bool scanned_once_{false};
};

} // namespace viewmodel
