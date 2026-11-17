/*
 * SPDX-License-Identifier: MIT
 */

#include "viewmodel/alice_viewmodel.h"

#include "model/chip_registry.h"

#include <algorithm>

namespace viewmodel {
namespace {

constexpr uint32_t kThinkingDotMs = 400;

size_t utf8_len(unsigned char lead) {
    if (lead < 0x80) return 1;
    if ((lead >> 5) == 0x6) return 2;
    if ((lead >> 4) == 0xE) return 3;
    if ((lead >> 3) == 0x1E) return 4;
    return 1;
}

// Splits UTF-8 into code points.
std::vector<std::string> code_points(const std::string& text) {
    std::vector<std::string> out;
    for (size_t i = 0; i < text.size();) {
        const size_t n = std::min(utf8_len(static_cast<unsigned char>(text[i])), text.size() - i);
        out.push_back(text.substr(i, n));
        i += n;
    }
    return out;
}

uint32_t decode(const std::string& cp) {
    const auto* s = reinterpret_cast<const unsigned char*>(cp.data());
    switch (cp.size()) {
    case 1: return s[0];
    case 2: return ((s[0] & 0x1Fu) << 6) | (s[1] & 0x3Fu);
    case 3: return ((s[0] & 0x0Fu) << 12) | ((s[1] & 0x3Fu) << 6) | (s[2] & 0x3Fu);
    default: return ((s[0] & 0x07u) << 18) | ((s[1] & 0x3Fu) << 12) | ((s[2] & 0x3Fu) << 6) | (s[3] & 0x3Fu);
    }
}

// Maps typography Departure Mono lacks to ASCII and drops emoji / symbols the
// bundled fonts cannot draw, so replies never show tofu boxes.
std::string normalize_for_screen(const std::string& text) {
    std::string out;
    for (const auto& cp : code_points(text)) {
        const uint32_t c = decode(cp);
        if (c == '\r' || c == 0xFE0F || c == 0x200D) continue;
        if (c == '\t') { out += ' '; continue; }
        if (c == 0x2018 || c == 0x2019) { out += '\''; continue; }
        if (c == 0x201C || c == 0x201D) { out += '"'; continue; }
        if (c == 0x2013 || c == 0x2014) { out += '-'; continue; }
        if (c == 0x2026) { out += "..."; continue; }
        if (c == 0x2022) { out += '*'; continue; }
        if (c >= 0x2190 && c <= 0x21FF) { out += cp; continue; }  // arrows (font has them)
        if (c >= 0x2000) continue;                                  // emoji, dingbats, CJK
        if (c == ' ' && !out.empty() && out.back() == ' ') continue;  // gaps left by dropped emoji
        out += cp;
    }
    // Drop spaces left before punctuation by removed emoji ("Hi ! " -> "Hi!").
    std::string tidy;
    for (size_t i = 0; i < out.size(); ++i) {
        if (out[i] == ' ' && i + 1 < out.size() && (out[i + 1] == '!' || out[i + 1] == ',' || out[i + 1] == '.' || out[i + 1] == '?')) continue;
        tidy += out[i];
    }
    return tidy;
}

} // namespace

std::string AliceViewModel::screen_text(const std::string& text) {
    return normalize_for_screen(text);
}

std::vector<std::string> AliceViewModel::wrap(const std::string& text, size_t cols) {
    std::vector<std::string> lines;
    std::string paragraph;
    const auto flush_paragraph = [&](const std::string& para) {
        std::vector<std::string> cps = code_points(para);
        size_t start = 0;
        if (cps.empty()) {
            lines.emplace_back();
            return;
        }
        while (start < cps.size()) {
            // Skip leading spaces on continuation lines.
            if (start > 0) {
                while (start < cps.size() && cps[start] == " ") ++start;
                if (start >= cps.size()) break;
            }
            size_t end = std::min(start + cols, cps.size());
            if (end < cps.size()) {
                // Break at the last space inside the window if there is one.
                size_t space = end;
                while (space > start && cps[space] != " ") --space;
                if (space > start) end = space;
            }
            std::string line;
            for (size_t i = start; i < end; ++i) line += cps[i];
            while (!line.empty() && line.back() == ' ') line.pop_back();
            lines.push_back(line);
            start = end;
        }
    };
    size_t pos = 0;
    while (pos <= text.size()) {
        const size_t nl = text.find('\n', pos);
        flush_paragraph(text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos));
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    // Collapse runs of blank lines (markdown spacing wastes the tiny bubble).
    std::vector<std::string> out;
    for (auto& line : lines) {
        if (line.empty() && (out.empty() || out.back().empty())) continue;
        out.push_back(std::move(line));
    }
    while (!out.empty() && out.back().empty()) out.pop_back();
    return out;
}

void AliceViewModel::tick(uint32_t elapsed_ms) {
    tick_chips(elapsed_ms);
    if (ChipModule* chip = active_chip(); chip && chip->take_go_home()) {
        leave_chip(Screen::Home);
    }
    if (screen_ == Screen::Boot) {
        boot_elapsed_ms_ = std::min(boot_elapsed_ms_ + elapsed_ms, kBootDurationMs);
        const int progress = static_cast<int>(boot_elapsed_ms_ * 100 / kBootDurationMs);
        if (progress != boot_progress_) {
            boot_progress_ = progress;
            ++revision_;
        }
        if (boot_elapsed_ms_ >= kBootDurationMs) {
            show(Screen::Home);
        }
        return;
    }

    if (screen_ == Screen::Slotted && chip_module(selected_chip_)) {
        slot_elapsed_ms_ += elapsed_ms;
        if (slot_elapsed_ms_ >= kSlotInMs) {
            open_chip(selected_chip_);
        }
    }

    if (!sleeping_ && (sleep_after_ms_ > 0 || dim_after_ms_ > 0)) {
        const ChipModule* chip = active_chip();
        // Waiting on Alice or a scan counts as activity: no dimming or napping mid-answer.
        const bool busy = (chip && chip->keep_awake()) || chat_state_ == ChatState::Thinking || scanning_;
        if (busy) {
            idle_ms_ = 0;
        } else {
            idle_ms_ += elapsed_ms;
            if (sleep_after_ms_ > 0 && idle_ms_ >= sleep_after_ms_) {
                sleep_now();
            }
        }
    }

    if (chat_state_ == ChatState::Thinking) {
        thinking_elapsed_ms_ += elapsed_ms;
        const int dots = static_cast<int>((thinking_elapsed_ms_ / kThinkingDotMs) % 4);
        if (dots != thinking_dots_) {
            thinking_dots_ = dots;
            ++revision_;
        }
    }
}

void AliceViewModel::handle_key(Key key) {
    // Any key skips the boot animation.
    if (screen_ == Screen::Boot) {
        show(Screen::Home);
        return;
    }
    idle_ms_ = 0;
    if (sleeping_) {
        // Any key wakes back to Home; the waking key is swallowed (a release
        // still has to reach a chip that is recording).
        if (key == Key::PushToTalkRelease) {
            if (ChipModule* chip = chip_module(selected_chip_)) chip->handle_key(key);
        }
        wake();
        return;
    }

    switch (screen_) {
    case Screen::Home:
        if (key == Key::Escape) {
            if (!input_.empty()) {
                input_.clear();
                ++revision_;
            } else {
                sleep_now();
            }
        } else if (key == Key::Backspace) {
            if (!input_.empty()) {
                size_t cut = input_.size() - 1;
                while (cut > 0 && (static_cast<unsigned char>(input_[cut]) & 0xC0) == 0x80) --cut;
                input_.erase(cut);
                ++revision_;
            }
        } else if (key == Key::Enter) {
            if (!input_.empty() && chat_state_ != ChatState::Thinking) {
                if (!chat_available_) {
                    set_bubble(ChatState::Error, "No Navi bridge configured. Set [bridge] url= in /etc/alicepet.conf.");
                } else {
                    outgoing_ = input_;
                    has_outgoing_ = true;
                    thinking_elapsed_ms_ = 0;
                    thinking_dots_ = 0;
                    set_bubble(ChatState::Thinking, "");
                }
                input_.clear();
                ++revision_;
            }
        } else if (key == Key::Up || key == Key::Left) {
            if (bubble_page_ > 0) {
                --bubble_page_;
                ++revision_;
            }
        } else if (key == Key::Down || key == Key::Right) {
            if (bubble_page_ + 1 < bubble_pages()) {
                ++bubble_page_;
                ++revision_;
            }
        } else if (key == Key::PushToTalk) {
            // Holding SPACE on Home jumps into VOICE and starts recording.
            const size_t voice = model::chip_index("voice");
            if (chip_module(voice)) {
                open_chip(voice);
                chip_module(voice)->handle_key(Key::PushToTalk);
            } else {
                set_bubble(ChatState::Error, "Voice isn't wired up yet. Type to me instead!");
            }
        } else if (input_.empty()) {
            // Digits are hotkeys only while the prompt is empty; once typing they are text.
            if (key == Key::Digit1) {
                show(Screen::NetScan);
                if (!scanned_once_) request_scan();
            } else if (key == Key::Digit2) {
                show(Screen::Folder);
            } else if (key == Key::Digit3) {
                open_chip(model::chip_index("voice"));
            }
        }
        break;

    case Screen::Folder: {
        const size_t count = model::chip_count();
        if (key == Key::Left) {
            selected_chip_ = (selected_chip_ + count - 1) % count;
            ++revision_;
        } else if (key == Key::Right) {
            selected_chip_ = (selected_chip_ + 1) % count;
            ++revision_;
        } else if (key == Key::Enter) {
            slot_elapsed_ms_ = 0;
            show(Screen::Slotted);
        } else if (key == Key::Escape) {
            show(Screen::Home);
        }
        break;
    }

    case Screen::Slotted:
        if (key == Key::Escape) {
            show(Screen::Folder);
        } else if (key == Key::Enter && chip_module(selected_chip_)) {
            open_chip(selected_chip_);
        }
        break;

    case Screen::Chip:
        if (ChipModule* chip = active_chip()) {
            if (!chip->handle_key(key)) {
                leave_chip(Screen::Folder);
            }
        }
        break;

    case Screen::NetScan:
        if (host_details_open_) {
            if (key == Key::Escape || key == Key::Enter) {
                host_details_open_ = false;
                ++revision_;
            }
            break;
        }
        if (key == Key::Escape) {
            show(Screen::Home);
        } else if (!hosts_.empty() && (key == Key::Up || key == Key::Left)) {
            selected_host_ = (selected_host_ + hosts_.size() - 1) % hosts_.size();
            ++revision_;
        } else if (!hosts_.empty() && (key == Key::Down || key == Key::Right)) {
            selected_host_ = (selected_host_ + 1) % hosts_.size();
            ++revision_;
        } else if (key == Key::Enter && !hosts_.empty()) {
            host_details_open_ = true;
            ++revision_;
        }
        break;

    case Screen::Boot:
        break;
    }
}

void AliceViewModel::handle_char(const std::string& utf8) {
    if (screen_ == Screen::Boot) {
        show(Screen::Home);
        return;
    }
    idle_ms_ = 0;
    if (sleeping_) {
        wake();
        return;
    }
    if (screen_ == Screen::Chip) {
        if (ChipModule* chip = active_chip()) {
            if (chip->wants_text()) {
                chip->handle_char(utf8);
                return;
            }
            chip->handle_char(utf8);
        }
        // Chips without text entry also take the letter navigation below.
    }
    if (screen_ == Screen::Home) {
        if (input_.empty()) {
            if (utf8 == "1") { handle_key(Key::Digit1); return; }
            if (utf8 == "2") { handle_key(Key::Digit2); return; }
            if (utf8 == "3") { handle_key(Key::Digit3); return; }
            if (utf8 == " ") return;  // no leading spaces
        }
        if (input_.size() + utf8.size() <= kMaxInput) {
            input_ += utf8;
            ++revision_;
        }
        return;
    }
    if (screen_ == Screen::NetScan && !host_details_open_ && (utf8 == "r" || utf8 == "R")) {
        request_scan();
        return;
    }
    // Screens without text entry also accept the device's letter navigation (F/X/Z/C).
    if (screen_ != Screen::Home) {
        if (utf8 == "f" || utf8 == "F") handle_key(Key::Up);
        else if (utf8 == "x" || utf8 == "X") handle_key(Key::Down);
        else if (utf8 == "z" || utf8 == "Z") handle_key(Key::Left);
        else if (utf8 == "c" || utf8 == "C") handle_key(Key::Right);
    }
}

void AliceViewModel::handle_ctrl(char letter) {
    idle_ms_ = 0;
    if (sleeping_) {
        wake();
        return;
    }
    if (ChipModule* chip = active_chip()) {
        chip->handle_ctrl(letter);
    }
}

uint32_t AliceViewModel::revision() const {
    const ChipModule* chip = active_chip();
    return revision_ * 65599u + (chip ? chip->revision() : 0u);
}

void AliceViewModel::set_chip_module(size_t chip_index, std::unique_ptr<ChipModule> module) {
    if (chip_index < chips_.size()) {
        chips_[chip_index] = std::move(module);
        ++revision_;
    }
}

ChipModule* AliceViewModel::chip_module(size_t chip_index) const {
    return chip_index < chips_.size() ? chips_[chip_index].get() : nullptr;
}

void AliceViewModel::open_chip(size_t chip_index) {
    ChipModule* chip = chip_module(chip_index);
    if (!chip) {
        return;
    }
    if (screen_ == Screen::Chip && selected_chip_ == chip_index) {
        return;
    }
    if (ChipModule* current = active_chip()) {
        current->leave();
    }
    selected_chip_ = chip_index;
    chip->enter();
    show(Screen::Chip);
}

void AliceViewModel::leave_chip(Screen next) {
    if (ChipModule* chip = active_chip()) {
        chip->leave();
    }
    show(next);
}

void AliceViewModel::tick_chips(uint32_t elapsed_ms) {
    for (auto& chip : chips_) {
        if (chip) chip->tick(elapsed_ms);
    }
}

void AliceViewModel::sleep_now() {
    if (sleeping_ || screen_ == Screen::Boot) {
        return;
    }
    sleeping_ = true;
    ++revision_;
}

void AliceViewModel::wake() {
    sleeping_ = false;
    idle_ms_ = 0;
    if (screen_ == Screen::Chip) {
        leave_chip(Screen::Home);
    } else {
        show(Screen::Home);
    }
    ++revision_;
}

std::vector<std::string> AliceViewModel::bubble_page_lines() const {
    std::vector<std::string> page;
    const size_t start = bubble_page_ * kBubbleRows;
    for (size_t i = start; i < bubble_lines_.size() && i < start + kBubbleRows; ++i) {
        page.push_back(bubble_lines_[i]);
    }
    return page;
}

size_t AliceViewModel::bubble_pages() const {
    return std::max<size_t>(1, (bubble_lines_.size() + kBubbleRows - 1) / kBubbleRows);
}

bool AliceViewModel::take_outgoing(std::string& out) {
    if (!has_outgoing_) {
        return false;
    }
    out = std::move(outgoing_);
    outgoing_.clear();
    has_outgoing_ = false;
    return true;
}

void AliceViewModel::on_chat_reply(bool ok, const std::string& text) {
    set_bubble(ok ? ChatState::Reply : ChatState::Error, text);
}

void AliceViewModel::set_bubble(ChatState state, const std::string& text) {
    chat_state_ = state;
    bubble_lines_ = wrap(normalize_for_screen(text), kBubbleCols);
    bubble_page_ = 0;
    ++revision_;
}

void AliceViewModel::set_hosts(std::vector<HostRow> hosts) {
    hosts_ = std::move(hosts);
    selected_host_ = 0;
    ++revision_;
}

size_t AliceViewModel::online_count() const {
    return static_cast<size_t>(std::count_if(hosts_.begin(), hosts_.end(), [](const HostRow& h) {
        return h.state == HostState::Online || h.state == HostState::Rest;
    }));
}

void AliceViewModel::request_scan() {
    if (scanning_) {
        return;
    }
    scan_requested_ = true;
    scanning_ = true;
    scanned_once_ = true;
    for (auto& host : hosts_) {
        host.state = HostState::Checking;
        host.latency_ms = -1;
    }
    ++revision_;
}

bool AliceViewModel::take_scan_request() {
    const bool requested = scan_requested_;
    scan_requested_ = false;
    return requested;
}

void AliceViewModel::on_host_result(size_t index, HostState state, int latency_ms, const std::string& detail) {
    if (index >= hosts_.size()) {
        return;
    }
    hosts_[index].state = state;
    hosts_[index].latency_ms = latency_ms;
    hosts_[index].detail = detail;
    ++revision_;
}

void AliceViewModel::on_scan_finished() {
    if (scanning_) {
        scanning_ = false;
        ++revision_;
    }
}

void AliceViewModel::show(Screen next) {
    if (screen_ == next) {
        return;
    }
    screen_ = next;
    host_details_open_ = false;
    ++revision_;
}

} // namespace viewmodel
