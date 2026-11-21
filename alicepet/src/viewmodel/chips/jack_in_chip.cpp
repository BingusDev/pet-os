/*
 * SPDX-License-Identifier: MIT
 */

#include "viewmodel/chips/jack_in_chip.h"

#include "platform/json.h"
#include "platform/net.h"
#include "platform/process.h"
#include "viewmodel/alice_viewmodel.h"
#include "viewmodel/chips/terminal_chip.h"
#include "model/chip_registry.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>

namespace viewmodel::chips {
namespace {

using platform::net::json_escape;

// usb-gadget.sh takes one up/down at a time and waits up to 20 s for the other
// to finish, so give it longer than that before giving up on it.
constexpr int kGadgetTimeoutMs = 40000;

std::string trim(const std::string& value) {
    const auto a = value.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    return value.substr(a, value.find_last_not_of(" \t\r\n") - a + 1);
}

std::string first_line(const std::string& text) {
    const std::string t = trim(text);
    return t.substr(0, t.find('\n'));
}

int as_int(const platform::json::Value& v) {
    return v.is_null() ? -1 : static_cast<int>(v.num(-1) + 0.5);
}

// The chips the PC app can open. Voice, Remote and Jack In stay on the PET.
struct PcChip {
    const char* id;
    const char* code;  // red letter on the PC folder row
    const char* desc;
};
constexpr PcChip kPcChips[] = {
    {"terminal", "T", "Shell to any host"},
    {"jellyfin", "J", "Your media library"},
    {"retro", "R", "Retro games on the PET"},
    {"lan-tech", "L", "Check the LAN"},
};

const PcChip* find_pc_chip(const std::string& id) {
    for (const auto& c : kPcChips) {
        if (id == c.id) return &c;
    }
    return nullptr;
}

std::string base64_encode(const std::string& in) {
    static const char* kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const uint32_t v = (static_cast<uint8_t>(in[i]) << 16) | (static_cast<uint8_t>(in[i + 1]) << 8) |
                           static_cast<uint8_t>(in[i + 2]);
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += kAlphabet[(v >> 6) & 63];
        out += kAlphabet[v & 63];
    }
    if (i + 1 == in.size()) {
        const uint32_t v = static_cast<uint8_t>(in[i]) << 16;
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += "==";
    } else if (i + 2 == in.size()) {
        const uint32_t v = (static_cast<uint8_t>(in[i]) << 16) | (static_cast<uint8_t>(in[i + 1]) << 8);
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += kAlphabet[(v >> 6) & 63];
        out += '=';
    }
    return out;
}

// Run-length RGB565: (count 1..255, pixel big-endian), the fastest thing to ship over the cable.
std::string rle_rgb565(const std::vector<uint16_t>& px, size_t from, size_t count) {
    std::string out;
    size_t i = 0;
    while (i < count) {
        const uint16_t v = px[from + i];
        size_t run = 1;
        while (i + run < count && run < 255 && px[from + i + run] == v) ++run;
        out += static_cast<char>(run);
        out += static_cast<char>(v >> 8);
        out += static_cast<char>(v & 0xff);
        i += run;
    }
    return out;
}

std::string read_file_base64(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return "";
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return base64_encode(bytes);
}

bool key_from_name(const std::string& name, Key& key) {
    static const std::pair<const char*, Key> kKeys[] = {
        {"Left", Key::Left},   {"Right", Key::Right},         {"Up", Key::Up},           {"Down", Key::Down},
        {"Enter", Key::Enter}, {"Escape", Key::Escape},       {"Backspace", Key::Backspace}, {"Tab", Key::Tab},
    };
    for (const auto& k : kKeys) {
        if (name == k.first) {
            key = k.second;
            return true;
        }
    }
    return false;
}

} // namespace

bool JackInChip::pc_capable(const std::string& chip_id) {
    return find_pc_chip(chip_id) != nullptr;
}

JackInChip::JackInChip(model::PetConfig config)
    : config_(std::move(config)), opens_(std::make_shared<Opens>()) {}

JackInChip::~JackInChip() {
    relay_.stop();
    if (sharing_) platform::jack::set_proxy_env("");
}

// --- lifecycle -----------------------------------------------------------------

void JackInChip::enter() {
    view_ = connected_ ? View::Session : View::Waiting;
    status_.clear();
    if (config_.jack_gadget_up.empty()) {
        usb_ok_ = true;
        usb_status_ = "USB not managed";
    } else if (!usb_busy_) {
        usb_busy_ = true;
        usb_ok_ = false;
        usb_status_ = "switching USB to device mode...";
        gadget_.run([cmd = config_.jack_gadget_up]() {
            GadgetResult r;
            r.up = true;
            r.code = platform::proc::run_capture(platform::proc::split_command(cmd), r.text, kGadgetTimeoutMs);
            return r;
        });
    }
    listen_retry_ms_ = 0;
    try_listen();
    touch();
}

void JackInChip::leave() {
    // The PC asked for another chip: the link and the USB port stay up behind it.
    if (connected_ && switching_) return;
    // Leaving the chip ends the session and gives the USB port back.
    if (connected_) jack_out("PET left JACK IN");
    server_.close();
    if (!config_.jack_gadget_down.empty()) {
        gadget_.run([cmd = config_.jack_gadget_down]() {
            GadgetResult r;
            r.code = platform::proc::run_capture(platform::proc::split_command(cmd), r.text, kGadgetTimeoutMs);
            return r;
        });
    }
    usb_ok_ = false;
    usb_status_.clear();
}

void JackInChip::try_listen() {
    if (server_.listening()) return;
    listen_error_ = server_.listen(config_.jack_listen, config_.jack_port);
}

// --- PC -> PET -------------------------------------------------------------------

void JackInChip::on_line(const std::string& line) {
    platform::json::Value msg;
    if (!platform::json::parse(line, msg) || !msg.is_object()) return;
    const std::string t = msg["t"].str();
    if (t == "hello") {
        reset_session();
        connected_ = true;
        host_ = msg["host"].str("PC");
        os_ = msg["os"].str();
        user_ = msg["user"].str();
        admin_ = msg["admin"].boolean();
        full_ = msg["full"].boolean();
        input_ok_ = msg["input"].boolean(false);
        view_ = View::Session;
        note("linked to " + host_ + (admin_ ? " (admin)" : "") + (full_ ? ", full access" : ", read-only"));
        send_chips();
    } else if (t == "scan") {
        scan_ = Scan{};
        scan_.have = true;
        scan_.cpu = as_int(msg["cpu"]);
        scan_.ram = as_int(msg["ram"]);
        scan_.disk = msg["disk"].str("C:");
        scan_.disk_free = as_int(msg["disk_free"]);
        scan_.startup = as_int(msg["startup"]);
        scan_.defender = msg["defender"].str("?");
        scan_.defender_since = msg["defender_since"].str();
        for (const auto& hog : msg["top"].items()) {
            scan_.top.push_back({hog["name"].str("?"), as_int(hog["mb"])});
        }
        if (!alice_asked_) ask_alice();
    } else if (t == "state") {
        const bool was_full = full_;
        const bool was_admin = admin_;
        full_ = msg["full"].boolean(full_);
        admin_ = msg["admin"].boolean(admin_);
        const bool net = msg["net"].boolean(sharing_);
        if ((full_ && asked_full_) || (admin_ && asked_admin_) || (net && asked_net_)) status_.clear();
        if (full_) asked_full_ = false;
        if (admin_) asked_admin_ = false;
        if (net) asked_net_ = false;
        if (full_ != was_full) note(full_ ? "PC turned Full Access on" : "PC turned Full Access off");
        if (admin_ != was_admin) note(admin_ ? "running as admin (UAC accepted)" : "admin dropped");
        if (msg["denied"].str() == "admin") {
            asked_admin_ = false;
            note("admin prompt was cancelled on the PC", LogLine::Kind::Fail);
        }
        if (net != sharing_) set_sharing(net);
        input_ok_ = msg["input"].boolean(input_ok_);
    } else if (t == "open") {
        const std::string id = msg["chip"].str();
        if (pc_capable(id)) {
            opening_.push_back(id);
        } else {
            note("PC asked for a chip that stays on the PET", LogLine::Kind::Fail);
        }
    } else if (t == "mirror") {
        const bool on = msg["on"].boolean(false) && connected_;
        if (on && !mirror_on_) last_px_.clear();  // the next frame is a full one
        mirror_on_ = on;
    } else if (t == "key") {
        Key key{Key::Enter};
        if (input_ok_ && key_from_name(msg["k"].str(), key) && inputs_.size() < 256) {
            inputs_.push_back({false, key, ""});
        }
    } else if (t == "char") {
        const std::string c = msg["c"].str();
        if (input_ok_ && !c.empty() && c.size() <= 4 && inputs_.size() < 256) {
            inputs_.push_back({true, Key::Enter, c});
        }
    } else if (t == "log") {
        // A command the PC app ran itself (the read-only scan, or one clicked there).
        const std::string text = first_line(msg["text"].str());
        if (!text.empty()) note("> " + text, msg["ok"].boolean(true) ? LogLine::Kind::Ok : LogLine::Kind::Fail);
    } else if (t == "result") {
        const int id = static_cast<int>(msg["id"].integer());
        const bool ok = msg["ok"].boolean();
        for (auto& l : log_) {
            if (l.id == id && l.kind == LogLine::Kind::Pending) l.kind = ok ? LogLine::Kind::Ok : LogLine::Kind::Fail;
        }
        const std::string out = AliceViewModel::screen_text(first_line(msg["out"].str()));
        if (!out.empty()) note("  " + out, ok ? LogLine::Kind::Note : LogLine::Kind::Fail);
        if (!ok && msg["reason"].str() == "read-only") status_ = "PC is read-only: [F] asks for Full Access";
    } else if (t == "chat" || t == "voice") {
        // Talking to Alice from the PC app: the PET is the one with the bridge.
        if (chat_waiting_) {
            send(R"({"t":"alice","ok":false,"text":"Still answering the last one...","heard":"","cmd":""})");
        } else if (t == "chat") {
            const std::string text = trim(msg["text"].str());
            if (!text.empty()) {
                note("PC: " + AliceViewModel::screen_text(text));
                chat_waiting_ = true;
                alice_waiting_ = true;
                const std::string prompt = clip_for_bridge(
                    "(Holden is typing on " + host_ + " through PET JACK IN; answer briefly, plain text. If a "
                    "PowerShell fix helps, put it alone on a last line 'CMD: <command>'.) " + text);
                chat_.run([config = config_, prompt]() { return bridge_ask(config, prompt); });
            }
        } else {
            std::string wav;
            if (platform::jack::base64_decode(msg["wav"].str(), wav) && wav.size() > 44) {
                note("PC: (voice, " + std::to_string(wav.size() / 32000) + " s)");
                chat_waiting_ = true;
                alice_waiting_ = true;
                chat_.run([config = config_, wav = std::move(wav)]() { return bridge_voice(config, wav); });
            } else {
                send(R"({"t":"alice","ok":false,"text":"That recording didn't come through. Try again?","heard":"","cmd":""})");
            }
        }
    } else if (t == "net_fail") {
        relay_.fail(static_cast<int>(msg["id"].integer()));
    } else if (t == "bye") {
        note("PC app closed (" + msg["why"].str("jack out") + ")");
        server_.drop_client();
        reset_session();
        view_ = View::Waiting;
    }
    touch();
}

void JackInChip::reset_session() {
    connected_ = false;
    full_ = admin_ = false;
    asked_full_ = asked_admin_ = asked_net_ = false;
    if (sharing_) set_sharing(false);
    scan_ = Scan{};
    alice_asked_ = false;
    alice_waiting_ = false;
    chat_waiting_ = false;
    input_ok_ = false;
    mirror_on_ = false;
    opening_.clear();
    inputs_.clear();
    last_px_.clear();
    last_screen_.clear();
    ++alice_generation_;
    alice_text_.clear();
    suggestion_.clear();
    host_.clear();
    os_.clear();
    user_.clear();
}

void JackInChip::set_sharing(bool on) {
    if (on) {
        auto opens = opens_;
        const std::string error = relay_.start(
            "127.0.0.1", config_.jack_proxy_port, config_.jack_listen, config_.jack_tunnel_port,
            [opens](int id, const std::string& host, int port) {
                std::lock_guard<std::mutex> lock(opens->mutex);
                opens->lines.push_back("{\"t\":\"net_open\",\"id\":" + std::to_string(id) + ",\"host\":\"" +
                                       json_escape(host) + "\",\"port\":" + std::to_string(port) + "}");
            });
        if (!error.empty()) {
            note("can't share PC internet: " + error, LogLine::Kind::Fail);
            send("{\"t\":\"drop\",\"what\":\"net\"}");
            sharing_ = false;
            return;
        }
        platform::jack::set_proxy_env("http://127.0.0.1:" + std::to_string(config_.jack_proxy_port));
        sharing_ = true;
        note("using the PC's internet");
    } else {
        relay_.stop();
        platform::jack::set_proxy_env("");
        if (sharing_) note("stopped using the PC's internet");
        sharing_ = false;
    }
}

// --- PET -> PC -------------------------------------------------------------------

void JackInChip::send(const std::string& json) {
    if (capture_sent) sent.push_back(json);
    server_.send(json);
}

void JackInChip::note(const std::string& text, LogLine::Kind kind) {
    log_.push_back({kind, text, 0});
    while (log_.size() > kMaxLog) log_.pop_front();
    if (view_ != View::Log) log_scroll_ = 0;
}

void JackInChip::ask(const char* what) {
    send(std::string("{\"t\":\"ask\",\"what\":\"") + what + "\"}");
    status_ = std::string("asked the PC: click ") +
              (std::string(what) == "full" ? "Full Access" : std::string(what) == "admin" ? "Request Admin" : "Share PC Internet") +
              " there";
}

void JackInChip::drop(const char* what) {
    send(std::string("{\"t\":\"drop\",\"what\":\"") + what + "\"}");
}

void JackInChip::ask_alice() {
    alice_asked_ = true;
    alice_waiting_ = true;
    alice_text_.clear();
    suggestion_.clear();
    alice_.run([config = config_, prompt = alice_prompt(host_, os_, scan_), gen = ++alice_generation_]() {
        return std::make_pair(gen, bridge_ask(config, prompt));
    });
}

void JackInChip::on_alice_reply(const BridgeReply& reply) {
    ++alice_generation_;
    alice_waiting_ = false;
    if (!reply.ok) {
        alice_text_ = "(can't reach Alice: " + reply.error + ")";
        suggestion_.clear();
    } else {
        split_reply(reply.reply, alice_text_, suggestion_);
    }
    touch();
}

void JackInChip::on_chat_reply(const BridgeReply& reply) {
    chat_waiting_ = false;
    alice_waiting_ = false;
    ++alice_generation_;  // a scan diagnosis still in flight would overwrite this answer
    std::string words;
    std::string command;
    if (reply.ok) split_reply(reply.reply, words, command);
    if (!reply.transcript.empty()) note("PC (heard): " + AliceViewModel::screen_text(reply.transcript));
    if (reply.ok) {
        alice_text_ = words;
        if (!command.empty()) suggestion_ = command;
        note("Alice: " + words);
    } else {
        note("Alice: (can't reach her: " + reply.error + ")", LogLine::Kind::Fail);
    }
    send("{\"t\":\"alice\",\"ok\":" + std::string(reply.ok ? "true" : "false") + ",\"text\":\"" +
         json_escape(reply.ok ? words : "Can't reach Alice: " + reply.error) + "\",\"heard\":\"" +
         json_escape(reply.transcript) + "\",\"cmd\":\"" + json_escape(command) + "\"}");
    touch();
}

void JackInChip::run_suggestion() {
    if (suggestion_.empty() || !connected_) return;
    if (!full_) {
        status_ = "PC is read-only: [F] asks for Full Access";
        touch();
        return;
    }
    const int id = next_id_++;
    send("{\"t\":\"run\",\"id\":" + std::to_string(id) + ",\"cmd\":\"" + json_escape(suggestion_) + "\"}");
    log_.push_back({LogLine::Kind::Pending, "> " + suggestion_, id});
    while (log_.size() > kMaxLog) log_.pop_front();
    suggestion_.clear();
    status_.clear();
    touch();
}

void JackInChip::jack_out(const std::string& why) {
    send("{\"t\":\"jackout\",\"why\":\"" + json_escape(why) + "\"}");
    server_.drop_client();
    reset_session();
    note("jacked out");
    view_ = View::Waiting;
    status_ = "Jacked out. The PC app closed and left nothing behind.";
    touch();
}

// --- keys ------------------------------------------------------------------------

bool JackInChip::handle_key(Key key) {
    if (view_ == View::Log) {
        if (key == Key::Escape) {
            view_ = connected_ ? View::Session : View::Waiting;
            log_scroll_ = 0;
        } else if (key == Key::Up && log_scroll_ + 1 < log_.size()) {
            ++log_scroll_;
        } else if (key == Key::Down && log_scroll_ > 0) {
            --log_scroll_;
        }
        touch();
        return true;
    }
    if (key == Key::Escape) {
        if (!status_.empty()) {
            status_.clear();
            touch();
            return true;
        }
        return false;  // leave() jacks out
    }
    if (key == Key::Enter) run_suggestion();
    return true;
}

void JackInChip::handle_char(const std::string& utf8) {
    const char c = utf8.size() == 1 ? static_cast<char>(std::tolower(static_cast<unsigned char>(utf8[0]))) : 0;
    if (c == 'l') {
        view_ = view_ == View::Log ? (connected_ ? View::Session : View::Waiting) : View::Log;
        log_scroll_ = 0;
        touch();
        return;
    }
    if (!connected_) return;
    if (c == 'f') {
        if (full_) {
            drop("full");  // taking access away needs no click on the PC
            status_ = "asked the PC to go back to read-only";
        } else {
            asked_full_ = true;
            ask("full");
        }
    } else if (c == 'a') {
        if (admin_) return;
        asked_admin_ = true;
        ask("admin");
    } else if (c == 'n') {
        if (sharing_) {
            drop("net");
            set_sharing(false);
        } else {
            asked_net_ = true;
            ask("net");
        }
    } else if (c == 'r') {
        send("{\"t\":\"scan\"}");
        alice_asked_ = false;
        status_ = "rescanning the PC...";
    } else if (c == '?' && !alice_waiting_ && scan_.have) {
        ask_alice();
    } else if (c == 'x') {
        jack_out("JACK OUT on the PET");
        return;
    } else {
        return;
    }
    touch();
}

// --- tick ------------------------------------------------------------------------

void JackInChip::tick(uint32_t elapsed_ms) {
    GadgetResult g;
    while (gadget_.poll(g)) {
        if (!g.up) continue;
        usb_busy_ = false;
        usb_ok_ = g.code == 0;
        usb_status_ = usb_ok_ ? "USB: drive + network ready" : "USB: " + first_line(g.text.empty() ? "gadget setup failed" : g.text);
        listen_retry_ms_ = 0;
        try_listen();
        touch();
    }
    if (!server_.listening()) {
        listen_retry_ms_ += elapsed_ms;
        if (listen_retry_ms_ >= 1000) {
            listen_retry_ms_ = 0;
            try_listen();
            if (server_.listening()) touch();
        }
    }
    check_cable(elapsed_ms);
    const auto polled = server_.poll();
    if (polled.connected && !connected_) touch();
    for (const auto& line : polled.lines) on_line(line);
    if (polled.disconnected && connected_) {
        note("the PC went away (unplugged or app closed)", LogLine::Kind::Fail);
        reset_session();
        view_ = view_ == View::Log ? View::Log : View::Waiting;
        touch();
    }
    std::vector<std::string> opens;
    {
        std::lock_guard<std::mutex> lock(opens_->mutex);
        opens.swap(opens_->lines);
    }
    for (const auto& line : opens) send(line);
    std::pair<size_t, BridgeReply> reply;
    while (alice_.poll(reply)) {
        if (reply.first == alice_generation_) on_alice_reply(reply.second);
    }
    BridgeReply chat;
    while (chat_.poll(chat)) {
        if (connected_) on_chat_reply(chat);
    }
}

// The CardputerZero only reaches a PC through USB-C2 with the USB-SW slider on
// C2. If the gadget is up but the controller has seen no host for a few
// seconds, say so on the PET: the PC can't show anything until this is right.
void JackInChip::check_cable(uint32_t elapsed_ms) {
    if (!usb_ok_ || connected_) {
        unattached_ms_ = 0;
        if (usb_no_pc_) { usb_no_pc_ = false; usb_status_ = "USB: drive + network ready"; touch(); }
        return;
    }
    udc_check_ms_ += elapsed_ms;
    if (udc_check_ms_ < 500) return;
    const uint32_t step = udc_check_ms_;
    udc_check_ms_ = 0;
    std::string state;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator("/sys/class/udc", ec)) {
        std::ifstream f(e.path() / "state");
        std::getline(f, state);
        break;
    }
    if (state.empty()) return;  // no controller info: don't guess
    if (state == "not attached") {
        unattached_ms_ += step;
        if (unattached_ms_ >= 5000 && !usb_no_pc_) {
            usb_no_pc_ = true;
            usb_status_ = "No PC: cable in C2 (top), USB-SW to C2";
            touch();
        }
    } else if (usb_no_pc_) {
        unattached_ms_ = 0;
        usb_no_pc_ = false;
        usb_status_ = "USB: PC found, start JackIn on it";
        touch();
    } else {
        unattached_ms_ = 0;
    }
}

// --- text helpers ----------------------------------------------------------------

std::string JackInChip::scan_summary(const Scan& scan) {
    if (!scan.have) return "scanning...";
    std::string out;
    const auto part = [&](const std::string& p) { out += (out.empty() ? "" : " · ") + p; };
    if (scan.cpu >= 0) part("CPU " + std::to_string(scan.cpu) + "%");
    if (scan.ram >= 0) part("RAM " + std::to_string(scan.ram) + "%");
    if (scan.disk_free >= 0) part(scan.disk + " " + std::to_string(scan.disk_free) + "% free");
    return out;
}

std::string JackInChip::alice_prompt(const std::string& host, const std::string& os, const Scan& scan) {
    std::string p =
        "PET JACK IN. Holden (store IT) plugged the PET into a PC; read-only scan below. Reply for a tiny screen: "
        "1-2 short sentences on the worst problem, then optionally one line 'CMD: <one PowerShell command>' that "
        "fixes it (Holden decides; it needs Full Access). No markdown. PC: " +
        host + " (" + os + "). " + scan_summary(scan);
    if (scan.startup >= 0) p += " · startup items " + std::to_string(scan.startup);
    p += " · Defender " + scan.defender + (scan.defender_since.empty() ? "" : " since " + scan.defender_since);
    if (!scan.top.empty()) {
        p += " · top RAM:";
        for (size_t i = 0; i < scan.top.size() && i < 4; ++i) {
            p += " " + scan.top[i].name + " " + std::to_string(scan.top[i].mb) + "MB";
        }
    }
    return clip_for_bridge(p);
}

void JackInChip::split_reply(const std::string& reply, std::string& words, std::string& command) {
    words.clear();
    command.clear();
    std::istringstream lines(reply);
    std::string line;
    while (std::getline(lines, line)) {
        std::string t = trim(line);
        if (t.rfind("```", 0) == 0) continue;
        std::string lower = t;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) { return std::tolower(ch); });
        if (lower.rfind("cmd:", 0) == 0) {
            std::string cmd = trim(t.substr(4));
            if (cmd.size() >= 2 && cmd.front() == '`' && cmd.back() == '`') cmd = cmd.substr(1, cmd.size() - 2);
            command = sanitize_command(cmd);
        } else if (!t.empty()) {
            words += (words.empty() ? "" : " ") + t;
        }
    }
    words = AliceViewModel::screen_text(words);
}

// The chips the PC can open, with their label art, sent once per link.
void JackInChip::send_chips() {
    std::string list;
    for (size_t i = 0; i < model::chip_count(); ++i) {
        const model::ChipInfo& info = model::chip_at(i);
        const PcChip* pc = find_pc_chip(info.id);
        if (!pc) continue;
        if (!list.empty()) list += ',';
        list += "{\"id\":\"" + json_escape(info.id) + "\",\"name\":\"" + json_escape(info.name) +
                "\",\"number\":\"" + json_escape(info.number) + "\",\"code\":\"" + json_escape(pc->code) +
                "\",\"desc\":\"" + json_escape(pc->desc) + "\",\"art\":\"" +
                read_file_base64(art_dir_ / (std::string(info.id) + "-small.png")) + "\"}";
    }
    send("{\"t\":\"chips\",\"list\":[" + list + "]}");
}

bool JackInChip::take_open(std::string& chip_id) {
    if (opening_.empty()) return false;
    chip_id = opening_.front();
    opening_.pop_front();
    return true;
}

bool JackInChip::take_input(Input& input) {
    if (inputs_.empty()) return false;
    input = inputs_.front();
    inputs_.pop_front();
    return true;
}

// About 12 fps while the PC is mirroring; nothing is captured otherwise.
bool JackInChip::wants_frame(uint32_t now_ms) {
    if (!mirroring()) return false;
    if (last_frame_ms_ != 0 && now_ms - last_frame_ms_ < 80) return false;
    last_frame_ms_ = now_ms;
    return true;
}

// Sends only the rows that changed since the last frame, in bands of up to 16 rows.
void JackInChip::offer_frame(const uint8_t* argb, uint32_t stride, int w, int h, const std::string& screen) {
    if (!mirroring() || w <= 0 || h <= 0) return;
    std::vector<uint16_t> px(static_cast<size_t>(w) * h);
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = argb + static_cast<size_t>(y) * stride;
        for (int x = 0; x < w; ++x) {
            uint32_t v;
            std::memcpy(&v, row + static_cast<size_t>(x) * 4, 4);
            const uint16_t r = (v >> 19) & 0x1f;
            const uint16_t g = (v >> 10) & 0x3f;
            const uint16_t b = (v >> 3) & 0x1f;
            px[static_cast<size_t>(y) * w + x] = static_cast<uint16_t>((r << 11) | (g << 5) | b);
        }
    }
    // A new screen, size, or link starts from a full frame.
    const bool full = last_px_.size() != px.size() || last_screen_ != screen;
    last_screen_ = screen;
    int y = 0;
    while (y < h) {
        if (!full) {
            bool same = true;
            const size_t off = static_cast<size_t>(y) * w;
            same = std::equal(px.begin() + off, px.begin() + off + w, last_px_.begin() + off);
            if (same) {
                ++y;
                continue;
            }
        }
        int end = y;
        while (end < h && end - y < 16) {
            if (!full) {
                const size_t off = static_cast<size_t>(end) * w;
                if (std::equal(px.begin() + off, px.begin() + off + w, last_px_.begin() + off)) break;
            }
            ++end;
        }
        send_frame_band(screen, w, h, px, y, end);
        y = end;
    }
    last_px_ = std::move(px);
}

void JackInChip::send_frame_band(const std::string& screen, int w, int h, const std::vector<uint16_t>& px, int y0,
                                 int y1) {
    const size_t from = static_cast<size_t>(y0) * w;
    const std::string rle = rle_rgb565(px, from, static_cast<size_t>(y1 - y0) * w);
    send("{\"t\":\"frame\",\"screen\":\"" + json_escape(screen) + "\",\"w\":" + std::to_string(w) +
         ",\"h\":" + std::to_string(h) + ",\"y\":" + std::to_string(y0) + ",\"n\":" +
         std::to_string(y1 - y0) + ",\"rle\":\"" + base64_encode(rle) + "\"}");
}

} // namespace viewmodel::chips
