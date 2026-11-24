/*
 * SPDX-License-Identifier: MIT
 */

#include "viewmodel/chips/jack_in_chip.h"

#include "platform/clock_sync.h"
#include "platform/json.h"
#include "platform/net.h"
#include "platform/process.h"
#include "viewmodel/alice_viewmodel.h"
#include "viewmodel/chips/terminal_chip.h"
#include "model/chip_registry.h"
#include "logger/logger.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <ctime>
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

// "2026-11-23 14:05", local time.
std::string now_text() {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_r(&now, &tm);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &tm);
    return buf;
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
    {"retro", "R", "Retro games (play on this PC)"},
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

// The one Jack In chip, for pc_link (set by its constructor).
JackInChip* g_jack = nullptr;

namespace pc_link {
bool connected() { return g_jack && g_jack->connected(); }
const std::string& host() {
    static const std::string none;
    return g_jack ? g_jack->host() : none;
}
void send(const std::string& json) {
    if (connected()) g_jack->send_line(json);
}
std::string relay_file(const std::string& upstream_url, const std::vector<std::string>& headers,
                       const std::string& name) {
    return connected() ? g_jack->relay_file(upstream_url, headers, name) : "";
}
std::string relay_site(const std::string& origin, const std::vector<std::string>& headers, const std::string& path) {
    return connected() ? g_jack->relay_site(origin, headers, path) : "";
}
bool take_progress(Progress& progress) { return g_jack && g_jack->take_progress(progress); }
}  // namespace pc_link

std::string JackInChip::relay_file(const std::string& upstream_url, const std::vector<std::string>& headers,
                                   const std::string& name) {
    if (!media_.running()) {
        const std::string error = media_.start(config_.jack_listen, config_.jack_media_port);
        if (!error.empty()) {
            note("media relay: " + error, LogLine::Kind::Fail);
            return "";
        }
    }
    const std::string path = media_.add_file(upstream_url, headers, name);
    if (path.empty()) return "";
    return "http://" + config_.jack_listen + ":" + std::to_string(config_.jack_media_port) + path;
}

std::string JackInChip::relay_site(const std::string& origin, const std::vector<std::string>& headers,
                                   const std::string& path) {
    const int port = config_.jack_media_port + 1;
    if (!site_.running()) {
        const std::string error = site_.start(config_.jack_listen, port);
        if (!error.empty()) {
            note("web relay: " + error, LogLine::Kind::Fail);
            return "";
        }
    }
    const std::string gate = site_.set_site(origin, headers, path);
    if (gate.empty()) return "";
    return "http://" + config_.jack_listen + ":" + std::to_string(port) + gate;
}

bool JackInChip::take_progress(pc_link::Progress& progress) {
    if (progress_.empty()) return false;
    progress = std::move(progress_.front());
    progress_.pop_front();
    return true;
}

bool JackInChip::pc_capable(const std::string& chip_id) {
    return find_pc_chip(chip_id) != nullptr;
}

JackInChip::JackInChip(model::PetConfig config)
    : config_(std::move(config)), gadget_(std::make_shared<GadgetQueue>()), opens_(std::make_shared<Opens>()) {
    g_jack = this;
}

JackInChip::~JackInChip() {
    if (g_jack == this) g_jack = nullptr;
    media_.stop();
    site_.stop();
    relay_.stop();
    if (sharing_) platform::jack::set_proxy_env("");
}

// --- lifecycle -----------------------------------------------------------------

void JackInChip::enter() {
    // Back from a chip the PC opened: the link and USB port never went down.
    if (connected_) {
        view_ = View::Session;
        touch();
        return;
    }
    view_ = View::Waiting;
    status_.clear();
    if (config_.jack_gadget_up.empty()) {
        usb_ok_ = true;
        usb_status_ = "USB not managed";
    } else if (!usb_want_up_) {
        usb_ok_ = false;
        usb_status_ = "switching USB to device mode...";
        submit_gadget(true);
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
    submit_gadget(false);
    usb_ok_ = false;
    usb_status_.clear();
}

// Queues a gadget job behind any that is running. A re-enter while the last
// "down" is still waiting simply runs "up" after it, so the screen never says
// ready for a port that was just taken down.
void JackInChip::submit_gadget(bool up) {
    usb_want_up_ = up;
    GadgetQueue::Job job;
    job.up = up;
    job.cmd = up ? config_.jack_gadget_up : config_.jack_gadget_down;
    if (job.cmd.empty()) return;
    std::lock_guard<std::mutex> lock(gadget_->mutex);
    static size_t next_seq = 0;
    job.seq = ++next_seq;
    if (up) usb_up_seq_ = job.seq;
    gadget_->jobs.push_back(std::move(job));
    if (gadget_->running) return;
    gadget_->running = true;
    std::thread([queue = gadget_]() {
        for (;;) {
            GadgetQueue::Job next;
            {
                std::lock_guard<std::mutex> inner(queue->mutex);
                if (queue->jobs.empty()) {
                    queue->running = false;
                    return;
                }
                next = std::move(queue->jobs.front());
                queue->jobs.pop_front();
            }
            GadgetResult r;
            r.up = next.up;
            r.seq = next.seq;
            r.code = platform::proc::run_capture(platform::proc::split_command(next.cmd), r.text, kGadgetTimeoutMs);
            std::lock_guard<std::mutex> inner(queue->mutex);
            queue->done.push_back(std::move(r));
        }
    }).detach();
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
        mode_ = msg["mode"].str(full_ ? "full" : "read");
        view_ = View::Session;
        session_lines_ = 0;
        session_began_ = now_text();
        note("linked to " + host_ + (admin_ ? " (admin)" : "") + (full_ ? ", full access" : ", read-only"));
        // A token Holden gave this PC with [T]: chip control is on from the start.
        // (Terminal keeps its own [K].) A token the PET no longer knows is dropped there too.
        const std::string token = msg["trust"].str();
        const std::string dir = state_dir();
        if (!token.empty() && !dir.empty()) {
            trusted_ = jack_store::find_trusted(jack_store::load_trusted(dir), token) >= 0;
            if (trusted_) {
                pc_control_ = true;
                note("trusted PC: it may open chips and talk to Alice");
            } else {
                send(R"({"t":"trust","token":""})");
            }
        }
        // The PET has no battery clock: a trusted PC may set it, any PC may fix one that is certainly wrong.
        const long long pc_time = msg["time"].integer(0);
        if (pc_time > 0 && !config_.clock_set.empty() &&
            platform::clock_sync::offer(config_.clock_set, pc_time, trusted_, "the PC")) {
            note("PET clock set from the PC");
        }
        if (!dir.empty()) last_visit_ = jack_store::load_last(dir, host_);
        send_pet();
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
        if (last_visit_.have && !last_noted_) {
            last_noted_ = true;
            note(jack_store::last_text(last_visit_, scan_.disk));
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
        const std::string mode = msg["mode"].str(full_ ? mode_ : "read");
        if (mode != mode_) {
            mode_ = mode;
            if (mode_ == "read" && agent_running_) agent_stop("PC switched to Read");
        }
    } else if (t == "abort") {
        if (agent_running_) agent_stop("stopped on the PC");
    } else if (t == "agent_more") {
        // The PC's answer to "Keep going?": Yes allows as many steps again.
        if (agent_running_ && !agent_held_.empty()) {
            const std::string held = std::move(agent_held_);
            agent_held_.clear();
            if (msg["yes"].boolean(false)) {
                agent_limit_ += agent_cap(mode_);
                note("Keep going: yes");
                agent_run(held);
            } else {
                agent_stop("Holden said stop after " + std::to_string(agent_steps_) + " commands");
            }
        }
    } else if (t == "open") {
        // Opening a chip uses the PET's own logins (SSH keys, Jellyfin, RomM), so
        // the PET decides, not the PC: [K] allows chips, and Terminal needs its own.
        const std::string id = msg["chip"].str();
        if (!pc_capable(id)) {
            note("PC asked for a chip that stays on the PET", LogLine::Kind::Fail);
        } else if (!pc_control_) {
            deny_open(id, "control");
        } else if (id == "terminal" && !pc_terminal_) {
            deny_open(id, "terminal");
        } else {
            opening_.push_back(id);
        }
    } else if (t == "home") {
        // The PC app's HOME: leave the PC-opened chip (stopping its player) and
        // come back to this Jack In screen.
        opening_.push_back(id());
    } else if (t == "mirror") {
        const bool on = msg["on"].boolean(false) && connected_;
        if (on && !mirror_on_) last_px_.clear();  // the next frame is a full one
        mirror_on_ = on;
    } else if (t == "key") {
        Key key{Key::Enter};
        if (input_ok_ && pc_control_ && key_from_name(msg["k"].str(), key) && inputs_.size() < 256) {
            inputs_.push_back({false, key, ""});
        }
    } else if (t == "pointer") {
        const std::string b = msg["b"].str();
        if (input_ok_ && pc_control_ && (b == "down" || b == "up") && inputs_.size() < 256) {
            Input in;
            in.pointer = true;
            in.down = b == "down";
            // A release outside the mirror picture arrives as -1/-1: release only.
            const long long x = msg["x"].integer(-1), y = msg["y"].integer(-1);
            in.x = x < 0 || y < 0 ? -1 : static_cast<int>(std::min<long long>(x, 319));
            in.y = x < 0 || y < 0 ? -1 : static_cast<int>(std::min<long long>(y, 169));
            inputs_.push_back(in);
        }
    } else if (t == "char") {
        const std::string c = msg["c"].str();
        if (input_ok_ && pc_control_ && !c.empty() && c.size() <= 4 && inputs_.size() < 256) {
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
        if (!ok && msg["reason"].str() == "read-only" && id != agent_id_) status_ = "PC is read-only: [F] asks for Full Access";
        if (agent_running_ && id == agent_id_) {
            agent_id_ = 0;
            if (chat_waiting_) {
                agent_stop("busy");
            } else {
                note("Alice reads the output (step " + std::to_string(agent_steps_) + ")");
                ask_chat(output_prompt(mode_, agent_cmd_, ok, msg["reason"].str(), msg["out"].str()));
            }
        }
    } else if (t == "chat" || t == "voice") {
        // Talking to Alice from the PC app: the PET is the one with the bridge.
        if (!pc_control_ && !trusted_) {
            // Alice is reached with the PET's own login, so a PC nobody allowed doesn't get her.
            if (pc_wants_.empty()) {
                pc_wants_ = "control";
                pc_wants_chip_.clear();
                attention_ = true;
                note("PC wants to talk to Alice: waiting for [K] here", LogLine::Kind::Fail);
            }
            status_ = "PC wants to talk to Alice: [K] allows it";
            send(R"({"t":"alice","ok":false,"text":"Press [K] on the PET to let this PC talk to Alice.","heard":"","cmd":""})");
        } else if (chat_waiting_) {
            send(R"({"t":"alice","ok":false,"text":"Still answering the last one...","heard":"","cmd":""})");
        } else if (t == "chat") {
            const std::string text = trim(msg["text"].str());
            if (!text.empty()) {
                note(config_.jack_operator + ": " + AliceViewModel::screen_text(text));
                // Say so when the bridge's 600 characters won't hold all of it.
                const std::string prompt = chat_prompt(host_, mode_, text);
                if (clip_for_bridge(prompt).size() < prompt.size()) {
                    note("that message was too long: Alice only got the start of it", LogLine::Kind::Fail);
                }
                agent_running_ = true;
                agent_steps_ = 0;
                agent_id_ = 0;
                agent_limit_ = agent_cap(mode_);
                agent_held_.clear();
                ask_chat(chat_prompt(host_, mode_, text));
            }
        } else {
            std::string wav;
            if (platform::jack::base64_decode(msg["wav"].str(), wav) && wav.size() > 44) {
                note(config_.jack_operator + ": (voice, " + std::to_string(wav.size() / 32000) + " s)");
                agent_running_ = true;
                agent_steps_ = 0;
                agent_id_ = 0;
                agent_limit_ = agent_cap(mode_);
                agent_held_.clear();
                chat_waiting_ = true;
                alice_waiting_ = true;
                // The same header a typed message gets, so Alice knows the access mode and the CMD: rule.
                chat_.run([config = config_, wav = std::move(wav), gen = chat_generation_,
                           prefix = chat_prompt(host_, mode_, "")]() {
                    return std::make_pair(gen, bridge_voice(config, wav, prefix));
                });
            } else {
                send(R"({"t":"alice","ok":false,"text":"That recording didn't come through. Try again?","heard":"","cmd":""})");
            }
        }
    } else if (t == "progress") {
        // Video playing in the PC app (Jellyfin chip): where it is, and when it stops.
        pc_link::Progress p;
        p.item = msg["item"].str();
        p.pos = msg["pos"].num(0);
        p.paused = msg["paused"].boolean(false);
        p.state = msg["state"].str("playing");
        p.error = msg["error"].str();
        if (progress_.size() < 64) progress_.push_back(std::move(p));
    } else if (t == "net_fail") {
        relay_.fail(static_cast<int>(msg["id"].integer()));
    } else if (t == "bye") {
        const std::string why = msg["why"].str("jack out");
        note("PC app closed (" + why + ")");
        // "admin handover" is the same visit going on in the elevated app.
        if (why != "admin handover") end_session("PC app closed (" + why + ")");
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
    agent_running_ = false;
    agent_steps_ = 0;
    agent_id_ = 0;
    agent_cmd_.clear();
    agent_held_.clear();
    mode_ = "read";
    input_ok_ = false;
    pc_control_ = config_.jack_pc_control;
    pc_terminal_ = false;
    pc_wants_.clear();
    pc_wants_chip_.clear();
    trusted_ = false;
    trust_armed_ = false;
    last_visit_ = jack_store::LastVisit{};
    last_noted_ = false;
    ++chat_generation_;  // an answer still on its way belongs to the session that just ended
    mirror_on_ = false;
    pc_chip_.clear();
    media_.stop();
    site_.stop();
    progress_.clear();
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

bool JackInChip::send(const std::string& json, bool droppable) {
    if (capture_sent) {
        sent.push_back(json);
        return true;
    }
    return server_.send(json, droppable);
}

// Who runs the PET and what this PC may do with it (sent at link and on every change).
void JackInChip::send_pet() {
    send("{\"t\":\"pet\",\"operator\":\"" + json_escape(config_.jack_operator) + "\",\"control\":" +
         (pc_control_ ? "true" : "false") + ",\"terminal\":" + (pc_terminal_ ? "true" : "false") + ",\"trusted\":" +
         (trusted_ ? "true" : "false") + "}");
}

void JackInChip::deny_open(const std::string& chip, const char* what) {
    pc_wants_ = what;
    pc_wants_chip_ = chip;
    attention_ = true;
    const bool terminal = std::string(what) == "terminal";
    note(terminal ? "PC asked for TERMINAL (your SSH keys): waiting for [K] here"
                  : "PC asked to open a chip: waiting for [K] here",
         LogLine::Kind::Fail);
    status_ = terminal ? "PC wants TERMINAL (your SSH keys): [K] allows" : "PC wants to open chips: [K] allows it";
    send(std::string("{\"t\":\"denied\",\"what\":\"") + what + "\",\"chip\":\"" + json_escape(chip) + "\"}");
}

// [K] on the PET: allow what the PC is waiting for, or switch PC control on/off.
void JackInChip::grant_key() {
    if (pc_wants_ == "terminal" && pc_control_) {
        pc_terminal_ = true;
        note("you let the PC open TERMINAL for this session");
    } else if (!pc_control_) {
        pc_control_ = true;
        note("you let the PC open and drive chips");
        // Terminal is never part of the first [K]: it is asked for by name.
        if (pc_wants_chip_ == "terminal") {
            pc_wants_ = "terminal";
            status_ = "PC wants TERMINAL (your SSH keys): [K] allows";
            attention_ = true;
            send_pet();
            send(R"({"t":"denied","what":"terminal","chip":"terminal"})");
            return;
        }
    } else {
        pc_control_ = false;
        pc_terminal_ = false;
        inputs_.clear();
        note("you took chip control back from the PC");
        status_ = "PC can no longer open or drive chips";
        pc_wants_.clear();
        pc_wants_chip_.clear();
        send_pet();
        return;
    }
    status_.clear();
    // The one place [T] is advertised: right after Holden allowed this PC once.
    if (pc_control_ && !pc_terminal_ && !trusted_ && !state_dir().empty()) status_ = "PC allowed. [T] twice trusts it every time";
    send_pet();
    if (!pc_wants_chip_.empty() && (pc_wants_chip_ != "terminal" || pc_terminal_)) opening_.push_back(pc_wants_chip_);
    pc_wants_.clear();
    pc_wants_chip_.clear();
}

void JackInChip::note(const std::string& text, LogLine::Kind kind) {
    push_log({kind, text, 0});
    if (view_ != View::Log) log_scroll_ = 0;
}

void JackInChip::push_log(LogLine line) {
    log_.push_back(std::move(line));
    while (log_.size() > kMaxLog) log_.pop_front();
    if (connected_) ++session_lines_;
}

std::string JackInChip::state_dir() const {
    return config_.jack_state_dir.empty() ? "" : platform::proc::expand_home(config_.jack_state_dir);
}

// [T] on the PET, twice: trust this PC from now on, or forget it again. The PET
// makes the token and keeps the list; the PC only shows the token back in hello.
void JackInChip::trust_key() {
    const std::string dir = state_dir();
    if (dir.empty()) {
        status_ = "no [jackin] state_dir: can't keep a trusted list";
        return;
    }
    if (!trust_armed_) {
        trust_armed_ = true;
        status_ = trusted_ ? "Forget this trusted PC? [T] again" : "Trust this PC every time? [T] again";
        return;
    }
    trust_armed_ = false;
    auto list = jack_store::load_trusted(dir);
    if (trusted_) {
        // Forget every entry for this PC's name; its token stops working at once.
        list.erase(std::remove_if(list.begin(), list.end(),
                                  [&](const jack_store::TrustedPc& pc) { return pc.host == host_; }),
                   list.end());
        if (!jack_store::save_trusted(dir, list)) {
            status_ = "couldn't save the trusted list";
            return;
        }
        trusted_ = false;
        send(R"({"t":"trust","token":""})");
        note("you no longer trust this PC");
        status_ = "This PC is no longer trusted";
        send_pet();
        return;
    }
    const std::string token = jack_store::new_token();
    if (token.empty()) {
        status_ = "couldn't make a trust token";
        return;
    }
    list.erase(std::remove_if(list.begin(), list.end(),
                              [&](const jack_store::TrustedPc& pc) { return pc.host == host_; }),
               list.end());
    while (list.size() >= jack_store::kMaxTrusted) list.erase(list.begin());
    list.push_back({token, host_, jack_store::today()});
    if (!jack_store::save_trusted(dir, list)) {
        status_ = "couldn't save the trusted list";
        return;
    }
    trusted_ = true;
    pc_control_ = true;
    send("{\"t\":\"trust\",\"token\":\"" + token + "\"}");
    note("you trusted this PC: it may open chips and talk to Alice every time");
    status_ = "Trusted. [T] twice forgets it again";
    send_pet();
    if (!pc_wants_chip_.empty() && pc_wants_chip_ != "terminal") opening_.push_back(pc_wants_chip_);
    if (pc_wants_ == "control") {
        pc_wants_.clear();
        pc_wants_chip_.clear();
    }
}

std::string JackInChip::report_text(const std::string& why) const {
    std::string out = "JACK IN report\n";
    out += "PC:      " + host_ + (os_.empty() ? "" : " (" + os_ + ")") + (user_.empty() ? "" : ", user " + user_) + "\n";
    out += "Linked:  " + session_began_ + "\n";
    out += "Ended:   " + now_text() + ", " + why + "\n";
    out += "Access:  " + mode_ + (admin_ ? ", admin" : "") + (trusted_ ? ", trusted PC" : "") + "\n";
    if (scan_.have) {
        out += "Scan:    " + scan_summary(scan_);
        if (scan_.startup >= 0) out += " · startup items " + std::to_string(scan_.startup);
        out += " · Defender " + scan_.defender + "\n";
        if (!scan_.top.empty()) {
            out += "Top RAM:";
            for (const auto& hog : scan_.top) out += " " + hog.name + " " + std::to_string(hog.mb) + "MB";
            out += "\n";
        }
    }
    if (last_visit_.have) out += "Before:  " + jack_store::last_text(last_visit_, scan_.disk) + "\n";
    if (!alice_text_.empty()) out += "Alice:   " + alice_text_ + "\n";
    if (!suggestion_.empty()) out += "Fix not run: " + suggestion_ + "\n";
    out += "\nSession log\n";
    const size_t count = std::min(session_lines_, log_.size());
    for (size_t i = log_.size() - count; i < log_.size(); ++i) {
        const LogLine& line = log_[i];
        const char* mark = line.kind == LogLine::Kind::Fail ? "x " : line.kind == LogLine::Kind::Pending ? "? " : "  ";
        out += mark + line.text + "\n";
    }
    return out;
}

void JackInChip::end_session(const std::string& why) {
    const std::string dir = state_dir();
    if (!connected_ || dir.empty()) return;
    if (config_.jack_reports) {
        const std::string path = jack_store::write_report(dir, host_, report_text(why));
        if (!path.empty()) note("report saved: " + path);
    }
    if (scan_.have) {
        jack_store::LastVisit visit;
        visit.have = true;
        visit.when = jack_store::today();
        visit.cpu = scan_.cpu;
        visit.ram = scan_.ram;
        visit.disk_free = scan_.disk_free;
        visit.startup = scan_.startup;
        visit.defender = scan_.defender;
        jack_store::save_last(dir, host_, visit);
    }
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
    const std::string last = last_visit_.have ? jack_store::last_text(last_visit_, scan_.disk) : "";
    alice_.run([config = config_, prompt = alice_prompt(host_, os_, scan_, last), gen = ++alice_generation_]() {
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
    if (!reply.transcript.empty()) note(config_.jack_operator + " (heard): " + AliceViewModel::screen_text(reply.transcript));
    if (reply.ok) {
        alice_text_ = words;
        note("Alice: " + words);
    } else {
        note("Alice: (can't reach her: " + reply.error + ")", LogLine::Kind::Fail);
    }
    // Her command goes straight to the PC while the loop runs; the PC applies its
    // mode (Read: read-only list, Ask: Yes/No each, Full: Yes/No only if destructive).
    bool run = reply.ok && !command.empty() && agent_running_ && connected_;
    // At the cap the command waits and the PC asks Holden "Keep going?" (agent_more).
    const bool hold = run && agent_steps_ >= agent_limit_;
    if (hold) run = false;
    const std::string text = reply.ok ? words : "Can't reach Alice: " + reply.error;
    if (!command.empty() && !run && !hold && reply.ok) suggestion_ = command;
    send("{\"t\":\"alice\",\"ok\":" + std::string(reply.ok ? "true" : "false") + ",\"text\":\"" +
         json_escape(text) + "\",\"heard\":\"" + json_escape(reply.transcript) + "\",\"cmd\":\"" +
         json_escape(run ? command : "") + "\",\"step\":" + std::to_string(run ? agent_steps_ + 1 : 0) + "}");
    if (run) {
        agent_run(command);
    } else if (hold) {
        agent_held_ = command;
        note("Alice ran " + std::to_string(agent_steps_) + " commands: asking the PC to keep going");
        send("{\"t\":\"agent_more\",\"steps\":" + std::to_string(agent_steps_) + ",\"next\":\"" +
             json_escape(command) + "\"}");
    } else {
        agent_running_ = false;
        agent_id_ = 0;
    }
    touch();
}

void JackInChip::ask_chat(const std::string& prompt) {
    chat_waiting_ = true;
    alice_waiting_ = true;
    chat_.run([config = config_, prompt = clip_for_bridge(prompt), gen = chat_generation_]() {
        return std::make_pair(gen, bridge_ask(config, prompt));
    });
}

void JackInChip::agent_run(const std::string& command) {
    ++agent_steps_;
    agent_id_ = next_id_++;
    agent_cmd_ = command;
    send("{\"t\":\"run\",\"id\":" + std::to_string(agent_id_) + ",\"agent\":true,\"cmd\":\"" +
         json_escape(command) + "\"}");
    push_log({LogLine::Kind::Pending, "> " + command, agent_id_});
}

void JackInChip::agent_stop(const std::string& why) {
    agent_held_.clear();
    agent_running_ = false;
    agent_id_ = 0;
    note("Alice's command loop stopped (" + why + ")");
    send("{\"t\":\"agent_stop\",\"why\":\"" + json_escape(why) + "\"}");
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
    push_log({LogLine::Kind::Pending, "> " + suggestion_, id});
    suggestion_.clear();
    status_.clear();
    touch();
}

void JackInChip::jack_out(const std::string& why) {
    send("{\"t\":\"jackout\",\"why\":\"" + json_escape(why) + "\"}");
    server_.flush(300);  // the PC has to see this line before the socket closes
    end_session(why);
    server_.drop_client();
    reset_session();
    note("jacked out");
    view_ = View::Waiting;
    status_ = "Jacked out. The PC app closed.";
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
    trust_armed_ = false;
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
    if (c != 't') trust_armed_ = false;
    if (c == 'l') {
        view_ = view_ == View::Log ? (connected_ ? View::Session : View::Waiting) : View::Log;
        log_scroll_ = 0;
        touch();
        return;
    }
    if (!connected_) return;
    if (c == 't') {
        trust_key();
    } else if (c == 'f') {
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
    } else if (c == 'k') {
        grant_key();
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
    std::deque<GadgetResult> gadget_done;
    {
        std::lock_guard<std::mutex> lock(gadget_->mutex);
        gadget_done.swap(gadget_->done);
    }
    for (const auto& g : gadget_done) {
        // Only the newest "up" counts, and only while the chip still wants the port up.
        if (!g.up || !usb_want_up_ || g.seq != usb_up_seq_) continue;
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
        end_session("the PC went away");
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
    std::pair<size_t, BridgeReply> chat;
    while (chat_.poll(chat)) {
        if (connected_ && chat.first == chat_generation_) on_chat_reply(chat.second);
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

std::string JackInChip::alice_prompt(const std::string& host, const std::string& os, const Scan& scan,
                                     const std::string& last) {
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
    if (!last.empty()) p += " · " + last + " (say what changed if it matters)";
    return clip_for_bridge(p);
}

namespace {
const char* mode_rule(const std::string& mode) {
    // Kept short: the bridge takes 600 characters and the rest is Holden's message.
    if (mode == "full") return "FULL: commands run at once; destructive ones need Holden's OK";
    if (mode == "ask") return "ASK: Holden OKs each command";
    return "READ: only read-only commands run (Get-*, Test-*, ipconfig...)";
}
std::string upper(std::string v) {
    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return std::toupper(c); });
    return v;
}
}  // namespace

std::string JackInChip::chat_prompt(const std::string& host, const std::string& mode, const std::string& text) {
    return "[JACK IN on " + host + ", " + mode_rule(mode) +
           ". To run PowerShell there put ONE line 'CMD: <command>' first; its output comes back to you. "
           "No CMD when done. Brief, no markdown.] " +
           text;
}

std::string JackInChip::output_prompt(const std::string& mode, const std::string& cmd, bool ok,
                                      const std::string& reason, const std::string& out) {
    std::string head = "[JACK IN, mode " + upper(mode) + "] ";
    if (!ok && reason == "declined") return head + "Holden declined `" + cmd + "` on the PC. Continue without it, or stop.";
    if (!ok && reason == "read-only")
        return head + "Not run: `" + cmd + "` is not read-only and the PC is in READ mode. Use a read-only command or stop.";
    std::string body = trim(out);
    if (body.empty()) body = "(no output)";
    head += "Output of `" + cmd + "` (exit " + (ok ? "ok" : "failed") + "): ";
    // The bridge keeps bridge_max_input() characters: the command and the start of its output.
    const size_t limit = bridge_max_input();
    const size_t room = limit > head.size() + 40 ? limit - head.size() - 20 : 40;
    const std::string clipped = clip_for_bridge(body, room);
    if (clipped.size() < body.size()) body = clipped + " [cut]";
    return head + body;
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
    if (!mirroring()) {
        frame_asap_ = false;
        return false;
    }
    if (frame_asap_) {
        frame_asap_ = false;
        last_frame_ms_ = now_ms;
        return true;
    }
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
        if (!send_frame_band(screen, w, h, px, y, end)) {
            // The PC isn't reading (a Yes/No box is up there): skip the rest and
            // start from a whole picture once it reads again.
            px.clear();
            break;
        }
        y = end;
    }
    last_px_ = std::move(px);
    if (input_pending_) {
        input_pending_ = false;
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - input_at_).count();
        if (timing_log_due(input_log_next_, ms)) LOG_INFO("pc input: next frame sent {:.1f} ms after the input", ms);
    }
}

// Called by the input drain only while the PC is linked.
void JackInChip::mark_input_applied() {
    input_pending_ = true;
    input_at_ = std::chrono::steady_clock::now();
}

// Logs a timing at most twice a second, and always one that took 50 ms or more.
bool JackInChip::timing_log_due(std::chrono::steady_clock::time_point& next, double ms) {
    const auto now = std::chrono::steady_clock::now();
    if (ms < 50.0 && now < next) return false;
    next = now + std::chrono::milliseconds(500);
    return true;
}

bool JackInChip::send_frame_band(const std::string& screen, int w, int h, const std::vector<uint16_t>& px, int y0,
                                 int y1) {
    const size_t from = static_cast<size_t>(y0) * w;
    const std::string rle = rle_rgb565(px, from, static_cast<size_t>(y1 - y0) * w);
    return send("{\"t\":\"frame\",\"screen\":\"" + json_escape(screen) + "\",\"w\":" + std::to_string(w) +
                ",\"h\":" + std::to_string(h) + ",\"y\":" + std::to_string(y0) + ",\"n\":" +
                std::to_string(y1 - y0) + ",\"rle\":\"" + base64_encode(rle) + "\"}",
                true);
}

} // namespace viewmodel::chips
