/*
 * SPDX-License-Identifier: MIT
 */

#include "view/chips/lan_tech_view.h"

#include <string>

namespace view {
namespace {

using viewmodel::chips::LanTechChip;

// mockups/tools.html .ln: green radial, dark-green bars, mint text.
constexpr ChipTheme kLanTheme{0x0f3a22, 0x03110a, 0x06200f, 0x2fa35a, 0x9dffc0};
constexpr uint32_t kRowInk = 0xd8ffe6;
constexpr uint32_t kSel = 0x1f6b3c;
constexpr int kLeft = 8;
constexpr int kWidth = 304;
constexpr int kRowH = 14;
constexpr int kDevRowH = 13;
constexpr size_t kVisibleDevices = 9;
constexpr size_t kLabelCols = 13;  // "Switch port ."
constexpr size_t kValueCols = 27;

uint32_t state_color(LanTechChip::State state) {
    switch (state) {
    case LanTechChip::State::Ok: return kGreen;
    case LanTechChip::State::Warn: return kYellow;
    case LanTechChip::State::Fail: return kRed;
    case LanTechChip::State::Pending: break;
    }
    return 0x5d8a6c;
}

std::string last_octet(const std::string& ip) {
    const auto dot = ip.rfind('.');
    return dot == std::string::npos ? ip : ip.substr(dot);
}

} // namespace

LanTechView::LanTechView(lv_obj_t* root, const AliceFonts& fonts, const std::filesystem::path& art_dir)
    : fonts_(fonts) {
    frame_ = make_chip_frame(root, kLanTheme, fonts_);
    load_png(art_dir / "icon-check.png", check_);
    body_ = layer(frame_.body, 0, 0, 320, 139);
}

void LanTechView::render(const viewmodel::ChipModule& chip) {
    const auto& lan = static_cast<const LanTechChip&>(chip);
    lv_obj_clean(body_);
    switch (lan.view()) {
    case LanTechChip::View::Test: render_test(lan); break;
    case LanTechChip::View::Alice: render_alice(lan); break;
    case LanTechChip::View::Devices: render_devices(lan); break;
    case LanTechChip::View::Device: render_device(lan); break;
    }
    lv_obj_align(frame_.tag, LV_ALIGN_TOP_RIGHT, -6, 2);
}

int LanTechView::check_row(const LanTechChip::Check& check, int y) {
    // Mark: the green check icon for OK, a red x for a failure, a yellow square
    // for a warning, a dim dot while pending (Departure Mono has no ✔/✘/●).
    if (check.state == LanTechChip::State::Ok && check_.loaded) {
        image(body_, check_, kLeft + 1, y + 4);
    } else {
        const char* mark = check.state == LanTechChip::State::Fail   ? "×"
                           : check.state == LanTechChip::State::Warn ? "■"
                                                                     : "·";
        text(body_, mark, fonts_.small, state_color(check.state), kLeft, y);
    }
    text(body_, dot_leader(check.label, kLabelCols), fonts_.small, kRowInk, kLeft + 11, y);
    text(body_, clip_cols(check.value, kValueCols), fonts_.small,
         check.state == LanTechChip::State::Fail ? kRed : kRowInk,
         kLeft + 11 + static_cast<int>(kLabelCols + 1) * kGlyphW, y);
    return y + kRowH;
}

int LanTechView::wrapped(const std::string& value, uint32_t color, int y, int max_h) {
    lv_obj_t* label = text(body_, value, fonts_.small, color, kLeft, y);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, kWidth);
    lv_obj_set_style_text_line_space(label, 1, 0);
    lv_obj_update_layout(label);
    if (max_h > 0 && lv_obj_get_height(label) > max_h) {
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_obj_set_height(label, max_h);
        return y + max_h;  // the layout isn't refreshed yet, so don't read the height back
    }
    return y + lv_obj_get_height(label);
}

void LanTechView::set_headline(const LanTechChip& lan) {
    uint32_t color = kYellow;
    if (!lan.running()) color = lan.verdict_ok() ? kGreen : kRed;
    lv_label_set_text(frame_.tag, lan.running() ? "testing..." : lan.headline().c_str());
    lv_obj_set_style_text_color(frame_.tag, lv_color_hex(color), 0);
}

void LanTechView::render_test(const LanTechChip& lan) {
    // Screen 5: one line per check, then the verdict.
    lv_label_set_text(frame_.title, "LAN TECH · JACK TEST");
    set_headline(lan);
    int y = 3;
    for (const auto& check : lan.checks()) y = check_row(check, y);
    const bool has_status = !lan.status().empty();
    const int bottom = has_status ? 139 - 14 : 139;
    if (!lan.running() && !lan.verdict().empty()) {
        const std::string verdict = lan.verdict_ok() ? "Verdict: " + lan.verdict() : lan.verdict();
        wrapped(verdict, lan.verdict_ok() ? kLanTheme.text : kWhite, y + 2, bottom - y - 4);
    }
    if (has_status) text(body_, clip_cols(lan.status(), 43), fonts_.small, kYellow, kLeft, bottom);
    if (lan.running()) {
        lv_label_set_text(frame_.footer, "testing the jack... [ESC]chips");
    } else if (lan.can_try_static()) {
        lv_label_set_text(frame_.footer, "[R]retest [V]try static IP [A]ask Alice");
    } else {
        lv_label_set_text(frame_.footer, "[R]retest [D]devices [A]Alice [S]save");
    }
}

void LanTechView::render_alice(const LanTechChip& lan) {
    // Screen 6: the checks that matter, then Alice's plain-English diagnosis.
    lv_label_set_text(frame_.title, "LAN · DIAGNOSIS");
    set_headline(lan);
    int y = 3;
    const auto& checks = lan.checks();
    for (size_t i = 0; i < checks.size() && i < 3; ++i) y = check_row(checks[i], y);
    y += 4;
    if (lan.alice_waiting()) {
        wrapped("Alice is reading the results...", kLanTheme.text, y, 0);
    } else {
        wrapped("Alice: " + lan.alice_text(), kWhite, y, 139 - y - 2);
    }
    if (!lan.status().empty()) text(body_, clip_cols(lan.status(), 43), fonts_.small, kYellow, kLeft, 139 - 14);
    lv_label_set_text(frame_.footer, "[R]retest [A]ask again [S]save [ESC]back");
}

void LanTechView::render_devices(const LanTechChip& lan) {
    // Screen 7: who's on this /24, one row each, the selected row highlighted;
    // devices first seen in the last day are marked NEW ([W] shows only those).
    lv_label_set_text(frame_.title, lan.new_only() ? "LAN · WHO'S NEW" : "LAN · DEVICES");
    const auto devices = lan.devices();
    if (lan.sweeping()) {
        lv_label_set_text(frame_.tag, "scanning...");
    } else if (lan.new_count() > 0) {
        lv_label_set_text_fmt(frame_.tag, "%zu found · %zu new", lan.device_count(), lan.new_count());
    } else {
        lv_label_set_text_fmt(frame_.tag, "%zu found", lan.device_count());
    }
    lv_obj_set_style_text_color(frame_.tag, lv_color_hex(lan.new_count() > 0 ? kYellow : kLanTheme.text), 0);
    lv_label_set_text(frame_.footer, lan.new_only() ? "[↑↓]scroll [↵]ping/ports [W]all [ESC]back"
                                                    : "[↑↓]scroll [↵]ping/ports [W]who's new");

    if (devices.empty()) {
        std::string msg = lan.sweeping() ? "Knocking on every address in this subnet (about 10 s)..."
                                         : "Nothing answered. Press R to scan again.";
        if (!lan.sweeping() && lan.new_only()) {
            msg = lan.baseline() ? "First scan of this network: everything here is now remembered, so new devices "
                                   "will stand out next time."
                                 : "Nothing new in the last day.";
        }
        wrapped(msg, kLanTheme.text, 6, 0);
        return;
    }
    const size_t selected = lan.scroll();
    const size_t first = selected >= kVisibleDevices ? selected - kVisibleDevices + 1 : 0;
    int y = 2;
    for (size_t i = first; i < devices.size() && i < first + kVisibleDevices; ++i) {
        const auto& d = *devices[i];
        const bool sel = i == selected;
        tap_target(body_, 4, y - 1, 312, kDevRowH, static_cast<int>(i) - static_cast<int>(selected), false);
        if (sel) {
            lv_obj_t* hl = box(body_, 4, y - 1, 312, kDevRowH, kSel);
            lv_obj_set_style_radius(hl, 3, 0);
        }
        const uint32_t ink = d.self ? kLanTheme.text : (sel ? kWhite : kRowInk);
        std::string octet = last_octet(d.ip);
        octet.resize(5, ' ');
        text(body_, octet, fonts_.small, ink, kLeft, y);
        if (d.self) {
            text(body_, "← this PET", fonts_.small, ink, kLeft + 5 * kGlyphW, y);
        } else {
            text(body_, dot_leader(d.name.empty() ? "?" : d.name, 18), fonts_.small, ink, kLeft + 5 * kGlyphW, y);
            if (d.is_new) {
                text(body_, "NEW", fonts_.small, kYellow, kLeft + 24 * kGlyphW, y);
                text(body_, clip_cols(d.mac.size() > 8 ? d.mac.substr(9) : d.mac, 13), fonts_.small,
                     sel ? kWhite : 0x6fbf8c, kLeft + 28 * kGlyphW, y);
            } else {
                text(body_, clip_cols(d.mac, 17), fonts_.small, sel ? kWhite : 0x6fbf8c, kLeft + 24 * kGlyphW, y);
            }
        }
        y += kDevRowH;
    }
    if (first + kVisibleDevices < devices.size()) {
        text(body_, "+" + std::to_string(devices.size() - first - kVisibleDevices) + " more ▼", fonts_.small,
             kLanTheme.text, kLeft, y);
    }
}

void LanTechView::render_device(const LanTechChip& lan) {
    // One device: name, MAC, ping, and which common ports answer.
    const auto& d = lan.detail();
    lv_label_set_text(frame_.title, ("LAN · " + d.ip).c_str());
    lv_label_set_text(frame_.tag, lan.probing() ? "checking..." : (d.is_new ? "NEW" : ""));
    lv_obj_set_style_text_color(frame_.tag, lv_color_hex(d.is_new ? kYellow : kLanTheme.text), 0);
    lv_label_set_text(frame_.footer, "[R]check again [ESC]devices");
    int y = 3;
    text(body_, clip_cols(d.name.empty() ? "(no name)" : d.name, 43), fonts_.small, kWhite, kLeft, y);
    y += 14;
    text(body_, "MAC  " + (d.mac.empty() ? std::string("-") : d.mac), fonts_.small, kRowInk, kLeft, y);
    y += 14;
    std::string ping = "Ping " + std::string(lan.probing() ? "..." : lan.detail_ping_ms() < 0
                                                                     ? "no reply (may block ping)"
                                                                     : std::to_string(static_cast<int>(lan.detail_ping_ms() + 0.5)) + " ms");
    text(body_, ping, fonts_.small, lan.detail_ping_ms() < 0 && !lan.probing() ? kYellow : kRowInk, kLeft, y);
    y += 18;
    text(body_, "Open ports:", fonts_.small, kLanTheme.text, kLeft, y);
    y += 14;
    if (lan.probing()) {
        text(body_, "checking " + std::to_string(LanTechChip::common_ports().size()) + " common ports...", fonts_.small,
             kRowInk, kLeft, y);
        return;
    }
    if (lan.detail_open().empty()) {
        text(body_, "none of the common ones", fonts_.small, kRowInk, kLeft, y);
        return;
    }
    std::string list;
    for (int port : lan.detail_open()) {
        const char* service = "";
        for (const auto& p : LanTechChip::common_ports()) {
            if (p.port == port) service = p.service;
        }
        if (!list.empty()) list += "  ";
        list += std::to_string(port) + " " + service;
    }
    wrapped(list, kGreen, y, 139 - y - 2);
}

} // namespace view
