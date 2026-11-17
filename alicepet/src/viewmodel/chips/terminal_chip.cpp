/*
 * SPDX-License-Identifier: MIT
 */

#include "viewmodel/chips/terminal_chip.h"

#include "platform/net.h"
#include "viewmodel/alice_viewmodel.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>
#include <sstream>

namespace viewmodel::chips {
namespace {

// A fresh session's screen must be quiet this long before its prompt is trusted.
constexpr uint32_t kPromptQuietMs = 700;

std::string trim(const std::string& value) {
    size_t a = 0;
    size_t b = value.size();
    while (a < b && std::isspace(static_cast<unsigned char>(value[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(value[b - 1]))) --b;
    return value.substr(a, b - a);
}

std::string lower(std::string value) {
    for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}

std::string strip_ticks(std::string value) {
    value = trim(value);
    while (!value.empty() && (value.front() == '`' || value.front() == '$')) value = trim(value.substr(1));
    while (!value.empty() && value.back() == '`') value.pop_back();
    return trim(value);
}

// "holden@100.103.43.8" -> "100.103.43.8"
std::string host_part(const std::string& target) {
    const size_t at = target.rfind('@');
    return at == std::string::npos ? target : target.substr(at + 1);
}

size_t utf8_len(unsigned char lead) {
    if (lead < 0x80) return 1;
    if ((lead >> 5) == 0x6) return 2;
    if ((lead >> 4) == 0xE) return 3;
    if ((lead >> 3) == 0x1E) return 4;
    return 1;
}

} // namespace

std::string sanitize_command(const std::string& command) {
    std::string out;
    for (const char ch : command) {
        const auto c = static_cast<unsigned char>(ch);
        if (c < 0x20 || c == 0x7f) continue;
        out += ch;
    }
    return trim(out);
}

std::string build_ask_prompt(const std::string& host_desc, const std::string& question) {
    const std::string head =
        "PET TERMINAL, Ask Alice. Reply in exactly two lines, no markdown: 'CMD: <one shell command>' then "
        "'WHY: <one short sentence>'. Holden sees the command and decides whether to run it. Keep the command "
        "short (under 120 characters). Host: " +
        host_desc + ". Question: ";
    return clip_for_bridge(head + question);
}

std::vector<SshConfigHost> read_ssh_config(const std::string& path) {
    std::vector<SshConfigHost> out;
    std::ifstream in(platform::proc::expand_home(path));
    std::string line;
    std::vector<size_t> current;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        std::istringstream words(line);
        std::string key;
        words >> key;
        key = lower(key);
        if (key == "host") {
            current.clear();
            std::string alias;
            bool first = true;
            while (words >> alias) {
                if (alias.find_first_of("*?!") != std::string::npos) continue;
                // Only the first alias of a Host line becomes a bookmark; others are synonyms.
                if (first) {
                    out.push_back({alias, "", "", 0});
                    current.push_back(out.size() - 1);
                    first = false;
                }
            }
        } else if (key == "match") {
            current.clear();
        } else {
            std::string value;
            std::getline(words, value);
            value = trim(value);
            for (size_t index : current) {
                if (key == "hostname") out[index].hostname = value;
                else if (key == "user") out[index].user = value;
                else if (key == "port") out[index].port = std::atoi(value.c_str());
            }
        }
    }
    return out;
}

TerminalChip::TerminalChip(model::PetConfig config) : config_(std::move(config)) {
    const auto ssh_hosts = config_.ssh_config_import.empty() ? std::vector<SshConfigHost>{}
                                                               : read_ssh_config(config_.ssh_config_import);
    const auto lookup = [&](const std::string& alias) -> const SshConfigHost* {
        for (const auto& h : ssh_hosts) {
            if (h.alias == alias) return &h;
        }
        return nullptr;
    };

    std::set<std::string> seen;
    for (const auto& bookmark : config_.bookmarks) {
        Host host;
        host.label = bookmark.label;
        host.target = bookmark.target;
        host.note = bookmark.note;
        host.probe_host = host_part(bookmark.target);
        host.port = bookmark.port > 0 ? bookmark.port : 22;
        if (const auto* alias = lookup(bookmark.target)) {
            if (!alias->hostname.empty()) host.probe_host = alias->hostname;
            if (bookmark.port <= 0 && alias->port > 0) host.port = alias->port;
        }
        seen.insert(bookmark.label);
        seen.insert(bookmark.target);
        seen.insert(host.probe_host + ":" + std::to_string(host.port));
        hosts_.push_back(std::move(host));
    }
    for (const auto& alias : ssh_hosts) {
        const std::string address = alias.hostname.empty() ? alias.alias : alias.hostname;
        const int port = alias.port > 0 ? alias.port : 22;
        if (seen.count(alias.alias) || seen.count(address + ":" + std::to_string(port))) continue;
        Host host;
        host.label = alias.alias;
        host.target = alias.alias;
        host.note = "ssh_config";
        host.probe_host = address;
        host.port = port;
        seen.insert(alias.alias);
        seen.insert(address + ":" + std::to_string(port));
        hosts_.push_back(std::move(host));
    }
}

void TerminalChip::enter() {
    if (!probed_) probe_all();
}

void TerminalChip::probe_all() {
    probed_ = true;
    for (size_t i = 0; i < hosts_.size(); ++i) {
        hosts_[i].reach = Reach::Checking;
        probes_.run([i, address = hosts_[i].probe_host, port = hosts_[i].port]() {
            const auto probe = platform::net::probe_tcp(address, port, 2500);
            return ProbeResult{i, probe.state == platform::net::ProbeState::Online, probe.latency_ms};
        });
    }
    touch();
}

bool TerminalChip::connected(size_t host) const {
    const auto it = sessions_.find(host);
    return it != sessions_.end() && !it->second->ended;
}

const TerminalChip::Session* TerminalChip::session() const {
    const auto it = sessions_.find(selected_);
    return it == sessions_.end() ? nullptr : it->second.get();
}

void TerminalChip::connect(size_t index) {
    if (index >= hosts_.size()) return;
    selected_ = index;
    auto& slot = sessions_[index];
    if (!slot || slot->ended) {
        slot = std::make_unique<Session>();
        auto argv = platform::proc::split_command(config_.ssh_cmd);
        for (const char* option : {"-tt", "-o", "ConnectTimeout=8", "-o", "ServerAliveInterval=20"}) {
            argv.emplace_back(option);
        }
        // An explicit bookmark port wins over ssh_config.
        const auto& host = hosts_[index];
        for (const auto& bookmark : config_.bookmarks) {
            if (bookmark.label == host.label && bookmark.port > 0) {
                argv.emplace_back("-p");
                argv.push_back(std::to_string(bookmark.port));
            }
        }
        argv.push_back(host.target);
        std::string error;
        slot->screen.feed("Connecting to " + host.label + "...\r\n");
        if (!slot->pty.start(argv, static_cast<int>(kCols), static_cast<int>(kRows), error)) {
            slot->screen.feed("\r\n" + error + "\r\n");
            slot->ended = true;
        }
    }
    mode_ = Mode::Shell;
    touch();
}

void TerminalChip::send_to_shell(const std::string& bytes) {
    const auto it = sessions_.find(selected_);
    if (it == sessions_.end()) return;
    if (it->second->ended) {
        // Any key on a closed session goes back to the list.
        if (bytes == "\r") {
            sessions_.erase(it);
            mode_ = Mode::Hosts;
            touch();
        }
        return;
    }
    it->second->pty.write(bytes);
}

bool TerminalChip::handle_key(Key key) {
    if (mode_ == Mode::Shell) {
        switch (key) {
        case Key::HoldEscape: mode_ = Mode::Hosts; touch(); return true;
        case Key::Enter: send_to_shell("\r"); return true;
        case Key::Backspace: send_to_shell("\x7f"); return true;
        case Key::Escape: send_to_shell("\x1b"); return true;
        case Key::Tab: send_to_shell("\t"); return true;
        case Key::Up: send_to_shell("\x1b[A"); return true;
        case Key::Down: send_to_shell("\x1b[B"); return true;
        case Key::Right: send_to_shell("\x1b[C"); return true;
        case Key::Left: send_to_shell("\x1b[D"); return true;
        default: return true;
        }
    }

    if (mode_ == Mode::Ask) {
        switch (ask_state_) {
        case AskState::Typing:
            if (key == Key::Escape || key == Key::HoldEscape) {
                mode_ = Mode::Hosts;
            } else if (key == Key::Backspace && !ask_input_.empty()) {
                size_t cut = ask_input_.size() - 1;
                while (cut > 0 && (static_cast<unsigned char>(ask_input_[cut]) & 0xC0) == 0x80) --cut;
                ask_input_.erase(cut);
            } else if (key == Key::Enter && !ask_input_.empty()) {
                const Host* host = current_host();
                ask_question_ = ask_input_;
                ask_input_.clear();
                ask_state_ = AskState::Waiting;
                wait_ms_ = 0;
                const std::string host_desc =
                    host ? "'" + host->label + "' (" + host->target + (host->note.empty() ? "" : ", " + host->note) + ")"
                         : std::string("unknown");
                const std::string prompt = build_ask_prompt(host_desc, ask_question_);
                asks_.run([config = config_, prompt]() { return bridge_ask(config, prompt); });
            }
            touch();
            return true;
        case AskState::Waiting:
            if (key == Key::Escape) {
                mode_ = Mode::Hosts;  // the reply is dropped when it arrives
                ask_state_ = AskState::Typing;
                touch();
            }
            return true;
        case AskState::Suggestion:
        case AskState::Error:
            if (key == Key::Enter && ask_state_ == AskState::Suggestion) {
                if (ask_command_fits()) run_suggestion(true);  // too long to show whole: E only
            } else if (key == Key::Escape || key == Key::Enter) {
                mode_ = Mode::Hosts;
                ask_state_ = AskState::Typing;
                touch();
            }
            return true;
        }
        return true;
    }

    // Hosts
    switch (key) {
    case Key::Escape:
        return false;
    case Key::Up:
    case Key::Left:
        if (!hosts_.empty()) selected_ = (selected_ + hosts_.size() - 1) % hosts_.size();
        touch();
        return true;
    case Key::Down:
    case Key::Right:
        if (!hosts_.empty()) selected_ = (selected_ + 1) % hosts_.size();
        touch();
        return true;
    case Key::Enter:
        connect(selected_);
        return true;
    default:
        return true;
    }
}

void TerminalChip::handle_char(const std::string& utf8) {
    if (mode_ == Mode::Shell) {
        const auto it = sessions_.find(selected_);
        if (it != sessions_.end() && it->second->ended) {
            send_to_shell("\r");  // any key closes a dead session view
            return;
        }
        send_to_shell(utf8);
        return;
    }
    if (mode_ == Mode::Ask) {
        if (ask_state_ == AskState::Typing) {
            if (ask_input_.size() + utf8.size() <= 200 && !(ask_input_.empty() && utf8 == " ")) ask_input_ += utf8;
            touch();
        } else if (ask_state_ == AskState::Suggestion && (utf8 == "e" || utf8 == "E")) {
            run_suggestion(false);
        }
        return;
    }
    // Hosts hotkeys
    if (utf8 == "a" || utf8 == "A") {
        if (hosts_.empty()) return;
        mode_ = Mode::Ask;
        ask_state_ = AskState::Typing;
        ask_input_.clear();
        touch();
    } else if ((utf8 == "d" || utf8 == "D") && sessions_.count(selected_)) {
        sessions_[selected_]->pty.close();
        sessions_.erase(selected_);
        touch();
    } else if (utf8 == "r" || utf8 == "R") {
        probe_all();
    }
}

void TerminalChip::handle_ctrl(char letter) {
    if (mode_ == Mode::Shell && letter >= 'a' && letter <= 'z') {
        send_to_shell(std::string(1, static_cast<char>(letter - 'a' + 1)));
    }
}

bool TerminalChip::keep_awake() const {
    if (mode_ == Mode::Ask) return ask_state_ == AskState::Waiting;
    if (mode_ == Mode::Shell) {
        const Session* s = session();
        return s && !s->ended && s->quiet_ms < kShellAwakeMs;
    }
    return false;
}

std::vector<std::string> TerminalChip::ask_command_lines() const {
    // Split into code points, then wrap at kCommandCols. A line breaks after its last
    // space when there is one (the space stays at the end of the line), otherwise
    // mid-word. Nothing is dropped: joining the lines gives the command back.
    std::vector<std::string> cps;
    for (size_t i = 0; i < ask_command_.size();) {
        const size_t len = std::min(utf8_len(static_cast<unsigned char>(ask_command_[i])), ask_command_.size() - i);
        cps.push_back(ask_command_.substr(i, len));
        i += len;
    }
    std::vector<std::string> lines;
    size_t start = 0;
    while (start < cps.size()) {
        size_t end = std::min(start + kCommandCols, cps.size());
        if (end < cps.size()) {
            size_t space = end;
            while (space > start && cps[space - 1] != " ") --space;
            if (space > start) end = space;
        }
        std::string line;
        for (size_t i = start; i < end; ++i) line += cps[i];
        lines.push_back(line);
        start = end;
    }
    return lines;
}

bool TerminalChip::at_shell_prompt(const Session& session) {
    // The last non-blank line on screen, as the user sees it.
    std::string last;
    for (size_t r = session.screen.rows(); r-- > 0;) {
        last = trim(session.screen.row_text(r));
        if (!last.empty()) break;
    }
    if (last.empty()) return false;
    const std::string l = lower(last);
    // Never type into a login, host-key or sudo prompt.
    for (const char* no : {"password", "passphrase", "yes/no", "(y/n)", "[y/n]", "verification code", "one-time", "otp:"}) {
        if (l.find(no) != std::string::npos) return false;
    }
    const char end = last.back();
    return end == '$' || end == '#' || end == '>' || end == '%';
}

// ENTER on a suggestion: open (or reuse) the host's shell and run it; E: type it only.
// A command that doesn't fit the box is never run (ENTER is refused in handle_key).
// Into a fresh session, or a shell that isn't sitting at a prompt, the command is only
// typed, never run, and only once a prompt shows (see tick()).
void TerminalChip::run_suggestion(bool execute) {
    if (ask_command_.empty()) return;
    if (execute && !ask_command_fits()) return;
    const bool fresh = !connected(selected_);
    connect(selected_);
    auto& slot = sessions_[selected_];
    if (!fresh && at_shell_prompt(*slot)) {
        slot->pty.write(ask_command_ + (execute ? "\r" : ""));
    } else {
        slot->pending_input = ask_command_;
        slot->pending_ms = 0;
    }
    ask_state_ = AskState::Typing;
}

void TerminalChip::on_ask_reply(const BridgeReply& reply) {
    if (mode_ != Mode::Ask || ask_state_ != AskState::Waiting) {
        return;  // cancelled
    }
    ask_command_.clear();
    ask_why_.clear();
    if (!reply.ok) {
        ask_error_ = reply.error;
        ask_state_ = AskState::Error;
        touch();
        return;
    }
    std::istringstream lines(reply.reply);
    std::string line;
    std::vector<std::string> other;
    while (std::getline(lines, line)) {
        std::string t = trim(line);
        while (!t.empty() && (t.front() == '*' || t.front() == '-' || t.front() == '>')) t = trim(t.substr(1));
        const std::string l = lower(t);
        if (l.rfind("cmd:", 0) == 0) ask_command_ = strip_ticks(t.substr(4));
        else if (l.rfind("why:", 0) == 0) ask_why_ = trim(t.substr(4));
        else if (!t.empty() && t != "```" && t.rfind("```", 0) != 0) other.push_back(t);
    }
    if (ask_command_.empty()) {
        // Fallback: the first `inline code` span.
        const size_t a = reply.reply.find('`');
        const size_t b = a == std::string::npos ? std::string::npos : reply.reply.find('`', a + 1);
        if (b != std::string::npos && b > a + 1) ask_command_ = strip_ticks(reply.reply.substr(a + 1, b - a - 1));
    }
    if (ask_why_.empty()) {
        for (const auto& t : other) ask_why_ += (ask_why_.empty() ? "" : " ") + t;
    }
    ask_command_ = sanitize_command(ask_command_);
    ask_why_ = AliceViewModel::screen_text(ask_why_);
    if (ask_command_.empty()) {
        ask_error_ = ask_why_.empty() ? "Alice didn't suggest a command." : ask_why_;
        ask_state_ = AskState::Error;
    } else {
        ask_state_ = AskState::Suggestion;
    }
    touch();
}

void TerminalChip::tick(uint32_t elapsed_ms) {
    ProbeResult probe{};
    while (probes_.poll(probe)) {
        if (probe.index < hosts_.size()) {
            hosts_[probe.index].reach = probe.up ? Reach::Up : Reach::Down;
            hosts_[probe.index].latency_ms = probe.latency_ms;
            touch();
        }
    }
    BridgeReply reply;
    while (asks_.poll(reply)) on_ask_reply(reply);
    if (mode_ == Mode::Ask && ask_state_ == AskState::Waiting) {
        const int before = thinking_dots();
        wait_ms_ += elapsed_ms;
        if (before != thinking_dots()) touch();
    }

    for (auto& [index, session] : sessions_) {
        std::string output;
        const bool alive = session->pty.read(output);
        session->quiet_ms += elapsed_ms;
        if (!output.empty()) {
            session->had_output = true;
            session->quiet_ms = 0;
            session->screen.feed(output);
            if (index == selected_ && mode_ == Mode::Shell) touch();
        }
        if (!alive && !session->ended) {
            session->ended = true;
            session->screen.feed("\r\n[connection closed - press a key]");
            touch();
        }
    }
    // Suggested command waiting for a shell: type it (no Enter) once a prompt is on
    // screen and output has settled; give up if none shows.
    if (auto it = sessions_.find(selected_); it != sessions_.end() && !it->second->pending_input.empty()) {
        Session& session = *it->second;
        session.pending_ms += elapsed_ms;
        if (session.ended) {
            session.pending_input.clear();
        } else if (session.had_output && session.quiet_ms >= kPromptQuietMs && at_shell_prompt(session)) {
            session.pty.write(session.pending_input);
            session.pending_input.clear();
            touch();
        } else if (session.pending_ms >= kPromptWaitMs) {
            session.pending_input.clear();
            session.screen.feed("\r\n[no shell prompt seen; suggested command not typed]\r\n");
            touch();
        }
    }

    if (mode_ == Mode::Shell) {
        const bool before = cursor_blink_on();
        blink_ms_ += elapsed_ms;
        if (before != cursor_blink_on()) touch();
    }
}

} // namespace viewmodel::chips
