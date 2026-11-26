/*
 * SPDX-License-Identifier: MIT
 *
 * Unit tests for PET-os's LVGL-free logic: the view model, text helpers, the
 * TERMINAL chip's Ask-Alice handling and the VT100 screen. No framework, so the
 * tests build anywhere the app builds: `ctest` or run pet-os_tests directly.
 */

#include "platform/clock_sync.h"
#include "viewmodel/chips/jack_store.h"
#include "model/chip_registry.h"
#include "model/navi.h"
#include "model/pet_config.h"
#include "model/power_profile.h"
#include "platform/json.h"
#include "platform/power.h"
#include "platform/process.h"
#include "viewmodel/alice_viewmodel.h"
#include "viewmodel/chips/bridge_client.h"
#include "viewmodel/chips/ir_codec.h"
#include "viewmodel/chips/jellyfin_chip.h"
#include "viewmodel/chips/lan_tech_chip.h"
#include "viewmodel/chips/jack_in_chip.h"
#include "platform/jack_link.h"
#include "platform/net.h"
#include "platform/device_status.h"
#include "viewmodel/chips/remote_chip.h"
#include "viewmodel/chips/retro_chip.h"
#include "viewmodel/chips/terminal_chip.h"
#include "viewmodel/chips/vt_screen.h"
#include "viewmodel/pc_input.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>

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

    vm.handle_char("1");
    CHECK(vm.screen() == Screen::Folder);
    vm.handle_key(Key::Escape);
    CHECK(vm.screen() == Screen::Home);
    vm.handle_char("2");
    CHECK(vm.screen() == Screen::NetScan);
    vm.handle_key(Key::Escape);
    CHECK(vm.screen() == Screen::Home);
}

void test_folder_wraps() {
    AliceViewModel vm;
    booted(vm);
    vm.handle_key(Key::Digit1);
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

// --- power profiles ---------------------------------------------------------

void test_power_profiles() {
    using model::chip_at;
    using model::chip_index;
    const auto jf = model::chip_profile(chip_at(chip_index("jellyfin")));
    CHECK(jf.wifi && jf.audio && jf.cores == 4 && !jf.slow_cpu);
    const auto retro = model::chip_profile(chip_at(chip_index("retro")));
    CHECK(retro.hdmi && retro.cores == 2);
    const auto lan = model::chip_profile(chip_at(chip_index("lan-tech")));
    CHECK(lan.wifi && !lan.audio && lan.cores == 2);
    const auto nap = model::nap_profile();
    CHECK(!nap.wifi && nap.slow_cpu && nap.cores == 1);
    // Every chip that talks to Alice or a server needs the network.
    for (const char* id : {"voice", "terminal", "remote", "jellyfin", "retro", "lan-tech"}) {
        CHECK(model::chip_profile(chip_at(chip_index(id))).wifi);
    }
}

std::string slurp(const std::filesystem::path& p) {
    std::ifstream in(p);
    std::string v;
    std::getline(in, v);
    return v;
}

void put(const std::filesystem::path& p, const std::string& v) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream(p) << v << "\n";
}

void test_power_manager_sysfs() {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / ("pet-os-power-" + std::to_string(::getpid()));
    fs::remove_all(root);
    const fs::path cpu = root / "sys/devices/system/cpu";
    for (int i = 1; i < 4; ++i) put(cpu / ("cpu" + std::to_string(i)) / "online", "1");
    put(cpu / "cpufreq/policy0/cpuinfo_min_freq", "600000");
    put(cpu / "cpufreq/policy0/cpuinfo_max_freq", "1000000");
    put(cpu / "cpufreq/policy0/scaling_max_freq", "1000000");
    put(root / "sys/class/rfkill/rfkill0/type", "bluetooth");
    put(root / "sys/class/rfkill/rfkill0/soft", "0");
    put(root / "sys/class/rfkill/rfkill1/type", "wlan");
    put(root / "sys/class/rfkill/rfkill1/soft", "0");
    put(root / "sys/class/leds/ACT/brightness", "255");

    platform::PowerManager pm(root.string());
    pm.init();
    CHECK(slurp(root / "sys/class/rfkill/rfkill0/soft") == "1");  // Bluetooth off
    CHECK(slurp(root / "sys/class/rfkill/rfkill1/soft") == "0");  // Wi-Fi untouched
    CHECK(slurp(root / "sys/class/leds/ACT/brightness") == "0");

    pm.apply(model::nap_profile());
    CHECK(slurp(cpu / "cpu1/online") == "0");
    CHECK(slurp(cpu / "cpu3/online") == "0");
    CHECK(slurp(cpu / "cpufreq/policy0/scaling_max_freq") == "600000");

    pm.apply(model::chip_profile(model::chip_at(model::chip_index("jellyfin"))));
    CHECK(slurp(cpu / "cpu1/online") == "1");
    CHECK(slurp(cpu / "cpu3/online") == "1");
    CHECK(slurp(cpu / "cpufreq/policy0/scaling_max_freq") == "1000000");

    pm.apply(model::launcher_profile());
    CHECK(slurp(cpu / "cpu1/online") == "0");
    pm.restore();
    CHECK(slurp(cpu / "cpu1/online") == "1");
    CHECK(slurp(cpu / "cpu2/online") == "1");
    fs::remove_all(root);
}

void test_config_local_overlay() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / ("pet-os-conf-" + std::to_string(::getpid()));
    fs::create_directories(dir);
    put(dir / "pet-os.conf", "[bridge]\nurl=\n[terminal]\nssh_config_import=\n[power]\nsleep_after_s=300\n");
    model::PetConfig config;
    CHECK(model::load_pet_config((dir / "pet-os.conf").string(), config));
    CHECK(config.bridge_url.empty());
    CHECK(config.bookmarks.empty());
    put(dir / "pet-os.local.conf", "[bridge]\nurl=http://mac:8797\n[terminal]\nhost=pi|me@10.0.0.2|home\n");
    CHECK(model::load_pet_config((dir / "pet-os.conf").string(), config));
    CHECK(config.bridge_url == "http://mac:8797");
    CHECK(config.bookmarks.size() == 1);
    CHECK(config.sleep_after_s == 300);  // untouched values still come from the main file
    CHECK(model::local_config_path("/etc/pet-os.conf") == "/etc/pet-os.local.conf");
    // rev28 keys and their defaults.
    CHECK(config.jack_reports && config.jack_state_dir == "~/.local/share/pet-os" && config.jellyfin_pc_user.empty());
    CHECK(config.clock_set.find("set-clock.sh") != std::string::npos);
    const auto fresh = model::parse_pet_config(
        "[jackin]\nreports=off\nstate_dir=/var/lib/pet\n[jellyfin]\npc_user=couch\npc_password=p w\n[clock]\nset=\n");
    CHECK(!fresh.jack_reports && fresh.jack_state_dir == "/var/lib/pet");
    CHECK(fresh.jellyfin_pc_user == "couch" && fresh.jellyfin_pc_password == "p w" && fresh.clock_set.empty());
    fs::remove_all(dir);
}

// --- REMOTE -----------------------------------------------------------------

viewmodel::chips::ir::Raw nec_raw(uint32_t code) {
    viewmodel::chips::ir::Raw raw{9000, 4500};
    for (int i = 31; i >= 0; --i) {
        raw.push_back(560);
        raw.push_back((code >> i) & 1 ? 1690 : 560);
    }
    raw.push_back(560);
    return raw;
}

void test_ir_codec() {
    namespace ir = viewmodel::chips::ir;
    const auto raw = nec_raw(0x20DF10EF);
    uint32_t code = 0;
    CHECK(ir::decode_nec(raw, code));
    CHECK(code == 0x20DF10EFu);
    CHECK(ir::describe(raw) == "NEC 0x20DF10EF");
    // Round trip through the storage text and ir-ctl's two output styles.
    CHECK(ir::parse_ir_ctl(ir::to_text(raw)) == raw);
    CHECK(ir::parse_ir_ctl(ir::to_send_file(raw)) == raw);
    CHECK(ir::parse_ir_ctl("-1000 +500 -400 +300 -200\n# timeout 125000") == (ir::Raw{500, 400, 300}));
    CHECK(ir::parse_ir_ctl("+100 +50 -20 +30") == (ir::Raw{150, 20, 30}));  // repeated pulses merge
    CHECK(!ir::decode_nec({500, 400, 300}, code));
    CHECK(ir::describe({500, 400, 300}) == "raw · 3 edges");
    CHECK(ir::to_send_file({500, 400, 300}) == "pulse 500\nspace 400\npulse 300\n");
}

void test_remote_chip_files_and_pad() {
    namespace fs = std::filesystem;
    using viewmodel::chips::RemoteChip;
    const fs::path dir = fs::temp_directory_path() / ("pet-os-remote-" + std::to_string(::getpid()));
    fs::remove_all(dir);
    put(dir / "tv.remote", "name=Store TV\nPOWER=" + viewmodel::chips::ir::to_text(nec_raw(0x20DF10EF)) + "\n");
    put(dir / "night.macro", "name=Night\nstep=Store TV|POWER\nstep=wait|2\n");
    model::PetConfig config;
    config.remote_dir = dir.string();
    RemoteChip remote(config);
    remote.enter();
    CHECK(remote.devices().size() == 1);
    CHECK(remote.devices()[0].name == "Store TV");
    CHECK(remote.devices()[0].learned() == 1);
    CHECK(remote.macros().size() == 1 && remote.macros()[0].steps.size() == 2);
    CHECK(remote.macros()[0].steps[1].wait_s == 2);

    remote.handle_key(Key::Enter);  // open the pad
    CHECK(remote.view() == RemoteChip::View::Pad);
    remote.handle_key(Key::Down);   // POWER -> VOL-
    CHECK(remote.button() == 4);
    remote.handle_key(Key::Down);   // -> UP (row of 6)
    CHECK(remote.button() == 9);
    for (int i = 0; i < 9; ++i) remote.handle_key(Key::Right);
    CHECK(remote.button() == 14);   // BACK, clamped at the row end
    remote.handle_key(Key::Up);     // column 5 -> CH+ (last of the row of 5)
    CHECK(remote.button() == 8);
    remote.handle_key(Key::Enter);  // CH+ isn't learned: nothing is sent
    CHECK(!remote.sending());
    CHECK(remote.status_error());

    // New device: the name becomes a file, learning starts at POWER.
    remote.handle_key(Key::Escape);
    remote.handle_key(Key::Down);
    remote.handle_key(Key::Enter);
    CHECK(remote.view() == RemoteChip::View::Name);
    for (char c : std::string("Wall AC!")) remote.handle_char(std::string(1, c));
    remote.handle_key(Key::Enter);
    CHECK(remote.view() == RemoteChip::View::Learn);
    CHECK(remote.button() == 0);
    CHECK(remote.devices().back().file == (dir / "wall-ac.remote").string());
    remote.handle_key(Key::Escape);  // done: saved, back on its pad
    CHECK(remote.view() == RemoteChip::View::Pad);
    CHECK(fs::exists(dir / "wall-ac.remote"));
    CHECK(RemoteChip::slug("  Projector · room 2 ") == "projector-room-2");
    fs::remove_all(dir);
}

// --- RETRO ------------------------------------------------------------------

void test_retro_parsing() {
    using viewmodel::chips::RetroChip;
    const auto platforms = RetroChip::parse_platforms(
        R"([{"id":3,"slug":"snes","name":"Super Nintendo","rom_count":2},
            {"id":1,"slug":"gba","display_name":"Game Boy Advance","rom_count":5},
            {"id":9,"slug":"nes","name":"NES","rom_count":0},
            {"id":5,"slug":"genesis-slash-megadrive","name":"Genesis","rom_count":1}])");
    CHECK(platforms.size() == 3);  // empty systems get no tab
    CHECK(platforms[0].label == "GBA" && platforms[1].label == "SNES" && platforms[2].label == "GEN");

    const std::string paged = R"({"items":[
        {"id":12,"name":"Mega Man Battle Network 3","fs_name":"mmbn3.gba","platform_slug":"gba","fs_size_bytes":8388608,
         "metadatum":{"first_release_date":1017619200000}},
        {"id":11,"name":"Advance Wars","fs_name":"aw.gba","platform_slug":"gba","file_size_bytes":4194304},
        {"id":13,"name":"No file"}],"total":3})";
    const auto games = RetroChip::parse_games(paged);
    CHECK(games.size() == 2);
    CHECK(games[0].name == "Advance Wars" && games[0].size == 4194304);  // sorted, old size field
    CHECK(games[1].year == 2002);
    CHECK(RetroChip::parse_games(R"([{"id":1,"name":"Tetris","fs_name":"t.gb","platform_slug":"gb"}])").size() == 1);

    CHECK(RetroChip::fit_text("gba") == "1:1 pixel-perfect");
    CHECK(RetroChip::fit_text("snes") == "scaled 75%");
    CHECK(RetroChip::size_text(8388608) == "8.0 MB");
}

void test_retro_save_sync_helpers() {
    using viewmodel::chips::RetroChip;
    CHECK(RetroChip::parse_iso_time("2026-11-15T10:20:30+00:00") == 1794738030LL);
    CHECK(RetroChip::parse_iso_time("2026-11-15T10:20:30.123456Z") == 1794738030LL);
    CHECK(RetroChip::parse_iso_time("2026-11-15T05:20:30-05:00") == 1794738030LL);
    CHECK(RetroChip::parse_iso_time("garbage") == 0);
    CHECK(RetroChip::ago_text(1000, 1030) == "just now");
    CHECK(RetroChip::ago_text(0, 2 * 86400 + 5) == "2 days ago");
    CHECK(RetroChip::ago_text(0, 86400 + 5) == "yesterday");

    const auto saves = RetroChip::parse_saves(R"([
        {"id":1,"file_name":"Golden_Sun.srm","updated_at":"2026-11-10T00:00:00+00:00"},
        {"id":2,"file_name":"Golden_Sun.srm","updated_at":"2026-11-12T00:00:00+00:00"},
        {"id":3,"file_name":"golden sun (mgba).sav","updated_at":"2026-11-14T00:00:00+00:00"}])");
    CHECK(saves.size() == 3);
    CHECK(RetroChip::pick_save(saves, "Golden_Sun.srm")->id == 2);  // newest with our name
    CHECK(RetroChip::pick_save(saves, "Other.srm")->id == 3);       // else newest overall
    CHECK(RetroChip::pick_save({}, "x.srm") == nullptr);

    const std::string body = RetroChip::multipart_body("B0UND", "saveFile", "Golden_Sun.srm", std::string("a\0b", 3));
    CHECK(body.rfind("--B0UND\r\nContent-Disposition: form-data; name=\"saveFile\"; filename=\"Golden_Sun.srm\"\r\n", 0) == 0);
    CHECK(body.find(std::string("\r\n\r\na\0b\r\n--B0UND--\r\n", 18)) != std::string::npos);

    model::PetConfig config;
    config.retro_dir = "/data/roms";
    RetroChip retro(config);
    RetroChip::Game game;
    game.file = "Golden_Sun.zip";
    game.platform = "gba";
    CHECK(retro.local_save_path(game) == "/data/roms/gba/saves/Golden_Sun.srm");

    // Newest-wins needs a believable clock: one that was never set, or one that
    // is behind a RomM save's stamp, pauses the sync instead of guessing.
    const long long nov14 = RetroChip::parse_iso_time("2026-11-14T00:00:00+00:00");
    CHECK(RetroChip::clock_trusted(saves, nov14 + 3600));
    CHECK(RetroChip::clock_trusted(saves, nov14 - 60));      // small skew is fine
    CHECK(!RetroChip::clock_trusted(saves, nov14 - 86400));  // RomM has a save "from tomorrow"
    CHECK(!RetroChip::clock_trusted({}, 1700000000LL));      // clock never set (2023)
    CHECK(RetroChip::clock_trusted({}, nov14));
}

void test_retro_failed_pull_keeps_both_saves() {
    using viewmodel::chips::RetroChip;
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / ("pet-os-retro-test-" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir / "gba" / "saves");
    const fs::path save = dir / "gba" / "saves" / "G.srm";
    const fs::path marker = dir / "gba" / "saves" / "G.srm.conflict";
    std::ofstream(dir / "gba" / "G.gba") << "rom";
    std::ofstream(save) << "old";
    fs::last_write_time(save, fs::file_time_type::clock::now() - std::chrono::hours(1));
    const fs::path player = dir / "player.sh";
    std::ofstream(player) << "#!/bin/sh\nsleep 0.2\necho played > \"$1/G.srm\"\n";
    fs::permissions(player, fs::perms::owner_all);

    model::PetConfig config;
    config.romm_url = "http://127.0.0.1:9";  // nothing listens: every request fails at once
    config.retro_dir = dir.string();
    config.retro_cores["gba"] = "core";
    config.retro_player = player.string() + " {savedir}";
    RetroChip retro(config);
    const auto wait_until = [&](const std::function<bool()>& done) {
        for (int i = 0; i < 400 && !done(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            retro.tick(10);
        }
        return done();
    };

    RetroChip::Result games;
    games.what = RetroChip::Result::What::Games;
    RetroChip::Game game;
    game.id = 1;
    game.name = "G";
    game.file = "G.gba";
    game.platform = "gba";
    games.games.push_back(game);
    retro.on_result(games);
    retro.handle_key(Key::Enter);  // open the game: asks RomM for its saves (fails)
    CHECK(retro.view() == RetroChip::View::Details);
    bool good = false;
    CHECK(wait_until([&] { return retro.save_text(good) != "checking RomM..."; }));

    // RomM has a save from ten seconds ago; the PET's is an hour old.
    RetroChip::Result saves;
    saves.what = RetroChip::Result::What::Saves;
    saves.rom_id = 1;
    RetroChip::Save remote;
    remote.id = 5;
    remote.file_name = "G.srm";
    remote.updated = static_cast<long long>(std::time(nullptr)) - 10;
    saves.saves.push_back(remote);
    retro.on_result(saves);

    retro.handle_key(Key::Enter);  // play: pulls the newer save first, and that fails
    CHECK(wait_until([&] { return retro.view() != RetroChip::View::Downloading; }));
    CHECK(retro.view() == RetroChip::View::Details);  // did not start on the old save by itself
    CHECK(retro.status().find("ENTER") != std::string::npos);
    CHECK(!fs::exists(marker));

    retro.handle_key(Key::Enter);  // the user's choice: play the PET's save
    CHECK(retro.view() == RetroChip::View::Playing);
    CHECK(fs::exists(marker));
    CHECK(wait_until([&] { return retro.view() != RetroChip::View::Playing; }));
    // The session changed the save, but it is not uploaded over RomM's newer one.
    CHECK(retro.status() == "save kept on the PET: RomM has a different one");
    CHECK(retro.save_text(good) == "differs (not uploaded)");
    retro.leave();
    fs::remove_all(dir);
}

void test_run_capture_cap() {
    // A reply bigger than the default 1 MiB cap is kept whole when the caller
    // asks for more room (https downloads go through curl's stdout), and the
    // child is drained either way so it exits normally.
    const std::vector<std::string> argv = {"sh", "-c", "head -c 3000000 /dev/zero; printf END"};
    std::string big;
    CHECK(platform::proc::run_capture(argv, big, 10000, false, 4 * 1024 * 1024) == 0);
    CHECK(big.size() == 3000003);
    CHECK(big.compare(big.size() - 3, 3, "END") == 0);
    std::string capped;
    CHECK(platform::proc::run_capture(argv, capped, 10000, false) == 0);
    CHECK(capped.size() == 1024 * 1024);
}

void test_retro_needs_server() {
    using viewmodel::chips::RetroChip;
    model::PetConfig config;  // no [romm] url
    RetroChip retro(config);
    retro.enter();
    CHECK(retro.view() == RetroChip::View::Error);
    CHECK(!retro.keep_awake());
    CHECK(!retro.handle_key(Key::Escape));  // ESC leaves the chip
}

// --- LAN TECH ---------------------------------------------------------------

void test_lan_who_is_new() {
    using viewmodel::chips::LanTechChip;
    using Device = LanTechChip::Device;
    const long long now = 2000000000LL;
    // First sweep of a network: a baseline, nothing is new.
    std::vector<Device> first{{"192.168.1.10", "aa:aa", "printer", false, false},
                              {"192.168.1.11", "", "", false, false}};
    const std::string known = LanTechChip::merge_known("", false, now, first);
    CHECK(!first[0].is_new && !first[1].is_new);
    CHECK(known.find("aa:aa 2000000000 192.168.1.10 printer") != std::string::npos);
    CHECK(known.find("192.168.1.11") == std::string::npos);  // no MAC, not remembered
    // Next day: the printer is old news, a new MAC is new.
    std::vector<Device> later{{"192.168.1.10", "aa:aa", "printer", false, false},
                              {"192.168.1.20", "bb:bb", "", false, false}};
    const std::string known2 = LanTechChip::merge_known(known, true, now + 2 * 86400, later);
    CHECK(!later[0].is_new);
    CHECK(later[1].is_new);
    CHECK(known2.find("aa:aa 2000000000 ") != std::string::npos);  // first_seen kept
    CHECK(known2.find("bb:bb 2000172800 192.168.1.20 -") != std::string::npos);
    // Seen an hour ago for the first time: still new.
    std::vector<Device> again{{"192.168.1.20", "bb:bb", "", false, false}};
    LanTechChip::merge_known(known2, true, now + 2 * 86400 + 3600, again);
    CHECK(again[0].is_new);
}

void test_lan_helpers() {
    using viewmodel::chips::LanTechChip;
    CHECK(LanTechChip::speed_text(93.4, 11.6) == "93↓ / 12↑ Mb/s");
    CHECK(LanTechChip::speed_text(-1, -1) == "speed test failed");
    CHECK(LanTechChip::speed_text(40, -1) == "40↓ / 0↑ Mb/s");
    model::PetConfig config;
    LanTechChip lan(config);
    CHECK(!lan.can_try_static());  // nothing tested yet
    CHECK(!lan.keep_awake());
    const std::string report = lan.report_text("2026-11-17 09:30");
    CHECK(report.rfind("PET-os LAN TECH report · 2026-11-17 09:30\n", 0) == 0);
    CHECK(report.find("...   Link: ") != std::string::npos);
    CHECK(report.find("Switch port") != std::string::npos);
    CHECK(LanTechChip::common_ports().size() == 16);
}

void test_lan_static_config() {
    const auto defaults = model::parse_pet_config("");
    CHECK(defaults.lan_static.size() == 3);
    CHECK(defaults.lan_speed_test);
    const auto custom = model::parse_pet_config(
        "[lantech]\nstatic=172.16.5.250/24|172.16.5.1\nstatic=10.1.1.250/24|10.1.1.1\nspeed_test=off\n");
    CHECK(custom.lan_static.size() == 2);
    CHECK(custom.lan_static[0] == "172.16.5.250/24|172.16.5.1");
    CHECK(!custom.lan_speed_test);
}

// --- JACK IN ----------------------------------------------------------------

int dial(int port) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    timeval tv{5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return fd;
}

std::string read_until(int fd, const std::string& marker) {
    std::string got;
    char c;
    while (got.find(marker) == std::string::npos && recv(fd, &c, 1, 0) == 1) got += c;
    return got;
}

void test_jack_session() {
    using viewmodel::chips::JackInChip;
    model::PetConfig config;
    config.jack_gadget_up.clear();
    config.jack_gadget_down.clear();
    config.bridge_url.clear();
    config.jack_pc_ask = true;  // the [K] gate is opt-in since rev29
    JackInChip jack(config);
    jack.capture_sent = true;
    CHECK(jack.view() == JackInChip::View::Waiting);
    jack.on_line(R"({"t":"hello","host":"LAPTOP-FRONT","os":"Win 11","user":"front-desk"})");
    CHECK(jack.connected() && jack.view() == JackInChip::View::Session);
    // The PET names its operator (never the Windows user) right after hello.
    CHECK(!jack.sent.empty() && jack.sent[0] == R"({"t":"pet","operator":"Operator","control":false,"terminal":false,"trusted":false})");
    // Then the Navi's name; no [navi] picture, so the PC keeps its packaged art.
    CHECK(jack.sent.size() > 1 && jack.sent[1] == R"({"t":"navi","name":"Alice","art":""})");
    jack.sent.clear();
    CHECK(!jack.full() && !jack.admin());
    CHECK(jack.keep_awake());
    jack.on_line(R"({"t":"scan","cpu":78,"ram":91,"disk":"C:","disk_free":8,"startup":14,"defender":"off"})");
    CHECK(jack.scan().have && jack.scan().ram == 91);
    CHECK(JackInChip::scan_summary(jack.scan()) == "CPU 78% · RAM 91% · C: 8% free");
    CHECK(jack.alice_waiting());
    viewmodel::chips::BridgeReply reply;
    reply.ok = true;
    reply.reply = "Defender is off.\nCMD: `Set-MpPreference -DisableRealtimeMonitoring $false`";
    jack.on_alice_reply(reply);
    CHECK(jack.alice_text() == "Defender is off.");
    CHECK(jack.suggestion() == "Set-MpPreference -DisableRealtimeMonitoring $false");
    // Read-only: ENTER doesn't send the fix.
    jack.handle_key(viewmodel::Key::Enter);
    CHECK(jack.sent.empty());
    CHECK(jack.status().find("read-only") != std::string::npos);
    // [F] only asks; the PC decides.
    jack.handle_char("f");
    CHECK(jack.sent.size() == 1 && jack.sent[0] == R"({"t":"ask","what":"full"})");
    CHECK(!jack.full() && jack.asked_full());
    jack.on_line(R"({"t":"state","full":true,"admin":false,"net":false})");
    CHECK(jack.full() && !jack.asked_full());
    jack.handle_key(viewmodel::Key::Enter);
    CHECK(jack.sent.back() == R"({"t":"run","id":1,"cmd":"Set-MpPreference -DisableRealtimeMonitoring $false"})");
    CHECK(jack.log().back().kind == JackInChip::LogLine::Kind::Pending);
    jack.on_line(R"({"t":"result","id":1,"ok":true,"out":""})");
    CHECK(jack.log().back().kind == JackInChip::LogLine::Kind::Ok);
    // [F] again takes access away without asking.
    jack.handle_char("f");
    CHECK(jack.sent.back() == R"({"t":"drop","what":"full"})");
    // [X] jacks out.
    jack.handle_char("x");
    CHECK(jack.sent.back().rfind(R"({"t":"jackout")", 0) == 0);
    CHECK(!jack.connected() && jack.view() == JackInChip::View::Waiting);
    CHECK(!jack.keep_awake());
    // Chat from the PC app goes to Alice through the PET and the answer goes back.
    jack.on_line(R"({"t":"hello","host":"LAPTOP-FRONT","os":"Win 11","user":"front-desk"})");
    // ... once Holden allowed this PC with [K]: Alice is reached with the PET's login.
    jack.on_line(R"({"t":"chat","text":"why is this PC slow?"})");
    CHECK(!jack.chat_waiting() && jack.pc_wants() == "control" && jack.take_attention());
    CHECK(jack.sent.back().find("Press [K]") != std::string::npos);
    jack.on_line(R"({"t":"voice","wav":"AAAA"})");
    CHECK(!jack.chat_waiting());
    jack.handle_char("k");
    CHECK(jack.pc_control() && jack.pc_wants().empty());
    jack.on_line(R"({"t":"chat","text":"why is this PC slow?"})");
    CHECK(jack.chat_waiting());
    CHECK(jack.log().back().text == "Operator: why is this PC slow?");
    CHECK(jack.agent_running());
    viewmodel::chips::BridgeReply chat;
    chat.ok = true;
    chat.reply = "Memory is nearly full.\nCMD: Stop-Process -Name OneDrive";
    jack.on_chat_reply(chat);
    CHECK(!jack.chat_waiting());
    // The loop sends her command to the PC itself (the PC applies its mode) and
    // feeds the output back to her as the next turn.
    CHECK(jack.sent.size() >= 2);
    CHECK(jack.sent[jack.sent.size() - 2] == R"({"t":"alice","ok":true,"text":"Memory is nearly full.","heard":"","cmd":"Stop-Process -Name OneDrive","step":1})");
    CHECK(jack.sent.back().find(R"("agent":true,"cmd":"Stop-Process -Name OneDrive")") != std::string::npos);
    CHECK(jack.suggestion().empty() && jack.agent_steps() == 1);
    const std::string run_line = jack.sent.back();
    const std::string run_id = run_line.substr(run_line.find("\"id\":") + 5, run_line.find(',', run_line.find("\"id\":")) - run_line.find("\"id\":") - 5);
    jack.on_line(R"({"t":"result","id":)" + run_id + R"(,"ok":false,"out":"declined on the PC","reason":"declined"})");
    CHECK(jack.chat_waiting());
    chat.reply = "OK, leaving it.";
    jack.on_chat_reply(chat);
    CHECK(!jack.agent_running() && jack.sent.back().find(R"("cmd":"","step":0)") != std::string::npos);
    // FULL: 25 commands, then "Keep going?" on the PC; Yes runs the held one.
    jack.on_line(R"({"t":"state","full":true,"admin":false,"net":false,"input":true,"mode":"full"})");
    jack.on_line(R"({"t":"chat","text":"fix it"})");
    chat.reply = "CMD: Get-Date";
    for (int i = 0; i < JackInChip::kAgentMaxStepsFull; ++i) {
        jack.on_chat_reply(chat);
        const std::string r = jack.sent.back();
        const size_t at = r.find("\"id\":") + 5;
        jack.on_line(R"({"t":"result","ok":true,"out":"x","id":)" + r.substr(at, r.find(',', at) - at) + "}");
    }
    CHECK(jack.agent_steps() == 25);
    jack.on_chat_reply(chat);
    CHECK(jack.agent_holding() && jack.sent.back().rfind(R"({"t":"agent_more","steps":25)", 0) == 0);
    jack.on_line(R"({"t":"agent_more","yes":true})");
    CHECK(!jack.agent_holding() && jack.agent_steps() == 26 && jack.sent.back().find(R"("agent":true)") != std::string::npos);
    jack.on_line(R"({"t":"abort"})");
    CHECK(!jack.agent_running());
    CHECK(JackInChip::output_prompt("full", "Get-Process", true, "", std::string(5000, 'x')).size() <= viewmodel::chips::kBridgeMaxInput);
    CHECK(JackInChip::chat_prompt("LAPTOP-FRONT", "read", "hi").find("READ") != std::string::npos);
    jack.on_line(R"({"t":"voice","wav":"not base64!"})");
    CHECK(!jack.chat_waiting());
    CHECK(jack.sent.back().find("didn't come through") != std::string::npos);
    platform::DeviceStatus st;
    CHECK(platform::net_route(st, false) == platform::NetRoute::Offline);
    st.wifi_connected = true;
    CHECK(platform::net_route(st, false) == platform::NetRoute::WiFi);
    st.ethernet_connected = true;
    CHECK(platform::net_route(st, false) == platform::NetRoute::Lan);
    CHECK(platform::net_route(st, true) == platform::NetRoute::PcNet);
    CHECK(std::string(platform::net_route_label(platform::NetRoute::PcNet)) == "PC NET");
    std::string raw;
    CHECK(platform::jack::base64_decode("UklGRg==", raw) && raw == "RIFF");
    CHECK(platform::jack::base64_decode("YWJj\nZA", raw) && raw == "abcd");
    std::string words, cmd;
    JackInChip::split_reply("All good here.", words, cmd);
    CHECK(words == "All good here." && cmd.empty());
    CHECK(JackInChip::alice_prompt("PC", "Win 11", jack.scan()).size() <= viewmodel::chips::kBridgeMaxInput);
}

void test_jack_line_server() {
    platform::jack::LineServer server;
    const int port = 47000 + (::getpid() % 1000);
    CHECK(server.listen("127.0.0.1", port).empty());
    const int pc = dial(port);
    CHECK(pc >= 0);
    const std::string hello = "{\"t\":\"hello\"}\n{\"t\":\"sc";
    send(pc, hello.data(), hello.size(), 0);
    platform::jack::LineServer::Poll got;
    for (int i = 0; i < 50 && got.lines.empty(); ++i) {
        auto p = server.poll();
        got.connected |= p.connected;
        got.lines.insert(got.lines.end(), p.lines.begin(), p.lines.end());
        if (got.lines.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(got.connected && got.lines.size() == 1 && got.lines[0] == "{\"t\":\"hello\"}");
    send(pc, "an\"}\n", 5, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    auto more = server.poll();
    CHECK(more.lines.size() == 1 && more.lines[0] == "{\"t\":\"scan\"}");
    CHECK(server.send("{\"t\":\"jackout\"}"));
    CHECK(read_until(pc, "\n") == "{\"t\":\"jackout\"}\n");
    close(pc);
    bool gone = false;
    for (int i = 0; i < 50 && !gone; ++i) {
        gone = server.poll().disconnected;
        if (!gone) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(gone && !server.connected());
}

void test_jack_relay() {
    using platform::jack::Relay;
    std::string host;
    int port = 0;
    bool is_connect = false;
    CHECK(Relay::parse_request("CONNECT api.example.com:443 HTTP/1.1\r\nHost: x\r\n\r\n", host, port, is_connect));
    CHECK(host == "api.example.com" && port == 443 && is_connect);
    CHECK(Relay::parse_request("GET http://example.com/a/b HTTP/1.1\r\n\r\n", host, port, is_connect));
    CHECK(host == "example.com" && port == 80 && !is_connect);
    CHECK(Relay::parse_request("CONNECT [2001:db8::1]:8443 HTTP/1.1\r\n\r\n", host, port, is_connect));
    CHECK(host == "2001:db8::1" && port == 8443);
    CHECK(!Relay::parse_request("GET /local HTTP/1.1\r\n\r\n", host, port, is_connect));
    CHECK(!Relay::parse_request("CONNECT nohost HTTP/1.1\r\n\r\n", host, port, is_connect));

    // A fake PC: dials the tunnel port back and plays the far server (echo with a prefix).
    const int base = 48000 + (::getpid() % 1000);
    Relay relay;
    std::string opened;
    std::thread pc;
    CHECK(relay.start("127.0.0.1", base, "127.0.0.1", base + 1, [&](int id, const std::string& h, int p) {
        opened = h + ":" + std::to_string(p);
        pc = std::thread([id, base]() {
            const int t = dial(base + 1);
            const std::string hi = "tunnel " + std::to_string(id) + "\n";
            send(t, hi.data(), hi.size(), 0);
            const std::string ping = read_until(t, "ping");
            const std::string pong = "pong:" + ping;
            send(t, pong.data(), pong.size(), 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            close(t);
        });
    }).empty());
    const int app = dial(base);
    const std::string req = "CONNECT api.example.com:443 HTTP/1.1\r\nHost: api.example.com:443\r\n\r\n";
    send(app, req.data(), req.size(), 0);
    CHECK(read_until(app, "\r\n\r\n") == "HTTP/1.1 200 Connection established\r\n\r\n");
    send(app, "ping", 4, 0);
    CHECK(read_until(app, "pong:ping") == "pong:ping");
    CHECK(opened == "api.example.com:443");
    close(app);
    if (pc.joinable()) pc.join();
    // A host the PC can't reach: 502.
    std::thread failer;
    relay.stop();
    CHECK(relay.start("127.0.0.1", base + 2, "127.0.0.1", base + 3, [&](int id, const std::string&, int) {
        failer = std::thread([&relay, id]() { relay.fail(id); });
    }).empty());
    const int app3 = dial(base + 2);
    const std::string req3 = "CONNECT down.example:443 HTTP/1.1\r\n\r\n";
    send(app3, req3.data(), req3.size(), 0);
    CHECK(read_until(app3, "\r\n").rfind("HTTP/1.1 502", 0) == 0);
    close(app3);
    if (failer.joinable()) failer.join();
    relay.stop();
}

// Records what reaches a chip the PC opened through JACK IN.
class ProbeChip : public viewmodel::ChipModule {
public:
    explicit ProbeChip(const char* id) : id_(id) {}
    const char* id() const override { return id_; }
    bool handle_key(viewmodel::Key key) override {
        keys.push_back(key);
        return key != viewmodel::Key::Escape;  // ESC at the top level leaves the chip
    }
    void enter() override { ++entered; }
    int entered{0};
    void handle_char(const std::string& utf8) override { text += utf8; }
    std::vector<viewmodel::Key> keys;
    std::string text;

private:
    const char* id_;
};

// Regression (rev12): with the PET panel showing the Jack In "OPERATING" card, the
// PC's keys, typing and mouse must still reach the chip it opened.
void test_pc_input_reaches_open_chip() {
    using viewmodel::Key;
    using viewmodel::chips::JackInChip;
    model::PetConfig config;
    config.jack_gadget_up.clear();
    config.jack_gadget_down.clear();
    config.bridge_url.clear();
    for (const char* id : {"jellyfin", "retro", "terminal", "lan-tech"}) for (const bool napped : {false, true}) {
        AliceViewModel vm;
        auto jack_owned = std::make_unique<JackInChip>(config);
        JackInChip& jack = *jack_owned;
        jack.capture_sent = true;
        auto probe_owned = std::make_unique<ProbeChip>(id);
        ProbeChip& probe = *probe_owned;
        vm.set_chip_module(model::chip_index("jack-in"), std::move(jack_owned));
        vm.set_chip_module(model::chip_index(id), std::move(probe_owned));
        vm.handle_key(Key::Enter);  // skip boot
        vm.open_chip(model::chip_index("jack-in"));
        // The PET napped on the Jack In screen while JackIn.cmd was starting: the
        // link must not leave the PC's keys waking it (which closed the PC's chip).
        if (napped) {
            vm.sleep_now();
            CHECK(vm.sleeping());
        }
        // The lines JackIn.ps1 sends (ConvertTo-Json -Compress of its ordered hashtables).
        jack.on_line(R"({"t":"hello","host":"HK-DESKTOP","os":"Windows 11","user":"hkrip","admin":false,"full":true,"input":true,"mode":"ask","app":"0.8"})");
        jack.on_line(R"({"t":"state","full":true,"admin":false,"net":false,"input":true,"mode":"ask"})");
        // No [K] on the PET: a linked PC may open and drive chips, Terminal included.
        jack.on_line(std::string(R"({"t":"open","chip":")") + id + "\"}");
        jack.on_line(R"({"t":"mirror","on":true})");
        viewmodel::drain_pc_requests(vm, nullptr);
        CHECK(vm.active_chip() == &probe);
        CHECK(jack.pc_chip() == id);
        int pointers = 0;
        const auto pointer = [&](const JackInChip::Input& in) {
            ++pointers;
            CHECK(in.x == 40 && in.y == 60);
        };
        for (const char* k : {"Down", "Up", "Left", "Right", "Enter", "Tab", "Backspace"}) {
            jack.on_line(std::string(R"({"t":"key","k":")") + k + "\"}");
        }
        jack.on_line(R"({"t":"char","c":"h"})");
        jack.on_line(R"({"t":"char","c":"i"})");
        jack.on_line(R"({"t":"pointer","x":40,"y":60,"b":"down"})");
        jack.on_line(R"({"t":"pointer","x":40,"y":60,"b":"up"})");
        jack.on_line(R"({"t":"key","k":"Escape"})");  // right click
        const auto batch = viewmodel::drain_pc_requests(vm, pointer);
        CHECK(batch.keys == 8 && batch.chars == 2 && batch.pointers == 2 && batch.dropped == 0);
        CHECK(probe.keys.size() == 8 && probe.keys.front() == Key::Down && probe.keys.back() == Key::Escape);
        CHECK(probe.text == "hi");
        CHECK(pointers == 2);
        CHECK(!vm.sleeping() && vm.active_chip() == &probe);
        // That ESC (right click) left the chip on the PET; the PC's chip came back
        // up and the next key still reaches it.
        jack.on_line(R"({"t":"key","k":"Down"})");
        viewmodel::drain_pc_requests(vm, pointer);
        CHECK(vm.active_chip() == &probe && probe.keys.size() == 9 && probe.entered == 2);
    }
}

// The PC can't give itself the PET: opening chips and typing into them need [K]
// on the PET, Terminal needs its own [K], and both end with the session.
void test_jack_pc_control_is_granted_on_the_pet() {
    using viewmodel::chips::JackInChip;
    model::PetConfig config;
    config.jack_gadget_up.clear();
    config.jack_gadget_down.clear();
    config.bridge_url.clear();
    config.jack_pc_ask = true;
    JackInChip jack(config);
    jack.capture_sent = true;
    std::string id;
    JackInChip::Input input;
    // What any program on the PC can send: it says it has input, then opens Terminal and types.
    jack.on_line(R"({"t":"hello","host":"STORE-PC","os":"Windows 11","user":"x","full":true,"input":true,"mode":"full"})");
    CHECK(!jack.pc_control() && !jack.pc_terminal());
    jack.on_line(R"({"t":"open","chip":"terminal"})");
    jack.on_line(R"({"t":"key","k":"Enter"})");
    jack.on_line(R"({"t":"char","c":"x"})");
    jack.on_line(R"({"t":"pointer","x":1,"y":1,"b":"down"})");
    CHECK(!jack.take_open(id) && !jack.take_input(input));
    CHECK(jack.sent.back() == R"({"t":"denied","what":"control","chip":"terminal"})");
    CHECK(jack.pc_wants() == "control" && jack.take_attention() && !jack.take_attention());
    CHECK(jack.status().find("[K]") != std::string::npos);
    // First [K]: chips, but never Terminal along with it.
    jack.handle_char("k");
    CHECK(jack.pc_control() && !jack.pc_terminal() && !jack.take_open(id));
    CHECK(jack.sent[jack.sent.size() - 2] == R"({"t":"pet","operator":"Operator","control":true,"terminal":false,"trusted":false})");
    CHECK(jack.sent.back() == R"({"t":"denied","what":"terminal","chip":"terminal"})" && jack.pc_wants() == "terminal");
    jack.on_line(R"({"t":"open","chip":"jellyfin"})");
    CHECK(jack.take_open(id) && id == "jellyfin");
    jack.on_line(R"({"t":"key","k":"Down"})");
    CHECK(jack.take_input(input) && input.key == viewmodel::Key::Down);
    jack.on_line(R"({"t":"open","chip":"terminal"})");
    CHECK(!jack.take_open(id) && jack.pc_wants() == "terminal");
    CHECK(jack.sent.back() == R"({"t":"denied","what":"terminal","chip":"terminal"})");
    // Second [K]: Terminal, and the chip the PC was waiting for opens.
    jack.handle_char("k");
    CHECK(jack.pc_terminal() && jack.take_open(id) && id == "terminal" && jack.pc_wants().empty());
    // [K] with nothing waiting takes it all back.
    jack.handle_char("K");
    CHECK(!jack.pc_control() && !jack.pc_terminal());
    jack.on_line(R"({"t":"key","k":"Down"})");
    CHECK(!jack.take_input(input));
    // A PC that only reads still gets nothing typed, even with [K].
    jack.handle_char("k");
    jack.on_line(R"({"t":"state","full":false,"input":false,"mode":"read"})");
    jack.on_line(R"({"t":"key","k":"Down"})");
    CHECK(!jack.take_input(input));
    // A new link (another program dialing in) starts without any grant.
    jack.on_line(R"({"t":"state","full":true,"input":true,"mode":"full"})");
    jack.on_line(R"({"t":"open","chip":"terminal"})");
    jack.handle_char("k");
    CHECK(jack.pc_terminal());
    jack.on_line(R"({"t":"hello","host":"STORE-PC","os":"Windows 11","user":"x","full":true,"input":true,"mode":"full"})");
    CHECK(!jack.pc_control() && !jack.pc_terminal() && jack.pc_wants().empty());
    // The default (no ask_on_pet): chips, Terminal and Alice from the start, nothing pressed.
    config.jack_pc_ask = false;
    JackInChip open_pet(config);
    open_pet.capture_sent = true;
    open_pet.on_line(R"({"t":"hello","host":"HK-DESKTOP","os":"Windows 11","user":"hk","full":true,"input":true,"mode":"ask"})");
    CHECK(open_pet.pc_control() && open_pet.pc_terminal());
    CHECK(open_pet.sent[0] == R"({"t":"pet","operator":"Operator","control":true,"terminal":true,"trusted":false})");
    open_pet.on_line(R"({"t":"open","chip":"retro"})");
    CHECK(open_pet.take_open(id) && id == "retro");
    open_pet.on_line(R"({"t":"open","chip":"terminal"})");
    CHECK(open_pet.take_open(id) && id == "terminal" && open_pet.pc_wants().empty());
    open_pet.on_line(R"({"t":"key","k":"Down"})");
    CHECK(open_pet.take_input(input) && input.key == viewmodel::Key::Down);
    open_pet.on_line(R"({"t":"chat","text":"hi"})");
    CHECK(open_pet.chat_waiting() && open_pet.pc_wants().empty());
    // [K] is still there to take it all back, and to give it again (Terminal with it).
    open_pet.handle_char("k");
    CHECK(!open_pet.pc_control() && !open_pet.pc_terminal());
    open_pet.handle_char("k");
    CHECK(open_pet.pc_control() && open_pet.pc_terminal());
    // An old config file that still says pc_control=ask changes nothing; ask_on_pet=on brings the gate back.
    CHECK(!model::parse_pet_config("[jackin]\npc_control=ask\n").jack_pc_ask);
    CHECK(model::parse_pet_config("[jackin]\nask_on_pet=on\n").jack_pc_ask);
    // The chat header leaves most of the bridge's 600 characters to the message.
    const std::string asked(380, 'q');
    const std::string prompt = JackInChip::chat_prompt("LAPTOP-FRONT", "full", asked);
    CHECK(prompt.size() <= viewmodel::chips::kBridgeMaxInput);
    CHECK(prompt.find(asked) != std::string::npos);
}

// A PC that stops reading (a Yes/No box is open there) must not freeze the PET
// or lose the link: frames are skipped, other lines wait and arrive in order.
void test_jack_link_send_never_blocks() {
    using platform::jack::LineServer;
    LineServer server;
    int port = 0;
    for (int p = 47400 + static_cast<int>(::getpid() % 500); p < 48400 && port == 0; ++p) {
        if (server.listen("127.0.0.1", p).empty()) port = p;
    }
    CHECK(port != 0);
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    CHECK(::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    for (int i = 0; i < 50 && !server.connected(); ++i) {
        server.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(server.connected());
    const std::string frame = "F" + std::string(8000, 'f');
    const auto start = std::chrono::steady_clock::now();
    int sent_frames = 0;
    int refused = 0;
    for (int i = 0; i < 2000; ++i) {
        if (server.send(frame, true)) ++sent_frames;
        else ++refused;
    }
    CHECK(server.send("LAST", false));  // a control line still queues behind the frames
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    CHECK(ms < 1500.0);                 // 16 MB offered to a reader that reads nothing: no 2 s waits
    CHECK(refused > 0 && sent_frames > 0);
    CHECK(server.connected());
    CHECK(server.backlog() <= LineServer::kDropBacklog + 2 * frame.size());
    // The PC reads again: every line that was accepted arrives whole and in order.
    std::string got;
    char buf[65536];
    for (int i = 0; i < 4000 && got.find("LAST\n") == std::string::npos; ++i) {
        server.poll();
        const ssize_t n = ::recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
        if (n > 0) got.append(buf, static_cast<size_t>(n));
        else std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(got.size() == static_cast<size_t>(sent_frames) * (frame.size() + 1) + 5);
    CHECK(got.compare(got.size() - 5, 5, "LAST\n") == 0);
    CHECK(server.backlog() == 0);
    ::close(fd);
}

void test_decode_chunked_bounds() {
    using platform::net::decode_chunked;
    CHECK(decode_chunked("5\r\nhello\r\n3\r\nabc\r\n0\r\n\r\n") == "helloabc");
    // A chunk size that wraps the position (it used to loop forever, growing the body).
    const std::string evil = "ffffffffffffffec\r\n" + std::string(4000, 'x');
    CHECK(decode_chunked(evil).size() <= evil.size());
    CHECK(decode_chunked("ffffffffffffffff\r\nabc") == "abc");
    CHECK(decode_chunked("10\r\nshort") == "short");  // cut-off reply: what arrived is kept
    CHECK(decode_chunked("zz\r\nabc").empty());
}

// A game started before RomM said which saves it has: the PET's save is not
// uploaded over whatever RomM holds until a fresh check allows it.
void test_retro_unknown_saves_are_not_overwritten() {
    using viewmodel::chips::RetroChip;
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / ("pet-os-retro-unknown-" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir / "gba" / "saves");
    const fs::path save = dir / "gba" / "saves" / "G.srm";
    const fs::path marker = dir / "gba" / "saves" / "G.srm.conflict";
    std::ofstream(dir / "gba" / "G.gba") << "rom";
    std::ofstream(save) << "old";
    fs::last_write_time(save, fs::file_time_type::clock::now() - std::chrono::hours(1));
    const fs::path player = dir / "player.sh";
    std::ofstream(player) << "#!/bin/sh\nsleep 0.2\necho played > \"$1/G.srm\"\n";
    fs::permissions(player, fs::perms::owner_all);

    model::PetConfig config;
    config.romm_url = "http://127.0.0.1:9";  // nothing listens: every request fails at once
    config.retro_dir = dir.string();
    config.retro_cores["gba"] = "core";
    config.retro_player = player.string() + " {savedir}";
    RetroChip retro(config);
    const auto wait_until = [&](const std::function<bool()>& done) {
        for (int i = 0; i < 400 && !done(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            retro.tick(10);
        }
        return done();
    };
    RetroChip::Result games;
    games.what = RetroChip::Result::What::Games;
    RetroChip::Game game;
    game.id = 1;
    game.name = "G";
    game.file = "G.gba";
    game.platform = "gba";
    games.games.push_back(game);
    retro.on_result(games);
    retro.handle_key(Key::Enter);  // details: asks RomM for its saves
    retro.handle_key(Key::Enter);  // play at once, before (or just as) the answer comes
    // It either waited for the check or started after it failed; never on a guess.
    CHECK(wait_until([&] { return retro.view() == RetroChip::View::Playing; }));
    CHECK(wait_until([&] { return retro.view() != RetroChip::View::Playing; }));
    // RomM couldn't be asked afterwards either: the changed save stays on the PET.
    CHECK(wait_until([&] { return retro.status() == "save kept on the PET: couldn't check RomM"; }));
    CHECK(!fs::exists(marker));

    // Same again, but this time the check after the play answers: RomM holds a
    // save newer than the one the PET started from, so both are kept.
    fs::last_write_time(save, fs::file_time_type::clock::now() - std::chrono::hours(1));
    retro.handle_key(Key::Enter);
    CHECK(wait_until([&] { return retro.view() == RetroChip::View::Playing; }));
    CHECK(wait_until([&] { return retro.view() != RetroChip::View::Playing; }));
    CHECK(wait_until([&] { return retro.status() == "save kept on the PET: couldn't check RomM"; }));
    RetroChip::Result check;
    check.what = RetroChip::Result::What::Saves;
    check.rom_id = 1;
    check.then_push = true;
    RetroChip::Save remote;
    remote.id = 7;
    remote.file_name = "G.srm";
    remote.updated = static_cast<long long>(std::time(nullptr)) - 600;  // ten minutes ago, PET started from an hour-old save
    check.saves.push_back(remote);
    retro.on_result(check);
    CHECK(retro.status() == "save kept on the PET: RomM has a different one");
    CHECK(fs::exists(marker));
    retro.leave();
    fs::remove_all(dir);
}

void test_retro_pagination_and_cache() {
    using viewmodel::chips::RetroChip;
    using Game = RetroChip::Game;
    // Page parse keeps server order and RomM's total.
    int total = 0;
    auto page = RetroChip::parse_games_page(R"({"items":[
        {"id":5,"name":"Zelda","fs_name":"z.gba","platform_slug":"gba"},
        {"id":4,"name":"Advance Wars","fs_name":"aw.gba","platform_slug":"gba"}],"total":87,"limit":25,"offset":0})", total);
    CHECK(total == 87 && page.size() == 2 && page[0].id == 5);
    RetroChip::parse_games_page("[]", total);
    CHECK(total == 0);
    RetroChip::parse_games_page("not json", total);
    CHECK(total == -1);

    const auto url = RetroChip::games_url("https://r", 3, 50, 25);
    CHECK(url.find("limit=25") != std::string::npos && url.find("offset=50") != std::string::npos);
    CHECK(url.find("platform_ids=3") != std::string::npos && url.find("with_char_index=false") != std::string::npos);
    CHECK(RetroChip::games_url("https://r", 0, 0, 25).find("platform_ids") == std::string::npos);

    auto mk = [](int id) { Game g; g.id = id; g.name = "g" + std::to_string(id); g.file = "f"; return g; };
    // Appending pages.
    std::vector<Game> games;
    RetroChip::merge_page(games, 0, {mk(1), mk(2)});
    RetroChip::merge_page(games, 2, {mk(3), mk(4)});
    CHECK(games.size() == 4 && games[2].id == 3);
    // Refresh over a cached list: page 0 replaces the head, the cached tail stays,
    // and an id that moved into the page is not shown twice.
    std::vector<Game> cached{mk(1), mk(2), mk(3), mk(4), mk(5)};
    RetroChip::merge_page(cached, 0, {mk(9), mk(1), mk(4)});
    CHECK(cached.size() == 4);  // 2 and 3 sat in the refreshed range; their own page brings them back
    CHECK(cached[0].id == 9 && cached[1].id == 1 && cached[2].id == 4 && cached[3].id == 5);
    int seen4 = 0;
    for (const auto& g : cached) seen4 += g.id == 4;
    CHECK(seen4 == 1);
    // Offset past the end just appends.
    std::vector<Game> few{mk(1)};
    RetroChip::merge_page(few, 10, {mk(2)});
    CHECK(few.size() == 2 && few[1].id == 2);

    // Lean cache round trip, bounded.
    Game g = mk(7);
    g.name = "Quote \"and\" slash \\";
    g.platform = "gba";
    g.size = 4194304;
    g.year = 2001;
    std::vector<Game> list{g, mk(8), mk(9)};
    const auto text = RetroChip::cache_json(342, list, 2);
    int ctotal = 0;
    const auto back = RetroChip::parse_cache(text, ctotal);
    CHECK(back.size() == 2 && ctotal == 342);
    CHECK(back[0].name == g.name && back[0].platform == "gba" && back[0].size == 4194304 && back[0].year == 2001);
    CHECK(RetroChip::parse_cache("garbage", ctotal).empty() && ctotal == 0);
    CHECK(text.size() < 1024);

    std::vector<RetroChip::Platform> plats{{0, "", "ALL", 9}, {1, "gba", "GBA", 9}};
    CHECK(RetroChip::parse_platforms(RetroChip::platforms_cache_json(plats)).size() == 1);
}

void test_retro_scrolling_systems_and_names() {
    using viewmodel::chips::RetroChip;
    using Game = RetroChip::Game;
    namespace fs = std::filesystem;
    // [retro] systems= parses to slugs; tabs follow it, matched loosely, no ALL.
    const auto conf_path = fs::temp_directory_path() / ("pet-os-retro-conf-" + std::to_string(::getpid()) + ".conf");
    std::ofstream(conf_path) << "[retro]\nsystems = gba,gb, GBC,nes,snes,gamegear,wonderswan,wonderswancolor\n";
    model::PetConfig config;
    CHECK(model::load_pet_config(conf_path.string(), config));
    fs::remove(conf_path);
    CHECK(config.retro_systems.size() == 8 && config.retro_systems[2] == "gbc" && config.retro_systems[7] == "wonderswancolor");
    const auto server = RetroChip::parse_platforms(R"([
        {"id":4,"slug":"gb","name":"Game Boy","rom_count":22},{"id":1,"slug":"gba","rom_count":87},
        {"id":2,"slug":"browser","rom_count":111},{"id":3,"slug":"gamegear","rom_count":11},
        {"id":11,"slug":"wonderswan-color","rom_count":4},{"id":5,"slug":"gbc","rom_count":34},
        {"id":12,"slug":"nes","rom_count":13},{"id":9,"slug":"snes","rom_count":22}])");
    const auto shown = RetroChip::filter_platforms(server, config.retro_systems);
    CHECK(shown.size() == 7 && shown[0].slug == "gba" && shown[1].slug == "gb" && shown[5].slug == "gamegear");
    CHECK(shown[6].slug == "wonderswan-color" && shown[6].label == "WSC" && shown[5].label == "GG");
    CHECK(RetroChip::filter_platforms(server, {}).size() == server.size());

    // Card titles: whole words per line, "..." only at the end.
    CHECK(RetroChip::wrap_title("Achievement Unlocked", 13, 3) == "Achievement\nUnlocked");
    CHECK(RetroChip::wrap_title("Advance Wars 2: Black Hole Rising", 13, 3) == "Advance Wars\n2: Black Hole\nRising");
    CHECK(RetroChip::wrap_title("The Legend of Zelda: A Link to the Past & Four Swords", 13, 3) ==
          "The Legend of\nZelda: A Link\nto the...");
    CHECK(RetroChip::wrap_title("Supercalifragilistic", 8, 2) == "Supercal\nifrag...");
    CHECK(RetroChip::wrap_title("Tetris", 13, 3) == "Tetris");
    // Unmatched games (no RomM name) show their tag-free file name.
    int total = 0;
    const auto named = RetroChip::parse_games_page(R"({"items":[
        {"id":1,"name":null,"fs_name_no_tags":"Altered","fs_name_no_ext":"Altered (USA) [!]","fs_name":"Altered (USA) [!].gba"},
        {"id":2,"name":"","fs_name":"x.gba"}],"total":2})", total);
    CHECK(named.size() == 2 && named[0].name == "Altered" && named[1].name == "x.gba");

    // The library: first allowed system, arrows (and Z/C letters) move, the next page loads near the end.
    config.romm_url = "http://127.0.0.1:1";
    RetroChip retro(config);
    retro.set_cache_dir((fs::temp_directory_path() / ("pet-os-retro-cache-" + std::to_string(::getpid()))).string());
    RetroChip::Result plats;
    plats.what = RetroChip::Result::What::Platforms;
    plats.platforms = server;
    retro.on_result(plats);
    CHECK(retro.platforms().size() == 7 && retro.platforms()[0].slug == "gba" && retro.platform() == 0);
    auto mk = [](int id) { Game g; g.id = id; g.name = "Game " + std::to_string(id); g.file = "f.gba"; g.platform = "gba"; return g; };
    RetroChip::Result page;
    page.what = RetroChip::Result::What::Games;
    page.generation = 1;  // the first tab's list
    page.total = 87;
    for (int i = 1; i <= 25; ++i) page.games.push_back(mk(i));
    page.games.push_back(mk(3));  // RomM repeating a game: shown once
    retro.on_result(page);
    CHECK(retro.view() == RetroChip::View::Library && retro.games().size() == 25 && retro.selected() == 0);
    CHECK(!retro.loading_more() && retro.has_more());
    retro.handle_key(Key::Down);  // one game
    CHECK(retro.selected() == 1);
    retro.handle_char("x");  // plain X (Fn+X is the arrow)
    CHECK(retro.selected() == 2);
    retro.handle_char("f");
    CHECK(retro.selected() == 1);
    retro.handle_key(Key::Right);  // Left/Right move a page
    CHECK(retro.selected() == 1 + RetroChip::kPageStep);
    retro.handle_char("z");
    CHECK(retro.selected() == 1);
    retro.handle_char("c");  // plain C (Fn+C is the arrow)
    CHECK(retro.selected() == 1 + RetroChip::kPageStep);
    retro.handle_key(Key::Left);
    CHECK(retro.selected() == 1);
    while (retro.selected() < 16) retro.handle_key(Key::Down);
    CHECK(!retro.loading_more());
    retro.handle_key(Key::Down);  // 8 from the end of what's loaded
    CHECK(retro.selected() == 17 && retro.loading_more());
    for (int i = 0; i < 20; ++i) retro.handle_key(Key::Down);
    CHECK(retro.selected() == 24);  // the end of the loaded list until the page lands
    RetroChip::Result next = page;
    next.offset = 25;
    next.games.clear();
    for (int i = 26; i <= 50; ++i) next.games.push_back(mk(i));
    next.games.push_back(mk(26));
    retro.on_result(next);
    CHECK(retro.games().size() == 50);
    retro.handle_key(Key::Down);
    CHECK(retro.selected() == 25 && retro.game()->id == 26);
    retro.handle_char("/");  // search still opens; letters then type
    retro.handle_char("c");
    CHECK(retro.searching() && retro.search() == "c");
    retro.handle_key(Key::Escape);
    retro.handle_key(Key::Tab);  // next system
    CHECK(retro.platform() == 1 && retro.selected() == 0);
}

void test_retro_list_layout() {
    using viewmodel::chips::RetroChip;
    // The window keeps the selection on screen and stays inside the list.
    CHECK(RetroChip::list_first(3, 2, RetroChip::kListRows) == 0);  // short list: no scroll
    CHECK(RetroChip::list_first(20, 0, RetroChip::kListRows) == 0);
    CHECK(RetroChip::list_first(20, 3, RetroChip::kListRows) == 0);
    CHECK(RetroChip::list_first(20, 4, RetroChip::kListRows) == 1);  // selection centred
    CHECK(RetroChip::list_first(20, 10, RetroChip::kListRows) == 7);
    CHECK(RetroChip::list_first(20, 19, RetroChip::kListRows) == 13);  // last window, no overrun
    CHECK(RetroChip::list_first(8, 7, RetroChip::kListRows) == 1);
    for (size_t sel = 0; sel < 20; ++sel) {
        const size_t first = RetroChip::list_first(20, sel, RetroChip::kListRows);
        CHECK(sel >= first && sel < first + RetroChip::kListRows);
    }
    CHECK(RetroChip::list_first(5, 4, 0) == 0);

    // Single-line titles keep whole words; a long name is cut at a space, not mid-word.
    const std::string one = RetroChip::wrap_title("Mega Man Battle Network 3", 12, 1);
    CHECK(one.rfind("Mega Man", 0) == 0);
    CHECK(one.find("Ba") == std::string::npos || one.find("Battle") != std::string::npos);
    CHECK(RetroChip::wrap_title("Tetris", 12, 1) == "Tetris");
}

void test_jellyfin_pc_direct_play() {
    using viewmodel::chips::pc_direct_play;
    // H.264 MP4 family with AAC, MP3 or no audio plays as it is.
    CHECK(pc_direct_play("mov,mp4,m4a,3gp", "h264", "aac"));
    CHECK(pc_direct_play("mp4", "h264", "mp3"));
    CHECK(pc_direct_play("mp4", "h264", ""));
    CHECK(pc_direct_play("mov,mp4", "h264", "aac"));
    // MKV never direct-plays on the PC, even when the streams are fine.
    CHECK(!pc_direct_play("matroska,webm", "h264", "aac"));
    CHECK(!pc_direct_play("mkv", "h264", "aac"));
    // Other video or audio codecs transcode.
    CHECK(!pc_direct_play("mp4", "hevc", "aac"));
    CHECK(!pc_direct_play("mp4", "h264", "ac3"));
    CHECK(!pc_direct_play("", "h264", "aac"));
}

void test_jellyfin_item_ids_are_encoded() {
    using platform::net::url_encode;
    // Item IDs and keys go into URL paths and queries, so reserved characters must be escaped.
    CHECK(url_encode("abc123") == "abc123");
    CHECK(url_encode("a b&c/d?e=f") == "a%20b%26c%2Fd%3Fe%3Df");
    CHECK(url_encode("../Auth").find('/') == std::string::npos);
}

void test_jellyfin_list_navigation() {
    using viewmodel::chips::JellyfinChip;
    using R = JellyfinChip::Result;
    // Same keys as RETRO: Up/Down step one row, Left/Right move a page of 7 rows (no wrap),
    // Esc clears a filter before it leaves the chip, TAB switches season.
    model::PetConfig config;  // no server: results are fed in through on_result()
    JellyfinChip chip(config);
    std::vector<JellyfinChip::Item> shows;
    for (int i = 0; i < 12; ++i) {
        JellyfinChip::Item item;
        item.id = "s" + std::to_string(i);
        item.name = "Show " + std::to_string(i);
        item.type = "Series";
        shows.push_back(item);
    }
    R library{};
    library.what = R::What::Library;
    library.items = shows;
    library.generation = 0;
    chip.on_result(library);
    CHECK(chip.view() == JellyfinChip::View::Library);
    CHECK(chip.selected() == 0);

    chip.handle_key(Key::Up);
    CHECK(chip.selected() == 0);  // no wrap at the top
    chip.handle_key(Key::Down);
    chip.handle_key(Key::Down);
    CHECK(chip.selected() == 2);
    chip.handle_key(Key::Up);
    CHECK(chip.selected() == 1);
    chip.handle_key(Key::Right);
    CHECK(chip.selected() == 8);  // one page
    chip.handle_key(Key::Right);
    CHECK(chip.selected() == 11);  // stops at the last row
    chip.handle_key(Key::Down);
    CHECK(chip.selected() == 11);
    chip.handle_key(Key::Left);
    CHECK(chip.selected() == 4);
    for (int i = 0; i < 5; ++i) chip.handle_key(Key::Left);
    CHECK(chip.selected() == 0);

    // Search filters as you type; Esc clears the filter first, a second Esc leaves the chip.
    chip.handle_char("/");
    chip.handle_char("1");
    CHECK(chip.library().size() == 3);  // Show 1, Show 10, Show 11
    chip.handle_key(Key::Enter);
    CHECK(!chip.searching());
    CHECK(chip.library().size() == 3);
    CHECK(chip.handle_key(Key::Escape));
    CHECK(chip.library().size() == 12 && chip.search().empty());
    CHECK(!chip.handle_key(Key::Escape));

    // Backspace drops a whole UTF-8 character, not one byte of it.
    chip.handle_char("/");
    chip.handle_char("\xc3\xa9");  // é
    CHECK(chip.search() == "\xc3\xa9");
    chip.handle_key(Key::Backspace);
    CHECK(chip.search().empty());
    chip.handle_key(Key::Enter);

    // Opening a show: seasons arrive, Specials is skipped, episodes follow.
    chip.handle_key(Key::Enter);  // generation 1: seasons
    R seasons{};
    seasons.what = R::What::Seasons;
    seasons.generation = 1;
    seasons.seasons = {{"a", "Specials", 0}, {"b", "Season 1", 1}, {"c", "Season 2", 2}};
    chip.on_result(seasons);  // generation 2: episodes of season 1
    CHECK(chip.view() == JellyfinChip::View::Episodes);
    CHECK(chip.season() == 1);
    R episodes{};
    episodes.what = R::What::Episodes;
    episodes.generation = 2;
    for (int i = 1; i <= 9; ++i) {
        JellyfinChip::Item item;
        item.id = "e" + std::to_string(i);
        item.name = "Episode";
        item.type = "Episode";
        item.index = i;
        episodes.items.push_back(item);
    }
    chip.on_result(episodes);
    CHECK(chip.episodes().size() == 9);
    CHECK(chip.episode() == 0);

    chip.handle_key(Key::Down);
    CHECK(chip.episode() == 1);
    chip.handle_key(Key::Down);
    chip.handle_key(Key::Down);
    CHECK(chip.episode() == 3);
    chip.handle_key(Key::Right);  // a page
    CHECK(chip.episode() == 8);  // last episode
    chip.handle_key(Key::Down);
    CHECK(chip.episode() == 8);
    chip.handle_key(Key::Up);
    CHECK(chip.episode() == 7);
    chip.handle_key(Key::Left);  // a page back
    CHECK(chip.episode() == 0);
    chip.handle_key(Key::Right);
    CHECK(chip.episode() == 7);

    // TAB moves to the next season (generation 3); Left/Right stay on episodes.
    chip.handle_key(Key::Tab);
    CHECK(chip.season() == 2);
    CHECK(chip.view() == JellyfinChip::View::Episodes);
    R season_two{};
    season_two.what = R::What::Episodes;
    season_two.generation = 3;
    season_two.items = {episodes.items[0], episodes.items[1]};
    chip.on_result(season_two);
    CHECK(chip.episodes().size() == 2);
    CHECK(chip.episode() == 0);
    CHECK(chip.handle_key(Key::Escape));
    CHECK(chip.view() == JellyfinChip::View::Library);
}

void test_http_status_line() {
    using platform::net::parse_http_status;
    CHECK(parse_http_status("HTTP/1.1 200 OK\r\n\r\n") == 200);
    CHECK(parse_http_status("HTTP/1.0 404 Not Found\r\n\r\n") == 404);
    // No space on the status line: must not read the "HTTP/1.1" prefix as 0 or borrow a later space.
    CHECK(parse_http_status("HTTP/1.1\r\nServer: a b\r\n\r\n") == 0);
    CHECK(parse_http_status("HTTP/1.1\r\n\r\nbody with spaces 500") == 0);
    CHECK(parse_http_status("garbage 200\r\n\r\n") == 0);
    CHECK(parse_http_status("") == 0);
}

// Server strings can carry separators, quotes, CR/LF, or ".."; none may reach a path or header.
void test_retro_safe_names() {
    using viewmodel::chips::RetroChip;
    const std::string traversal = RetroChip::safe_name("../../etc/passwd", "fallback");
    CHECK(traversal == "etcpasswd");
    CHECK(traversal.find('/') == std::string::npos);
    CHECK(traversal.find("..") == std::string::npos);

    const std::string injected = RetroChip::safe_name("Game\"\r\nX-Evil: 1", "fallback");
    CHECK(injected == "GameX-Evil: 1");
    CHECK(injected.find_first_of("\r\n\"") == std::string::npos);

    CHECK(RetroChip::safe_name("", "other") == "other");
    CHECK(RetroChip::safe_name("..", "other") == "other");
    CHECK(RetroChip::safe_name("/\\", "other") == "other");
    CHECK(RetroChip::safe_name(std::string("\0", 1), "other") == "other");
    CHECK(RetroChip::safe_name("Zelda (USA).sfc", "x") == "Zelda (USA).sfc");

    // The multipart filename header keeps its two quote delimiters and nothing else.
    const std::string body = RetroChip::multipart_body("B", "saveFile", "a\"\r\nEvil: 1", "x");
    CHECK(body.find("filename=\"aEvil: 1\"\r\n") != std::string::npos);
    CHECK(body.find("\r\nEvil: 1") == std::string::npos);
}

// [T] twice trusts a PC: the PET makes the token, the PC shows it at the next
// link and gets chip control at once (never Terminal). Every session leaves a report.
void test_jack_trust_and_report() {
    using viewmodel::chips::JackInChip;
    namespace store = viewmodel::chips::jack_store;
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "pet-os-jack-store-test";
    std::filesystem::remove_all(dir);
    model::PetConfig config;
    config.jack_gadget_up.clear();
    config.jack_gadget_down.clear();
    config.bridge_url.clear();
    config.jack_pc_ask = true;
    config.clock_set.clear();
    config.jack_state_dir = dir.string();
    const char* hello = R"({"t":"hello","host":"HK-DESKTOP","os":"Windows 11","user":"hk","full":true,"input":true,"mode":"ask"})";
    std::string token;
    {
        JackInChip jack(config);
        jack.capture_sent = true;
        jack.on_line(hello);
        CHECK(!jack.trusted() && !jack.pc_control());
        // One [T] only asks; another key calls it off.
        jack.handle_char("t");
        CHECK(jack.trust_armed() && !jack.trusted() && jack.status().find("[T] again") != std::string::npos);
        jack.handle_char("l");
        jack.handle_char("l");
        CHECK(!jack.trust_armed());
        jack.handle_char("t");
        jack.handle_char("T");
        CHECK(jack.trusted() && jack.pc_control() && !jack.pc_terminal());
        const std::string head = R"({"t":"trust","token":")";
        for (const auto& line : jack.sent) {
            if (line.rfind(head, 0) == 0) token = line.substr(head.size(), 64);
        }
        CHECK(token.size() == 64 && token.find_first_not_of("0123456789abcdef") == std::string::npos);
        CHECK(jack.sent.back() == R"({"t":"pet","operator":"Operator","control":true,"terminal":false,"trusted":true})");
        jack.on_line(R"({"t":"scan","cpu":78,"ram":91,"disk":"C:","disk_free":8,"startup":14,"defender":"off"})");
        jack.on_line(R"({"t":"log","text":"Get-Process","ok":true})");
        const std::string report = jack.report_text("test");
        CHECK(report.find("HK-DESKTOP (Windows 11), user hk") != std::string::npos);
        CHECK(report.find("RAM 91%") != std::string::npos && report.find("> Get-Process") != std::string::npos);
        CHECK(report.find("trusted PC") != std::string::npos);
        jack.handle_char("x");
    }
    // The list is private and the session left one report and the scan for next time.
    const auto list = store::load_trusted(dir.string());
    CHECK(list.size() == 1 && list[0].host == "HK-DESKTOP" && list[0].token == token);
    CHECK((std::filesystem::status(dir / "jack-trusted.conf").permissions() & std::filesystem::perms::others_all) ==
          std::filesystem::perms::none);
    CHECK((std::filesystem::status(dir / "jack-trusted.conf").permissions() & std::filesystem::perms::group_all) ==
          std::filesystem::perms::none);
    size_t reports = 0;
    for (const auto& e : std::filesystem::directory_iterator(dir / "jack-reports")) {
        if (e.path().extension() == ".txt") ++reports;
    }
    CHECK(reports == 1);
    const auto last = store::load_last(dir.string(), "HK-DESKTOP");
    CHECK(last.have && last.ram == 91 && last.defender == "off");
    CHECK(store::last_text(last, "C:").find("RAM 91%") != std::string::npos);
    {
        // The next link with the token: chips at once, Terminal still asks, last visit noted.
        JackInChip jack(config);
        jack.capture_sent = true;
        std::string with = hello;
        with.insert(with.size() - 1, ",\"trust\":\"" + token + "\"");
        jack.on_line(with);
        CHECK(jack.trusted() && jack.pc_control() && !jack.pc_terminal());
        std::string id;
        jack.on_line(R"({"t":"open","chip":"terminal"})");
        CHECK(!jack.take_open(id) && jack.pc_wants() == "terminal");
        jack.on_line(R"({"t":"scan","cpu":10,"ram":40,"disk":"C:","disk_free":50,"startup":3,"defender":"on"})");
        bool noted = false;
        for (const auto& line : jack.log()) noted = noted || line.text.find("last visit") != std::string::npos;
        CHECK(noted);
        // The admin handover is the same visit: no report for it.
        jack.on_line(R"({"t":"bye","why":"admin handover"})");
        // A wrong token gets nothing and is told to forget it.
        std::string wrong = hello;
        wrong.insert(wrong.size() - 1, ",\"trust\":\"" + std::string(64, 'a') + "\"");
        jack.sent.clear();
        jack.on_line(wrong);
        CHECK(!jack.trusted() && !jack.pc_control());
        CHECK(std::find(jack.sent.begin(), jack.sent.end(), R"({"t":"trust","token":""})") != jack.sent.end());
        // [T] twice on a trusted PC forgets it.
        jack.on_line(with);
        CHECK(jack.trusted());
        jack.handle_char("t");
        jack.handle_char("t");
        CHECK(!jack.trusted() && store::load_trusted(dir.string()).empty());
        jack.on_line(with);
        CHECK(!jack.trusted());
    }
    reports = 0;
    for (const auto& e : std::filesystem::directory_iterator(dir / "jack-reports")) {
        if (e.path().extension() == ".txt") ++reports;
    }
    CHECK(reports == 1);
    // Host names from a PC never leave the reports folder.
    CHECK(store::safe_host("../../etc/passwd").find('/') == std::string::npos);
    CHECK(store::safe_host("..").find('.') == std::string::npos);
    std::filesystem::remove_all(dir);
}

// A ROM download goes straight to its file, tells the server's time, and leaves
// nothing behind when the reply is an error or larger than allowed.
void test_http_download_streams_to_disk() {
    if (std::system("command -v curl >/dev/null 2>&1") != 0) return;  // the download path is curl
    for (const char* name : {"no_proxy", "NO_PROXY"}) {
        const char* old = std::getenv(name);
        setenv(name, (std::string(old ? old : "") + (old && *old ? "," : "") + "127.0.0.1").c_str(), 1);
    }
    const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    int one = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    CHECK(::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0 && ::listen(listener, 4) == 0);
    socklen_t len = sizeof addr;
    getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &len);
    const int port = ntohs(addr.sin_port);
    const std::string rom(300000, 'R');
    // Three requests: the ROM, a 404, and the ROM again for a caller that allows less.
    std::thread server([listener, rom]() {
        for (int i = 0; i < 3; ++i) {
            const int fd = ::accept(listener, nullptr, nullptr);
            if (fd < 0) return;
            char buf[2048];
            std::string request;
            while (request.find("\r\n\r\n") == std::string::npos) {
                const ssize_t n = ::recv(fd, buf, sizeof buf, 0);
                if (n <= 0) break;
                request.append(buf, static_cast<size_t>(n));
            }
            const bool missing = i == 1;
            const std::string body = missing ? "no such rom" : rom;
            const std::string head = std::string(missing ? "HTTP/1.1 404 Not Found" : "HTTP/1.1 200 OK") +
                                     "\r\nDate: Sun, 22 Nov 2026 02:19:22 GMT\r\nContent-Length: " +
                                     std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
            const std::string reply = head + body;
            size_t sent = 0;
            while (sent < reply.size()) {
                const ssize_t n = ::send(fd, reply.data() + sent, reply.size() - sent, MSG_NOSIGNAL);
                if (n <= 0) break;
                sent += static_cast<size_t>(n);
            }
            ::close(fd);
        }
    });
    const std::string url = "http://127.0.0.1:" + std::to_string(port) + "/api/roms/1/content/Game%20[!].gb";
    const std::string path = (std::filesystem::temp_directory_path() / "pet-os-download-test.part").string();
    auto r = platform::net::http_download(url, {"Authorization: Bearer x"}, path, 10000, 1024 * 1024);
    CHECK(r.ok && r.status == 200 && r.body.empty());
    CHECK(r.date == 1795313962);
    std::error_code ec;
    CHECK(std::filesystem::file_size(path, ec) == rom.size());
    r = platform::net::http_download(url, {}, path, 10000, 1024 * 1024);
    CHECK(r.status == 404 && !std::filesystem::exists(path));
    r = platform::net::http_download(url, {}, path, 10000, 1000);
    CHECK(!r.ok && r.error == "reply too large" && !std::filesystem::exists(path));
    ::close(listener);
    server.join();
}

// The PET's clock is only set by a source Holden trusts, unless it is certainly wrong.
void test_clock_sync_rules() {
    using namespace platform::clock_sync;
    const long long good = kSaneEpoch + 5000000;
    CHECK(should_set(1700000000, good, false));        // a clock from before the PET existed: anyone may fix it
    CHECK(!should_set(good, good + 86400, false));     // a sane clock: not from a PC nobody trusted
    CHECK(should_set(good, good + 86400, true));
    CHECK(!should_set(good, good + 30, true));         // close enough
    CHECK(!should_set(good, 1700000000, true));        // never back to a time that can't be right
    CHECK(!should_set(good, 0, true));
    CHECK(parse_http_date("Sun, 22 Nov 2026 02:19:22 GMT") == 1795313962);
    CHECK(parse_http_date("garbage") == 0 && parse_http_date("") == 0);
}

// [navi]: the Navi's name, the owner and the picture come from the config, not the code.
void test_navi_identity() {
    using viewmodel::chips::JackInChip;
    const model::PetConfig defaults = model::parse_pet_config("");
    CHECK(defaults.navi_name == "Alice" && defaults.navi_owner.empty() && defaults.navi_picture.empty());
    CHECK(defaults.jack_operator == "Operator");

    const model::PetConfig mine =
        model::parse_pet_config("[navi]\nname=Rex\nowner=Sam Lee\npicture=~/rex.png\n[jackin]\nlisten=10.55.0.1\n");
    CHECK(mine.navi_name == "Rex" && mine.navi_owner == "Sam Lee" && mine.navi_picture == "~/rex.png");
    CHECK(mine.jack_operator == "Sam Lee");  // the owner runs Jack In unless operator= says otherwise
    CHECK(model::parse_pet_config("[navi]\nowner=Sam\n[jackin]\noperator=Front desk\n").jack_operator == "Front desk");
    CHECK(model::parse_pet_config("[navi]\nname=\n").navi_name == "Alice");

    model::set_navi({"  Rex the Great Navi!  ", "Sam\x01 Lee", ""});
    CHECK(model::navi_name() == "Rex the Grea");  // 12 characters
    CHECK(model::navi_exe() == "REX_THE_GREA.EXE");
    CHECK(model::owner_or("x") == "Sam Lee");
    model::set_navi({"", "", ""});
    CHECK(model::navi_name() == "Alice" && model::owner_or("the user") == "the user");

    model::set_navi({"Rex", "Sam", ""});
    JackInChip::Scan scan;
    const std::string fix = JackInChip::alice_prompt("PC", "Windows 11", scan, "");
    CHECK(fix.find("Sam plugged the PET") != std::string::npos && fix.find("Holden") == std::string::npos);
    CHECK(viewmodel::chips::build_ask_prompt("'pi' (pi@127.0.0.1)", "x").find("Ask Rex.") != std::string::npos);
    CHECK(JackInChip::output_prompt("ask", "Stop-Service x", false, "declined", "").find("Sam declined") != std::string::npos);
    // The longest owner still leaves the chat header room for a 380-character message.
    model::set_navi({"Rex", "Abcdefghijklmnop", ""});
    const std::string asked(380, 'q');
    const std::string chat = JackInChip::chat_prompt("LAPTOP-FRONT", "full", asked);
    CHECK(chat.size() <= viewmodel::chips::kBridgeMaxInput && chat.find(asked) != std::string::npos);
    // Jack In sends the [navi] picture to the PC as base64 ("PNG" -> "UE5H").
    {
        const auto png = std::filesystem::temp_directory_path() / "pet-os-test-navi.png";
        { std::ofstream(png, std::ios::binary) << "PNG"; }
        model::set_navi({"Norm", "Sam", png.string()});
        model::PetConfig config;
        config.jack_gadget_up.clear();
        config.jack_gadget_down.clear();
        config.bridge_url.clear();
        viewmodel::chips::JackInChip jack(config);
        jack.capture_sent = true;
        jack.on_line(R"({"t":"hello","host":"LAPTOP-FRONT","os":"Win 11","user":"front-desk"})");
        bool found = false;
        for (const auto& line : jack.sent) found = found || line == R"({"t":"navi","name":"Norm","art":"UE5H"})";
        CHECK(found);
        // A missing picture sends no art.
        model::set_navi({"Norm", "Sam", "/nonexistent/norm.png"});
        jack.sent.clear();
        jack.on_line(R"({"t":"hello","host":"LAPTOP-FRONT","os":"Win 11","user":"front-desk"})");
        found = false;
        for (const auto& line : jack.sent) found = found || line == R"({"t":"navi","name":"Norm","art":""})";
        CHECK(found);
        std::filesystem::remove(png);
    }
    model::set_navi({});
}

int main() {
    // Nothing here may write to the real home folder (Jack In reports, trusted PCs).
    const std::filesystem::path home = std::filesystem::temp_directory_path() / "pet-os-tests-home";
    std::filesystem::create_directories(home);
    setenv("HOME", home.c_str(), 1);
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"wrap", test_wrap},
        {"pc_input_reaches_open_chip", test_pc_input_reaches_open_chip},
        {"jack_pc_control_is_granted_on_the_pet", test_jack_pc_control_is_granted_on_the_pet},
        {"jack_link_send_never_blocks", test_jack_link_send_never_blocks},
        {"decode_chunked_bounds", test_decode_chunked_bounds},
        {"retro_unknown_saves_are_not_overwritten", test_retro_unknown_saves_are_not_overwritten},
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
        {"power_profiles", test_power_profiles},
        {"power_manager_sysfs", test_power_manager_sysfs},
        {"config_local_overlay", test_config_local_overlay},
        {"ir_codec", test_ir_codec},
        {"remote_chip_files_and_pad", test_remote_chip_files_and_pad},
        {"retro_parsing", test_retro_parsing},
        {"retro_scrolling_systems_and_names", test_retro_scrolling_systems_and_names},
        {"retro_list_layout", test_retro_list_layout},
        {"jellyfin_list_navigation", test_jellyfin_list_navigation},
        {"jellyfin_pc_direct_play", test_jellyfin_pc_direct_play},
        {"jellyfin_item_ids_are_encoded", test_jellyfin_item_ids_are_encoded},
        {"retro_pagination_and_cache", test_retro_pagination_and_cache},
        {"retro_needs_server", test_retro_needs_server},
        {"retro_save_sync_helpers", test_retro_save_sync_helpers},
        {"retro_failed_pull_keeps_both_saves", test_retro_failed_pull_keeps_both_saves},
        {"retro_safe_names", test_retro_safe_names},
        {"run_capture_cap", test_run_capture_cap},
        {"lan_who_is_new", test_lan_who_is_new},
        {"lan_helpers", test_lan_helpers},
        {"lan_static_config", test_lan_static_config},
        {"jack_session", test_jack_session},
        {"jack_trust_and_report", test_jack_trust_and_report},
        {"navi_identity", test_navi_identity},
        {"clock_sync_rules", test_clock_sync_rules},
        {"http_download_streams_to_disk", test_http_download_streams_to_disk},
        {"jack_line_server", test_jack_line_server},
        {"jack_relay", test_jack_relay},
        {"http_status_line", test_http_status_line},
    };
    for (const auto& [name, fn] : tests) {
        const int before = g_failures;
        fn();
        std::printf("%s %s\n", g_failures == before ? "PASS" : "FAIL", name);
    }
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
