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
    if (!lan.running() && !lan.verdict().empty()) {
        const std::string verdict = lan.verdict_ok() ? "Verdict: " + lan.verdict() : lan.verdict();
        wrapped(verdict, lan.verdict_ok() ? kLanTheme.text : kWhite, y + 2, 139 - y - 4);
    }
    lv_label_set_text(frame_.footer, lan.running() ? "testing the jack... [ESC]chips"
                                                   : "[R]retest [D]devices [A]ask Alice");
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
    lv_label_set_text(frame_.footer, "[R]retest [A]ask again [ESC]back");
}

void LanTechView::render_devices(const LanTechChip& lan) {
    // Screen 7: who's on this /24, one row each, the selected row highlighted.
    lv_label_set_text(frame_.title, "LAN · DEVICES");
    const auto& devices = lan.devices();
    if (lan.sweeping()) {
        lv_label_set_text(frame_.tag, "scanning...");
    } else {
        lv_label_set_text_fmt(frame_.tag, "%zu found", devices.size());
    }
    lv_obj_set_style_text_color(frame_.tag, lv_color_hex(kLanTheme.text), 0);
    lv_label_set_text(frame_.footer, "[↑↓]scroll [R]rescan [ESC]back");

    if (devices.empty()) {
        wrapped(lan.sweeping() ? "Knocking on every address in this subnet (about 10 s)..."
                               : "Nothing answered. Press R to scan again.",
                kLanTheme.text, 6, 0);
        return;
    }
    const size_t selected = lan.scroll();
    const size_t first = selected >= kVisibleDevices ? selected - kVisibleDevices + 1 : 0;
    int y = 2;
    for (size_t i = first; i < devices.size() && i < first + kVisibleDevices; ++i) {
        const auto& d = devices[i];
        const bool sel = i == selected;
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
            text(body_, clip_cols(d.mac, 17), fonts_.small, sel ? kWhite : 0x6fbf8c, kLeft + 24 * kGlyphW, y);
        }
        y += kDevRowH;
    }
    if (first + kVisibleDevices < devices.size()) {
        text(body_, "+" + std::to_string(devices.size() - first - kVisibleDevices) + " more ▼", fonts_.small,
             kLanTheme.text, kLeft, y);
    }
}

} // namespace view
