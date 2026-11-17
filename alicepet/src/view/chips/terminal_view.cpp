/*
 * SPDX-License-Identifier: MIT
 */

#include "view/chips/terminal_view.h"

#include <algorithm>
#include <string>

namespace view {
namespace {

using viewmodel::chips::TerminalChip;

constexpr ChipTheme kTermTheme{0x05080a, 0x05080a, 0x0e1418, 0x2b6b44, 0x47f08a};
constexpr uint32_t kSel = 0x2b6b44;
constexpr uint32_t kOut = 0xd6f5df;
constexpr uint32_t kSalmon = 0xff7a7a;
constexpr int kRowH = 15;
constexpr size_t kVisibleHosts = 9;
constexpr int kLineH = 12;

} // namespace

TerminalView::TerminalView(lv_obj_t* root, const AliceFonts& fonts) : fonts_(fonts) {
    frame_ = make_chip_frame(root, kTermTheme, fonts_);
    hosts_ = layer(frame_.body, 0, 0, 320, 139);
    shell_ = layer(frame_.body, 0, 0, 320, 139);
    for (size_t r = 0; r < rows_.size(); ++r) {
        rows_[r] = text(shell_, "", fonts_.small, kOut, 4, 3 + static_cast<int>(r) * kLineH);
        lv_label_set_long_mode(rows_[r], LV_LABEL_LONG_CLIP);
        lv_obj_set_width(rows_[r], static_cast<int>(TerminalChip::kCols) * kGlyphW + 2);
    }
    cursor_ = box(shell_, 0, 0, kGlyphW, 11, kTermTheme.text);
    ask_ = layer(frame_.body, 0, 0, 320, 139);
}

void TerminalView::render(const viewmodel::ChipModule& chip) {
    const auto& term = static_cast<const TerminalChip&>(chip);
    set_visible(hosts_, term.mode() == TerminalChip::Mode::Hosts);
    set_visible(shell_, term.mode() == TerminalChip::Mode::Shell);
    set_visible(ask_, term.mode() == TerminalChip::Mode::Ask);
    lv_obj_set_style_text_color(frame_.tag, lv_color_hex(kTermTheme.text), 0);
    switch (term.mode()) {
    case TerminalChip::Mode::Hosts: render_hosts(term); break;
    case TerminalChip::Mode::Shell: render_shell(term); break;
    case TerminalChip::Mode::Ask: render_ask(term); break;
    }
    lv_obj_align(frame_.tag, LV_ALIGN_TOP_RIGHT, -6, 2);
}

void TerminalView::render_hosts(const TerminalChip& term) {
    lv_label_set_text(frame_.title, "TERMINAL · HOSTS");
    lv_label_set_text(frame_.footer, "[↵]connect [A]ask Alice [D]hang up [R]check");
    const auto& hosts = term.hosts();
    size_t up = 0;
    for (const auto& h : hosts) up += h.reach == TerminalChip::Reach::Up ? 1 : 0;
    lv_label_set_text_fmt(frame_.tag, "%zu/%zu up", up, hosts.size());

    lv_obj_clean(hosts_);
    if (hosts.empty()) {
        text(hosts_, "No bookmarks. Add [terminal] host= lines\nto /etc/alicepet.conf", fonts_.small, kOut, 8, 6);
        return;
    }
    const size_t first = term.selected() >= kVisibleHosts ? term.selected() - kVisibleHosts + 1 : 0;
    for (size_t i = first; i < hosts.size() && i < first + kVisibleHosts; ++i) {
        const auto& h = hosts[i];
        const int y = 2 + static_cast<int>(i - first) * kRowH;
        const bool selected = i == term.selected();
        if (selected) {
            lv_obj_t* hl = box(hosts_, 6, y, 308, kRowH - 1, kSel);
            lv_obj_set_style_radius(hl, 3, 0);
        }
        uint32_t dot = 0x5d6b66;
        if (h.reach == TerminalChip::Reach::Up) dot = kGreen;
        if (h.reach == TerminalChip::Reach::Down) dot = kRed;
        lv_obj_t* d = box(hosts_, 12, y + 4, 6, 6, dot);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        text(hosts_, dot_leader(h.label, 22), fonts_.small, selected ? kWhite : kOut, 24, y + 1);
        std::string note = h.reach == TerminalChip::Reach::Down ? "offline" : h.note;
        if (term.connected(i)) note = "> connected";
        text(hosts_, clip_cols(note, 18), fonts_.small,
             term.connected(i) ? kYellow : (selected ? kWhite : kOut), 24 + 23 * kGlyphW, y + 1);
    }
}

void TerminalView::render_shell(const TerminalChip& term) {
    const auto* host = term.current_host();
    const auto* session = term.session();
    lv_label_set_text(frame_.title, host ? clip_cols(host->target, 32).c_str() : "shell");
    lv_label_set_text_fmt(frame_.tag, "%zux%zu", TerminalChip::kCols, TerminalChip::kRows);
    lv_label_set_text(frame_.footer, "[hold ESC]hosts [Ctrl+C]stop [Tab]complete");
    if (!session) return;
    for (size_t r = 0; r < rows_.size(); ++r) {
        lv_label_set_text(rows_[r], session->screen.row_text(r).c_str());
    }
    const bool show = session->screen.cursor_visible() && !session->ended && term.cursor_blink_on();
    set_visible(cursor_, show);
    lv_obj_set_pos(cursor_, 4 + static_cast<int>(session->screen.cursor_col()) * kGlyphW,
                   3 + static_cast<int>(session->screen.cursor_row()) * kLineH);
}

void TerminalView::render_ask(const TerminalChip& term) {
    const auto* host = term.current_host();
    lv_label_set_text(frame_.title, ("ASK ALICE · " + clip_cols(host ? host->label : "?", 28)).c_str());
    lv_label_set_text(frame_.tag, "suggest");
    lv_obj_set_style_text_color(frame_.tag, lv_color_hex(kYellow), 0);
    lv_label_set_text(frame_.footer, "nothing runs until you press Enter");
    lv_obj_clean(ask_);

    const auto wrap_label = [&](const std::string& value, uint32_t color, int y) {
        lv_obj_t* label = text(ask_, value, fonts_.small, color, 8, y);
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(label, 304);
        lv_obj_set_style_text_line_space(label, 2, 0);
        lv_obj_update_layout(label);
        return y + lv_obj_get_height(label);
    };

    switch (term.ask_state()) {
    case TerminalChip::AskState::Typing:
        wrap_label("? " + term.ask_input() + "_", kWhite, 6);
        wrap_label("Ask Alice about " + (host ? host->label : std::string("this host")) +
                       ". She suggests one command; you decide.",
                   kOut, 56);
        lv_label_set_text(frame_.footer, "[↵]ask [ESC]back · nothing runs by itself");
        break;
    case TerminalChip::AskState::Waiting: {
        static const char* kDots[] = {"", ".", "..", "..."};
        const int y = wrap_label("? " + term.ask_question(), kWhite, 6);
        wrap_label(std::string("Alice is thinking") + kDots[term.thinking_dots() & 3], kSalmon, y + 6);
        break;
    }
    case TerminalChip::AskState::Error: {
        const int y = wrap_label("? " + term.ask_question(), kWhite, 6);
        wrap_label("Alice: " + term.ask_error(), kSalmon, y + 6);
        lv_label_set_text(frame_.footer, "[ESC]back");
        break;
    }
    case TerminalChip::AskState::Suggestion: {
        int y = wrap_label("? " + term.ask_question(), kWhite, 6);
        y = wrap_label("Alice: " + clip_cols(term.ask_why(), 88), kSalmon, y + 4);
        lv_obj_t* cmd = box(ask_, 8, y + 6, 304, 22, 0x0b1a10);
        set_border(cmd, 1, kTermTheme.edge);
        lv_obj_set_style_radius(cmd, 3, 0);
        lv_obj_t* cmd_text = text(cmd, term.ask_command(), fonts_.small, kTermTheme.text, 6, 4);
        lv_label_set_long_mode(cmd_text, LV_LABEL_LONG_CLIP);
        lv_obj_set_width(cmd_text, 290);
        const int by = y + 36;
        const auto button = [&](const char* label, int x, bool filled) {
            lv_obj_t* b = box(ask_, x, by, 0, 16, filled ? kTermTheme.text : kTermTheme.bg_top);
            lv_obj_set_size(b, LV_SIZE_CONTENT, 16);
            lv_obj_set_style_pad_hor(b, 5, 0);
            set_border(b, 1, kTermTheme.text);
            lv_obj_set_style_radius(b, 3, 0);
            lv_obj_t* t = text(b, label, fonts_.small, filled ? 0x05080a : kTermTheme.text, 0, 0);
            lv_obj_align(t, LV_ALIGN_LEFT_MID, 0, 0);
            lv_obj_update_layout(b);
            return x + lv_obj_get_width(b) + 6;
        };
        int x = button("↵ Run", 8, true);
        x = button("E Edit", x, false);
        button("ESC No", x, false);
        break;
    }
    }
}

} // namespace view
