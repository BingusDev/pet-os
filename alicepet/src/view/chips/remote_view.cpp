/*
 * SPDX-License-Identifier: MIT
 */

#include "view/chips/remote_view.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>

namespace view {
namespace {

using viewmodel::chips::RemoteChip;

// mockups/tools.html .rm: red radial, dark-red bars, pink text, red highlight.
constexpr ChipTheme kRemoteTheme{0x4a1418, 0x120405, 0x2a0a0d, 0xe2434b, 0xffb0b0};
constexpr uint32_t kHi = 0xe2434b;
constexpr uint32_t kDim = 0x7a4048;
constexpr int kLeft = 8;
constexpr int kRowH = 15;
constexpr size_t kVisibleRows = 8;

// Pad labels (Departure Mono has no ◀ ▶ ⏻).
constexpr std::array<const char*, RemoteChip::kButtonCount> kPadLabels = {
    "PWR", "INPUT", "HDMI1", "HDMI2", "VOL-", "VOL+", "MUTE", "CH-", "CH+", "▲", "←", "OK", "→", "▼", "BACK"};

std::string upper(std::string value) {
    for (auto& c : value) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return value;
}

} // namespace

RemoteView::RemoteView(lv_obj_t* root, const AliceFonts& fonts) : fonts_(fonts) {
    frame_ = make_chip_frame(root, kRemoteTheme, fonts_);
    body_ = layer(frame_.body, 0, 0, 320, 139);
}

void RemoteView::render(const viewmodel::ChipModule& chip) {
    const auto& remote = static_cast<const RemoteChip&>(chip);
    lv_obj_clean(body_);
    lv_obj_set_style_text_color(frame_.tag, lv_color_hex(kRemoteTheme.text), 0);
    switch (remote.view()) {
    case RemoteChip::View::Devices: render_devices(remote); break;
    case RemoteChip::View::Name: render_name(remote); break;
    case RemoteChip::View::Pad: render_pad(remote); break;
    case RemoteChip::View::Learn: render_learn(remote); break;
    case RemoteChip::View::Macros: render_macros(remote); break;
    case RemoteChip::View::MacroRun: render_macro_run(remote); break;
    }
    lv_obj_align(frame_.tag, LV_ALIGN_TOP_RIGHT, -6, 2);
}

int RemoteView::wrapped(const std::string& value, uint32_t color, int x, int y, int width) {
    lv_obj_t* label = text(body_, value, fonts_.small, color, x, y);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, width);
    lv_obj_set_style_text_line_space(label, 1, 0);
    lv_obj_update_layout(label);
    return y + lv_obj_get_height(label);
}

void RemoteView::row(const std::string& left, const std::string& right, int y, bool selected, uint32_t color) {
    if (selected) {
        lv_obj_t* hl = box(body_, 6, y - 1, 308, kRowH - 1, kHi);
        lv_obj_set_style_radius(hl, 3, 0);
    }
    text(body_, clip_cols(left, 34), fonts_.small, selected ? kWhite : color, kLeft + 2, y);
    if (!right.empty()) {
        lv_obj_t* r = text(body_, right, fonts_.small, selected ? kWhite : kDim, 0, y);
        lv_obj_align(r, LV_ALIGN_TOP_RIGHT, -10, y);
    }
}

void RemoteView::render_devices(const RemoteChip& remote) {
    // Screen 1: saved devices, then "Learn new device".
    lv_label_set_text(frame_.title, "REMOTE · DEVICES");
    const auto& devices = remote.devices();
    lv_label_set_text_fmt(frame_.tag, "%zu saved", devices.size());
    lv_label_set_text(frame_.footer, "[↑↓]pick [↵]open [L]learn [M]macros");
    const size_t total = devices.size() + 1;
    const size_t first = remote.selected() >= kVisibleRows ? remote.selected() - kVisibleRows + 1 : 0;
    int y = 4;
    for (size_t i = first; i < total && i < first + kVisibleRows; ++i, y += kRowH) {
        const bool sel = i == remote.selected();
        if (i < devices.size()) {
            const auto& d = devices[i];
            row(d.name, std::to_string(d.learned()) + "/" + std::to_string(RemoteChip::kButtonCount), y, sel, kWhite);
        } else {
            row("+ Learn new device...", "", y, sel, kRemoteTheme.text);
        }
    }
    if (!remote.status().empty()) {
        text(body_, clip_cols(remote.status(), 43), fonts_.small, remote.status_error() ? kRed : kRemoteTheme.text,
             kLeft, 139 - 14);
    }
}

void RemoteView::render_name(const RemoteChip& remote) {
    lv_label_set_text(frame_.title, "REMOTE · NEW DEVICE");
    lv_label_set_text(frame_.tag, "name it");
    lv_label_set_text(frame_.footer, "[↵]start learning [ESC]cancel");
    int y = wrapped("What is this remote for? e.g. Store TV, Projector, Wall AC", kRemoteTheme.text, kLeft, 8, 304);
    lv_obj_t* field = box(body_, kLeft, y + 8, 304, 20, kRemoteTheme.bar);
    set_border(field, 1, kHi);
    lv_obj_set_style_radius(field, 3, 0);
    text(field, clip_cols(remote.name_input(), 40) + "_", fonts_.small, kWhite, 5, 3);
    wrapped("Next, the PET asks for each button in turn: point the real remote at it and press that button.",
            kDim, kLeft, y + 38, 304);
}

void RemoteView::render_pad(const RemoteChip& remote) {
    // Screen 2: the 4/5/6 button grid; the selected button is filled red.
    const auto* device = remote.device();
    if (!device) return;
    lv_label_set_text(frame_.title, clip_cols(upper(device->name), 30).c_str());
    lv_label_set_text(frame_.tag, remote.sending() ? "IR · sending" : "IR · ready");
    lv_label_set_text(frame_.footer, "[↵]send [L]learn [ESC]back · aim top edge");
    size_t index = 0;
    int y = 6;
    for (size_t r = 0; r < RemoteChip::kRowSizes.size(); ++r) {
        const int n = static_cast<int>(RemoteChip::kRowSizes[r]);
        const int w = (304 - (n - 1) * 4) / n;
        for (int c = 0; c < n; ++c, ++index) {
            const bool sel = index == remote.button();
            const bool learned = !device->codes[index].empty();
            lv_obj_t* b = box(body_, kLeft + c * (w + 4), y, w, 22, sel ? kHi : kRemoteTheme.bg_bottom);
            set_border(b, 1, sel ? kHi : (learned ? kRemoteTheme.text : kDim));
            lv_obj_set_style_radius(b, 4, 0);
            lv_obj_t* t = text(b, kPadLabels[index], fonts_.small, sel ? kWhite : (learned ? kRemoteTheme.text : kDim), 0, 0);
            lv_obj_align(t, LV_ALIGN_CENTER, 0, 0);
        }
        y += 27;
    }
    const std::string status = remote.status().empty() ? std::to_string(device->learned()) + " of 15 buttons learned"
                                                       : remote.status();
    wrapped(status, remote.status_error() ? kRed : kRemoteTheme.text, kLeft, y + 6, 304);
}

void RemoteView::render_learn(const RemoteChip& remote) {
    // Screen 3: ask for one button, show the live capture bar, then the result.
    const auto* device = remote.device();
    if (!device) return;
    const size_t b = remote.button();
    const auto state = remote.learn_state();
    lv_label_set_text(frame_.title, ("LEARN · " + clip_cols(upper(device->name), 28)).c_str());
    lv_label_set_text(frame_.tag, state == RemoteChip::LearnState::Listening  ? "listening"
                                  : state == RemoteChip::LearnState::Captured ? "captured"
                                                                              : "no signal");
    lv_obj_set_style_text_color(frame_.tag, lv_color_hex(state == RemoteChip::LearnState::Listening  ? kYellow
                                                         : state == RemoteChip::LearnState::Captured ? kGreen
                                                                                                     : kRed),
                                0);
    int y = wrapped("Point the real remote at the PET's IR eye and press:", kWhite, kLeft + 4, 6, 300);
    text(body_, RemoteChip::kButtons[b], fonts_.small, kYellow, kLeft + 4, y + 2);
    y += 18;

    // Capture bar: stripes scroll while listening, solid once captured.
    lv_obj_t* bar = box(body_, kLeft + 4, y, 296, 20, kRemoteTheme.bg_bottom);
    set_border(bar, 1, kHi);
    lv_obj_set_style_radius(bar, 3, 0);
    lv_obj_set_style_clip_corner(bar, true, 0);
    if (state != RemoteChip::LearnState::Failed) {
        const int shift = state == RemoteChip::LearnState::Listening ? static_cast<int>(remote.learn_ms() / 60 % 11) : 0;
        for (int x = -11 + shift; x < 296; x += 11) {
            if (x + 6 <= 0) continue;
            box(bar, std::max(0, x), 3, state == RemoteChip::LearnState::Captured ? 3 : 2, 12, kHi);
            if (state == RemoteChip::LearnState::Captured && x + 5 < 294) box(bar, x + 5, 3, 1, 12, kHi);
        }
    }
    y += 26;
    if (state == RemoteChip::LearnState::Captured) {
        text(body_, clip_cols("Captured " + remote.learn_detail() + " · saved as " + RemoteChip::kButtons[b], 42),
             fonts_.small, kGreen, kLeft + 4, y);
    } else if (state == RemoteChip::LearnState::Failed) {
        text(body_, clip_cols(remote.learn_detail(), 42), fonts_.small, kRed, kLeft + 4, y);
    } else {
        text(body_, "waiting for a signal...", fonts_.small, kDim, kLeft + 4, y);
    }
    if (b + 1 < RemoteChip::kButtonCount) {
        text(body_,
             std::string("Next: ") + RemoteChip::kButtons[b + 1] + " (" + std::to_string(b + 2) + "/" +
                 std::to_string(RemoteChip::kButtonCount) + ")",
             fonts_.small, kRemoteTheme.text, kLeft + 4, y + 15);
    } else {
        text(body_, "Last button: Enter saves this remote", fonts_.small, kRemoteTheme.text, kLeft + 4, y + 15);
    }
    lv_label_set_text(frame_.footer, state == RemoteChip::LearnState::Failed ? "[R]retry [S]skip [ESC]done"
                                                                             : "[↵]next [S]skip [T]test-fire [ESC]done");
}

void RemoteView::render_macros(const RemoteChip& remote) {
    lv_label_set_text(frame_.title, "REMOTE · MACROS");
    const auto& macros = remote.macros();
    lv_label_set_text_fmt(frame_.tag, "%zu saved", macros.size());
    lv_label_set_text(frame_.footer, "[↑↓]pick [↵]run [ESC]back");
    if (macros.empty()) {
        int y = wrapped("No macros yet. Add a .macro file in " + remote.dir() + ":", kRemoteTheme.text, kLeft, 6, 304);
        wrapped("name=Movie night\nstep=Living-room TV|POWER\nstep=wait|5\nstep=Living-room TV|HDMI2", kWhite, kLeft + 8,
                y + 6, 296);
        return;
    }
    int y = 4;
    for (size_t i = 0; i < macros.size() && i < kVisibleRows; ++i, y += kRowH) {
        row(macros[i].name, std::to_string(macros[i].steps.size()) + " steps", y, i == remote.macro(), kWhite);
    }
}

void RemoteView::render_macro_run(const RemoteChip& remote) {
    // Screen 4: the steps, the running one highlighted, and a live status line.
    const auto& macro = remote.macros()[remote.macro()];
    lv_label_set_text(frame_.title, ("MACRO · \"" + clip_cols(upper(macro.name), 24) + "\"").c_str());
    lv_label_set_text_fmt(frame_.tag, "%zu steps", macro.steps.size());
    const size_t current = remote.macro_step();
    constexpr size_t kShown = 6;
    const size_t first = current >= kShown ? current - kShown + 1 : 0;
    int y = 4;
    for (size_t i = first; i < macro.steps.size() && i < first + kShown; ++i, y += kRowH) {
        const auto& step = macro.steps[i];
        const std::string what = step.device == "wait" ? "wait " + std::to_string(step.wait_s) + "s"
                                                       : step.device + " " + step.button;
        const bool done = i < current || remote.macro_done();
        text(body_, clip_cols(std::to_string(i + 1) + " · " + what, 42), fonts_.small,
             i == current && !remote.macro_done() ? kWhite : (done ? kDim : kRemoteTheme.text), kLeft, y);
    }
    std::string status;
    uint32_t color = kGreen;
    if (remote.macro_done()) {
        status = remote.status_error() ? remote.status() : "Done. Whole room set up.";
        color = remote.status_error() ? kRed : kGreen;
    } else if (current < macro.steps.size() && macro.steps[current].device == "wait") {
        status = "→ Running step " + std::to_string(current + 1) + " ... " + std::to_string(remote.macro_wait_left_s()) + "s";
    } else {
        status = "→ Running step " + std::to_string(current + 1) + " ...";
    }
    text(body_, clip_cols(status, 43), fonts_.small, color, kLeft, std::max(y + 6, 100));
    lv_label_set_text(frame_.footer, remote.macro_done() ? "[↵]back · one key = whole room set up" : "[ESC]stop");
}

} // namespace view
