/*
 * SPDX-License-Identifier: MIT
 *
 * Unit tests for AlicePET's LVGL-free logic: the view model, text helpers, the
 * TERMINAL chip's Ask-Alice handling and the VT100 screen. No framework, so the
 * tests build anywhere the app builds: `ctest` or run alicepet_tests directly.
 */

#include "model/chip_registry.h"
#include "model/pet_config.h"
#include "model/power_profile.h"
#include "platform/json.h"
#include "platform/power.h"
#include "platform/process.h"
#include "viewmodel/alice_viewmodel.h"
#include "viewmodel/chips/bridge_client.h"
#include "viewmodel/chips/ir_codec.h"
#include "viewmodel/chips/lan_tech_chip.h"
#include "viewmodel/chips/jack_in_chip.h"
#include "platform/jack_link.h"
#include "viewmodel/chips/remote_chip.h"
#include "viewmodel/chips/retro_chip.h"
#include "viewmodel/chips/terminal_chip.h"
#include "viewmodel/chips/vt_screen.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <cstdio>
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
    const fs::path root = fs::temp_directory_path() / ("alicepet-power-" + std::to_string(::getpid()));
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
    const fs::path dir = fs::temp_directory_path() / ("alicepet-conf-" + std::to_string(::getpid()));
    fs::create_directories(dir);
    put(dir / "alicepet.conf", "[bridge]\nurl=\n[terminal]\nssh_config_import=\n[power]\nsleep_after_s=300\n");
    model::PetConfig config;
    CHECK(model::load_pet_config((dir / "alicepet.conf").string(), config));
    CHECK(config.bridge_url.empty());
    CHECK(config.bookmarks.empty());
    put(dir / "alicepet.local.conf", "[bridge]\nurl=http://mac:8797\n[terminal]\nhost=pi|me@10.0.0.2|home\n");
    CHECK(model::load_pet_config((dir / "alicepet.conf").string(), config));
    CHECK(config.bridge_url == "http://mac:8797");
    CHECK(config.bookmarks.size() == 1);
    CHECK(config.sleep_after_s == 300);  // untouched values still come from the main file
    CHECK(model::local_config_path("/etc/alicepet.conf") == "/etc/alicepet.local.conf");
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
    const fs::path dir = fs::temp_directory_path() / ("alicepet-remote-" + std::to_string(::getpid()));
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
    CHECK(report.rfind("AlicePET LAN TECH report · 2026-11-17 09:30\n", 0) == 0);
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
    JackInChip jack(config);
    jack.capture_sent = true;
    CHECK(jack.view() == JackInChip::View::Waiting);
    jack.on_line(R"({"t":"hello","host":"LAPTOP-FRONT","os":"Win 11","user":"front-desk"})");
    CHECK(jack.connected() && jack.view() == JackInChip::View::Session);
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
        {"power_profiles", test_power_profiles},
        {"power_manager_sysfs", test_power_manager_sysfs},
        {"config_local_overlay", test_config_local_overlay},
        {"ir_codec", test_ir_codec},
        {"remote_chip_files_and_pad", test_remote_chip_files_and_pad},
        {"retro_parsing", test_retro_parsing},
        {"retro_needs_server", test_retro_needs_server},
        {"retro_save_sync_helpers", test_retro_save_sync_helpers},
        {"lan_who_is_new", test_lan_who_is_new},
        {"lan_helpers", test_lan_helpers},
        {"lan_static_config", test_lan_static_config},
        {"jack_session", test_jack_session},
        {"jack_line_server", test_jack_line_server},
        {"jack_relay", test_jack_relay},
    };
    for (const auto& [name, fn] : tests) {
        const int before = g_failures;
        fn();
        std::printf("%s %s\n", g_failures == before ? "PASS" : "FAIL", name);
    }
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
