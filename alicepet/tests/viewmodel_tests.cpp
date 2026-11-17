/*
 * SPDX-License-Identifier: MIT
 *
 * Unit tests for AlicePET's LVGL-free logic: the view model, text helpers, the
 * TERMINAL chip's Ask-Alice handling and the VT100 screen. No framework, so the
 * tests build anywhere the app builds: `ctest` or run alicepet_tests directly.
 */

#include "model/chip_registry.h"
#include "model/pet_config.h"
#include "platform/json.h"
#include "platform/process.h"
#include "viewmodel/alice_viewmodel.h"
#include "viewmodel/chips/bridge_client.h"
#include "viewmodel/chips/terminal_chip.h"
#include "viewmodel/chips/vt_screen.h"

#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        ++g_checks;                                                                    \
        if (!(cond)) {                                                                 \
            ++g_failures;                                                              \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                              \
    } while (0)

using viewmodel::AliceViewModel;
using viewmodel::Key;
using viewmodel::Screen;
using viewmodel::chips::TerminalChip;

void booted(AliceViewModel& vm) {
    vm.tick(AliceViewModel::kBootDurationMs);
}

// --- text helpers ---------------------------------------------------------

void test_wrap() {
    const auto lines = AliceViewModel::wrap("the quick brown fox jumps over the lazy dog", 10);
    CHECK(lines.size() == 5);
    CHECK(lines[0] == "the quick");
    for (const auto& line : lines) CHECK(line.size() <= 10);

    // Hard breaks are kept, blank runs collapse, trailing blanks are dropped.
    const auto para = AliceViewModel::wrap("a\n\n\nb\n\n", 24);
    CHECK((para == std::vector<std::string>{"a", "", "b"}));

    // A word longer than the line is split, not lost.
    const auto long_word = AliceViewModel::wrap("abcdefghijkl", 5);
    CHECK((long_word == std::vector<std::string>{"abcde", "fghij", "kl"}));
}

void test_screen_text() {
    CHECK(AliceViewModel::screen_text("it\xE2\x80\x99s") == "it's");          // right single quote
    CHECK(AliceViewModel::screen_text("wait\xE2\x80\xA6") == "wait...");      // ellipsis
    CHECK(AliceViewModel::screen_text("Hi \xF0\x9F\x90\x87 !") == "Hi!");      // emoji dropped, gap closed
    CHECK(AliceViewModel::screen_text("a\tb\r") == "a b");
}

void test_clip_for_bridge() {
    using viewmodel::chips::clip_for_bridge;
    CHECK(clip_for_bridge("hello", 3) == "he" + std::string("l"));
    CHECK(clip_for_bridge(std::string(700, 'x')).size() == viewmodel::chips::kBridgeMaxInput);
    // Never cuts inside a code point; an emoji counts as two JavaScript characters.
    const std::string emoji = "ab\xF0\x9F\x90\x87";
    CHECK(clip_for_bridge(emoji, 3) == "ab");
    CHECK(clip_for_bridge(emoji, 4) == emoji);
    CHECK(clip_for_bridge("\xC3\xA9\xC3\xA9", 1) == "\xC3\xA9");
}

// --- view model -------------------------------------------------------------

void test_boot_and_hotkeys() {
    AliceViewModel vm;
    CHECK(vm.screen() == Screen::Boot);
    vm.tick(AliceViewModel::kBootDurationMs / 2);
    CHECK(vm.screen() == Screen::Boot);
    CHECK(vm.boot_progress() == 50);
    vm.tick(AliceViewModel::kBootDurationMs);
    CHECK(vm.screen() == Screen::Home);

    vm.handle_char("2");
    CHECK(vm.screen() == Screen::Folder);
    vm.handle_key(Key::Escape);
    CHECK(vm.screen() == Screen::Home);
}

void test_folder_wraps() {
    AliceViewModel vm;
    booted(vm);
    vm.handle_key(Key::Digit2);
    const size_t count = model::chip_count();
    CHECK(vm.selected_chip() == 0);
    vm.handle_key(Key::Left);
    CHECK(vm.selected_chip() == count - 1);
    vm.handle_key(Key::Right);
    CHECK(vm.selected_chip() == 0);
    vm.handle_char("c");  // letter navigation alias for Right
    CHECK(vm.selected_chip() == 1);
    vm.handle_key(Key::Enter);
    CHECK(vm.screen() == Screen::Slotted);
}

void test_chat_prompt() {
    AliceViewModel vm;
    booted(vm);
    vm.set_chat_available(true);
    vm.handle_char(" ");  // no leading space
    CHECK(vm.input().empty());
    for (const char* c : {"h", "i", "2"}) vm.handle_char(c);  // digits are text once typing
    CHECK(vm.input() == "hi2");
    CHECK(vm.screen() == Screen::Home);
    vm.handle_key(Key::Backspace);
    CHECK(vm.input() == "hi");

    vm.handle_key(Key::Enter);
    std::string out;
    CHECK(vm.take_outgoing(out));
    CHECK(out == "hi");
    CHECK(!vm.take_outgoing(out));
    CHECK(vm.chat_state() == viewmodel::ChatState::Thinking);

    vm.on_chat_reply(true, std::string(200, 'w'));
    CHECK(vm.chat_state() == viewmodel::ChatState::Reply);
    CHECK(vm.bubble_pages() > 1);
    vm.handle_key(Key::Down);
    CHECK(vm.bubble_page() == 1);
}

void test_sleep_and_wake() {
    AliceViewModel vm;
    booted(vm);
    vm.set_sleep_after_ms(1000);
    vm.tick(999);
    CHECK(!vm.sleeping());
    vm.tick(2);
    CHECK(vm.sleeping());
    vm.handle_char("q");  // waking key is swallowed
    CHECK(!vm.sleeping());
    CHECK(vm.input().empty());

    vm.handle_key(Key::Escape);  // ESC on an empty prompt naps
    CHECK(vm.sleeping());
}

void test_dim_then_sleep() {
    AliceViewModel vm;
    booted(vm);
    vm.set_dim_after_ms(500);
    vm.set_sleep_after_ms(1000);
    vm.tick(499);
    CHECK(!vm.dimmed());
    vm.tick(2);
    CHECK(vm.dimmed());
    CHECK(!vm.sleeping());
    vm.handle_char("q");  // a key while dimmed brightens and is NOT swallowed
    CHECK(!vm.dimmed());
    CHECK(vm.input() == "q");
    vm.tick(1001);
    CHECK(vm.sleeping());
    CHECK(!vm.dimmed());  // asleep means off, not dim
}

// --- TERMINAL chip ------------------------------------------------------------

model::PetConfig terminal_config() {
    model::PetConfig config;
    config.ssh_config_import.clear();
    config.bookmarks.push_back({"pi", "holden@127.0.0.1", "test box", 0});
    return config;
}

// Puts the chip in Ask -> Waiting, as if the question had been sent.
void ask(TerminalChip& term, const std::string& question) {
    term.handle_char("a");
    for (char c : question) term.handle_char(std::string(1, c));
    term.handle_key(Key::Enter);
}

void test_terminal_reply_parsing() {
    TerminalChip term(terminal_config());
    ask(term, "disk");
    CHECK(term.ask_state() == TerminalChip::AskState::Waiting);
    viewmodel::chips::BridgeReply reply;
    reply.ok = true;
    reply.reply = "CMD: `df -h /`\nWHY: Shows how full the root disk is.";
    term.on_ask_reply(reply);
    CHECK(term.ask_state() == TerminalChip::AskState::Suggestion);
    CHECK(term.ask_command() == "df -h /");
    CHECK(term.ask_why() == "Shows how full the root disk is.");
    CHECK(term.ask_command_fits());

    // Fallback: first inline code span.
    TerminalChip term2(terminal_config());
    ask(term2, "uptime");
    reply.reply = "Try `uptime` to see the load.";
    term2.on_ask_reply(reply);
    CHECK(term2.ask_command() == "uptime");
}

void test_terminal_long_command_cannot_run() {
    TerminalChip term(terminal_config());
    ask(term, "clean");
    viewmodel::chips::BridgeReply reply;
    reply.ok = true;
    const std::string command = "echo " + std::string(150, 'x') + " && rm -rf /tmp/nothing";
    reply.reply = "CMD: " + command + "\nWHY: long";
    term.on_ask_reply(reply);
    CHECK(term.ask_command() == command);
    CHECK(!term.ask_command_fits());
    CHECK(term.ask_command_lines().size() > TerminalChip::kCommandLines);
    // ENTER is refused: the suggestion stays up and nothing connects.
    term.handle_key(Key::Enter);
    CHECK(term.mode() == TerminalChip::Mode::Ask);
    CHECK(term.ask_state() == TerminalChip::AskState::Suggestion);
    CHECK(term.session() == nullptr);
}

void test_terminal_command_lines() {
    TerminalChip term(terminal_config());
    ask(term, "x");
    viewmodel::chips::BridgeReply reply;
    reply.ok = true;
    const std::string command(TerminalChip::kCommandCols * TerminalChip::kCommandLines, 'y');
    reply.reply = "CMD: " + command;
    term.on_ask_reply(reply);
    const auto lines = term.ask_command_lines();
    CHECK(lines.size() == TerminalChip::kCommandLines);
    std::string joined;
    for (const auto& line : lines) {
        CHECK(line.size() <= TerminalChip::kCommandCols);
        joined += line;
    }
    CHECK(joined == command);  // every character is shown
    CHECK(term.ask_command_fits());
}

void test_terminal_soft_wrap() {
    TerminalChip term(terminal_config());
    ask(term, "x");
    viewmodel::chips::BridgeReply reply;
    reply.ok = true;
    const std::string command = "sudo journalctl -u jellyfin --since today --no-pager | tail -n 40";
    reply.reply = "CMD: " + command;
    term.on_ask_reply(reply);
    const auto lines = term.ask_command_lines();
    CHECK(lines.size() == 2);
    std::string joined;
    for (const auto& line : lines) {
        CHECK(line.size() <= TerminalChip::kCommandCols);
        joined += line;
    }
    CHECK(joined == command);  // wrapping only moves where lines break
    CHECK(lines[0].back() == ' ');  // broken after a space, not mid-word
}

void test_terminal_keep_awake() {
    TerminalChip term(terminal_config());
    CHECK(!term.keep_awake());  // host list can nap
    ask(term, "disk");
    CHECK(term.keep_awake());  // waiting on Alice
    viewmodel::chips::BridgeReply reply;
    reply.ok = true;
    reply.reply = "CMD: df -h";
    term.on_ask_reply(reply);
    CHECK(!term.keep_awake());  // a suggestion on screen doesn't hold the pet awake
}

void test_sanitize_and_prompt() {
    using viewmodel::chips::build_ask_prompt;
    using viewmodel::chips::sanitize_command;
    CHECK(sanitize_command("ls\x1b[2J -la\r\n") == "ls[2J -la");
    CHECK(sanitize_command("  uptime  ") == "uptime");

    const std::string prompt = build_ask_prompt("'pi' (holden@127.0.0.1)", std::string(1000, 'q'));
    CHECK(prompt.size() <= viewmodel::chips::kBridgeMaxInput);
    // The reply format comes before the question, so truncation never drops it.
    CHECK(prompt.find("CMD:") != std::string::npos);
    CHECK(prompt.find("CMD:") < prompt.find("Question:"));
}

// --- VT100 screen -------------------------------------------------------------

void test_vt_screen() {
    viewmodel::chips::VtScreen screen(10, 3);
    screen.feed("hello\r\nworld");
    CHECK(screen.row_text(0) == "hello");
    CHECK(screen.row_text(1) == "world");

    // Huge counts are clamped instead of looping for minutes.
    const auto start = std::chrono::steady_clock::now();
    screen.feed("\x1b[2000000000S\x1b[2000000000L\x1b[2000000000P\x1b[2000000000@");
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    CHECK(ms < 1000);
    CHECK(screen.row_text(0).empty());
}

// --- platform helpers -------------------------------------------------------

void test_split_command() {
    using platform::proc::split_command;
    CHECK((split_command("mpv --fs 'a b' \"c d\"") == std::vector<std::string>{"mpv", "--fs", "a b", "c d"}));
    CHECK(split_command("   ").empty());
}

void test_json() {
    platform::json::Value v;
    CHECK(platform::json::parse(R"({"ok":true,"reply":"hi é","n":[1,2]})", v));
    CHECK(v["ok"].boolean());
    CHECK(v["reply"].str() == "hi \xC3\xA9");
    CHECK(v["n"].size() == 2);
    CHECK(v["missing"]["deeper"].is_null());
    CHECK(!platform::json::parse("{\"unterminated", v));
}

} // namespace

int main() {
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"wrap", test_wrap},
        {"screen_text", test_screen_text},
        {"clip_for_bridge", test_clip_for_bridge},
        {"boot_and_hotkeys", test_boot_and_hotkeys},
        {"folder_wraps", test_folder_wraps},
        {"chat_prompt", test_chat_prompt},
        {"sleep_and_wake", test_sleep_and_wake},
        {"dim_then_sleep", test_dim_then_sleep},
        {"terminal_reply_parsing", test_terminal_reply_parsing},
        {"terminal_long_command_cannot_run", test_terminal_long_command_cannot_run},
        {"terminal_command_lines", test_terminal_command_lines},
        {"terminal_soft_wrap", test_terminal_soft_wrap},
        {"terminal_keep_awake", test_terminal_keep_awake},
        {"sanitize_and_prompt", test_sanitize_and_prompt},
        {"vt_screen", test_vt_screen},
        {"split_command", test_split_command},
        {"json", test_json},
    };
    for (const auto& [name, fn] : tests) {
        const int before = g_failures;
        fn();
        std::printf("%s %s\n", g_failures == before ? "PASS" : "FAIL", name);
    }
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
