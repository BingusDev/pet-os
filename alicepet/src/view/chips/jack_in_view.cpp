/*
 * SPDX-License-Identifier: MIT
 */

#include "view/chips/jack_in_view.h"

#include <algorithm>
#include <string>
#include <vector>

namespace view {
namespace {

using viewmodel::chips::JackInChip;
using LogKind = JackInChip::LogLine::Kind;

// mockups/comms.html .ji: violet radial, plum bars, lilac text.
constexpr ChipTheme kJackTheme{0x1a0f3a, 0x05030f, 0x2a0a3a, 0xc04aff, 0xe0b3ff};
constexpr uint32_t kLilac = 0xe0b3ff;
constexpr uint32_t kDim = 0x9a7ab8;
constexpr uint32_t kPink = 0xffb3d6;
constexpr uint32_t kOrange = 0xe8a31a;
constexpr int kLeft = 8;
constexpr int kRowH = 14;
constexpr size_t kCols = 43;

uint32_t level_color(int percent, int warn, int bad) {
    if (percent >= bad) return kRed;
    if (percent >= warn) return kYellow;
    return kGreen;
}

uint32_t log_color(LogKind kind) {
    switch (kind) {
    case LogKind::Ok: return kLilac;
    case LogKind::Fail: return kRed;
    case LogKind::Pending: return kYellow;
    case LogKind::Note: break;
    }
    return kDim;
}

std::string log_text(const JackInChip::LogLine& line) {
    if (line.kind == LogKind::Pending) return line.text + " ...";
    if (line.kind == LogKind::Fail && line.text.rfind("> ", 0) == 0) return line.text + " ×";
    return line.text;
}

// Draws coloured pieces left to right on one row (monospace, so x = columns * 7).
struct Row {
    lv_obj_t* parent;
    const lv_font_t* font;
    int x;
    int y;
    void add(const std::string& value, uint32_t color) {
        text(parent, value, font, color, x, y);
        size_t cols = 0;
        for (size_t i = 0; i < value.size(); ++i) {
            if ((static_cast<unsigned char>(value[i]) & 0xC0) != 0x80) ++cols;
        }
        x += static_cast<int>(cols) * kGlyphW;
    }
};

} // namespace

JackInView::JackInView(lv_obj_t* root, const AliceFonts& fonts) : fonts_(fonts) {
    frame_ = make_chip_frame(root, kJackTheme, fonts_);
    body_ = layer(frame_.body, 0, 0, 320, 139);
    pills_ = layer(frame_.screen, 0, 0, 320, 16);
}

void JackInView::render(const viewmodel::ChipModule& chip) {
    const auto& jack = static_cast<const JackInChip&>(chip);
    lv_obj_clean(body_);
    lv_obj_clean(pills_);
    lv_label_set_text(frame_.tag, "");
    switch (jack.view()) {
    case JackInChip::View::Waiting: render_waiting(jack); break;
    case JackInChip::View::Session: render_session(jack); break;
    case JackInChip::View::Log: render_log(jack); break;
    }
    lv_obj_align(frame_.tag, LV_ALIGN_TOP_RIGHT, -6, 2);
}

int JackInView::wrapped(const std::string& value, uint32_t color, int x, int y, int w, int max_h) {
    lv_obj_t* label = text(body_, value, fonts_.small, color, x, y);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, w);
    lv_obj_set_style_text_line_space(label, 1, 0);
    lv_obj_update_layout(label);
    if (max_h > 0 && lv_obj_get_height(label) > max_h) {
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_obj_set_height(label, max_h);
        return y + max_h;  // the layout isn't refreshed yet, so don't read the height back
    }
    return y + lv_obj_get_height(label);
}

void JackInView::pills(const JackInChip& jack) {
    // Right to left: PC net, ADMIN, FULL / READ-ONLY.
    struct Pill {
        std::string label;
        uint32_t bg;
        uint32_t ink;
    };
    std::vector<Pill> list;
    if (jack.sharing()) list.push_back({"PC net", kEdgeBlue, kWhite});
    if (jack.admin()) list.push_back({"ADMIN", kOrange, 0x1a0f00});
    list.push_back(jack.full() ? Pill{"FULL", kGreen, 0x000000} : Pill{"READ-ONLY", kYellow, 0x3a1f00});
    int right = 316;
    for (const auto& p : list) {
        const int w = static_cast<int>(p.label.size()) * kGlyphW + 6;
        right -= w;
        lv_obj_t* pill = box(pills_, right, 2, w, 12, p.bg);
        lv_obj_set_style_radius(pill, 3, 0);
        text(pill, p.label, fonts_.small, p.ink, 3, 0);
        right -= 3;
    }
}

void JackInView::render_waiting(const JackInChip& jack) {
    lv_label_set_text(frame_.title, "JACK IN");
    lv_label_set_text(frame_.tag, jack.usb_ok() ? "USB ready" : "USB ...");
    lv_obj_set_style_text_color(frame_.tag, lv_color_hex(jack.usb_ok() ? kGreen : kYellow), 0);
    int y = 4;
    const char* steps[] = {"Plug the PET into the PC (USB-C).", "Open the ALICEPET drive, run JackIn.cmd.",
                           "It starts read-only. Fixes need you to", "click Full Access on the PC."};
    const char* nums[] = {"1", "2", "3", " "};
    for (int i = 0; i < 4; ++i) {
        text(body_, nums[i], fonts_.small, kGold, kLeft, y);
        text(body_, steps[i], fonts_.small, kWhite, kLeft + 2 * kGlyphW, y);
        y += kRowH;
    }
    y += 6;
    text(body_, clip_cols("Waiting for the PC on " + jack.listen_address() + "...", kCols), fonts_.small, kLilac,
         kLeft, y);
    y += kRowH;
    if (!jack.usb_status().empty()) {
        text(body_, clip_cols(jack.usb_status(), kCols), fonts_.small, jack.usb_ok() ? kDim : kYellow, kLeft, y);
        y += kRowH;
    }
    if (!jack.status().empty()) wrapped(jack.status(), kGreen, kLeft, y + 4, 304, 139 - y - 6);
    lv_label_set_text(frame_.footer, "[L]log [ESC]chips");
}

void JackInView::render_session(const JackInChip& jack) {
    lv_label_set_text(frame_.title, clip_cols("JACK IN · " + jack.host(), 26).c_str());
    pills(jack);

    // Host line: "Win 11 · front-desk · admin: requested..."
    int y = 3;
    Row host{body_, fonts_.small, kLeft, y};
    std::string desc = jack.os().empty() ? "PC" : jack.os();
    if (!jack.user().empty()) desc += " · " + jack.user();
    host.add(clip_cols(desc, 26), kWhite);
    if (jack.asked_admin()) host.add(" · admin: requested...", kYellow);
    else if (jack.asked_full()) host.add(" · full: requested...", kYellow);
    else if (jack.asked_net()) host.add(" · net: requested...", kYellow);
    y += kRowH;

    // Scan: CPU / RAM / disk, then startup + Defender, coloured by how bad.
    const auto& scan = jack.scan();
    Row stats{body_, fonts_.small, kLeft, y};
    if (!scan.have) {
        stats.add("scanning the PC (read-only)...", kLilac);
    } else {
        if (scan.cpu >= 0) {
            stats.add("CPU ", kWhite);
            stats.add(std::to_string(scan.cpu) + "%", level_color(scan.cpu, 70, 90));
        }
        if (scan.ram >= 0) {
            stats.add(scan.cpu >= 0 ? " · RAM " : "RAM ", kWhite);
            stats.add(std::to_string(scan.ram) + "%", level_color(scan.ram, 75, 90));
        }
        if (scan.disk_free >= 0) {
            stats.add(" · " + scan.disk + " ", kWhite);
            stats.add(std::to_string(scan.disk_free) + "% free", level_color(100 - scan.disk_free, 80, 90));
        }
        y += kRowH;
        Row more{body_, fonts_.small, kLeft, y};
        if (scan.startup >= 0) {
            more.add("Startup: ", kWhite);
            more.add(std::to_string(scan.startup), scan.startup > 10 ? kRed : scan.startup > 6 ? kYellow : kGreen);
            more.add(" · ", kWhite);
        }
        more.add("Defender: ", kWhite);
        const bool on = scan.defender == "on";
        more.add(on ? "on" : scan.defender == "off" ? "OFF" : scan.defender, on ? kGreen : kRed);
    }
    y += kRowH + 2;

    // The last few commands, newest last (two when a fix needs the room).
    const bool has_fix = !jack.suggestion().empty();
    const size_t keep = has_fix ? 2 : 3;
    const auto& log = jack.log();
    std::vector<const JackInChip::LogLine*> recent;
    for (auto it = log.rbegin(); it != log.rend() && recent.size() < keep; ++it) {
        if (it->text.rfind("> ", 0) == 0) recent.insert(recent.begin(), &*it);
    }
    for (const auto* line : recent) {
        text(body_, clip_cols(log_text(*line), kCols), fonts_.small, log_color(line->kind), kLeft, y);
        y += 12;
    }
    y += 3;

    // Alice, then her suggested fix in a box.
    const int bottom = jack.status().empty() ? 139 : 139 - kRowH;
    if (jack.alice_waiting()) {
        y = wrapped("Alice is reading the scan...", kPink, kLeft, y, 304, 0);
    } else if (!jack.alice_text().empty()) {
        y = wrapped("Alice: " + jack.alice_text(), kPink, kLeft, y, 304, bottom - y - (has_fix ? 20 : 2));
    }
    if (has_fix && y + 16 <= bottom) {
        lv_obj_t* fix = box(body_, kLeft - 2, y + 2, 308, 15, 0x120a1c);
        set_border(fix, 1, jack.full() ? kGreen : kDim);
        lv_obj_set_style_radius(fix, 3, 0);
        text(fix, clip_cols("> " + jack.suggestion(), 42), fonts_.small, jack.full() ? kGreen : kLilac, 3, 1);
    }
    if (!jack.status().empty()) {
        text(body_, clip_cols(jack.status(), kCols), fonts_.small, kYellow, kLeft, 139 - kRowH + 1);
    }
    // [K] is shown as what it would do next: allow the PC to drive chips, or take that back.
    const char* k = jack.pc_control() && jack.pc_wants().empty() ? "[K]pc off" : "[K]pc on";
    const std::string footer = has_fix ? std::string("[↵]fix [F]full [A]adm ") + k + " [L]log [X]OUT"
                                       : std::string("[F]full [A]adm [N]net ") + k + " [L]log [X]OUT";
    lv_label_set_text(frame_.footer, footer.c_str());
}

void JackInView::render_log(const JackInChip& jack) {
    lv_label_set_text(frame_.title, "JACK IN · SESSION LOG");
    const auto& log = jack.log();
    lv_label_set_text_fmt(frame_.tag, "%zu lines", log.size());
    lv_obj_set_style_text_color(frame_.tag, lv_color_hex(kLilac), 0);
    lv_label_set_text(frame_.footer, "[↑↓]scroll [L]/[ESC]back");
    if (log.empty()) {
        text(body_, "Nothing yet.", fonts_.small, kDim, kLeft, 6);
        return;
    }
    constexpr size_t kRows = 10;
    const size_t end = log.size() - std::min(jack.log_scroll(), log.size() - 1);
    const size_t start = end > kRows ? end - kRows : 0;
    int y = 3;
    for (size_t i = start; i < end; ++i) {
        text(body_, clip_cols(log_text(log[i]), kCols), fonts_.small, log_color(log[i].kind), kLeft, y);
        y += 13;
    }
}

} // namespace view
