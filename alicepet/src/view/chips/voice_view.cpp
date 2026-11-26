/*
 * SPDX-License-Identifier: MIT
 */

#include "view/chips/voice_view.h"
#include "model/navi.h"

#include <cstdio>
#include <string>

namespace view {
namespace {

constexpr ChipTheme kVoiceTheme{0x5a1a3e, 0x14040d, 0x2a0a1d, 0xe85a9a, 0xffb3d6};
constexpr uint32_t kWave = 0xff6fae;
constexpr uint32_t kBubbleBg = 0xffe6f2;
constexpr uint32_t kBubbleInk = 0x3a0a22;
constexpr uint32_t kErrorInk = 0xb3263c;
constexpr int kBubbleX = 116;
constexpr int kBubbleW = 320 - kBubbleX - 6;
constexpr int kWaveX = 124;

using viewmodel::chips::VoiceChip;

} // namespace

VoiceView::VoiceView(lv_obj_t* root, const AliceFonts& fonts, const std::filesystem::path& art_dir)
    : fonts_(fonts) {
    frame_ = make_chip_frame(root, kVoiceTheme, fonts_);
    load_navi_picture(art_dir / "alice-home.png", 110, 110, alice_);
    load_png(art_dir / "icon-mic-11.png", mic_);
    lv_label_set_text(frame_.title, "VOICE");
    lv_obj_t* bar = lv_obj_get_parent(frame_.tag);
    mic_icon_ = image(bar, mic_, 0, 2);

    alice_img_ = image(frame_.body, alice_, 2, 8);

    // Push-to-talk: live mic level bars, status, privacy line.
    wave_ = layer(frame_.body, kWaveX, 6, VoiceChip::kWaveBars * 5, 30);
    for (size_t i = 0; i < bars_.size(); ++i) {
        bars_[i] = box(wave_, static_cast<int>(i) * 5, 13, 3, 4, kWave);
        lv_obj_set_style_radius(bars_[i], 1, 0);
    }
    status_ = text(frame_.body, "", fonts_.small, kWhite, kWaveX, 46);
    hint_ = text(frame_.body, "", fonts_.small, kVoiceTheme.text, kWaveX, 66);
    lv_label_set_long_mode(hint_, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hint_, 320 - kWaveX - 6);
    lv_obj_set_style_text_line_space(hint_, 3, 0);

    // Answer: "You: ..." right-aligned, Alice's reply in a pink bubble.
    you_ = text(frame_.body, "", fonts_.small, kWhite, 0, 3);
    lv_obj_set_width(you_, 320 - 12);
    lv_obj_set_style_text_align(you_, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(you_, LV_LABEL_LONG_CLIP);
    lv_obj_set_x(you_, 6);
    bubble_ = box(frame_.body, kBubbleX, 20, kBubbleW, 6 * 14 + 10, kBubbleBg);
    lv_obj_set_style_radius(bubble_, 7, 0);
    set_border(bubble_, 2, kVoiceTheme.edge);
    bubble_text_ = text(bubble_, "", fonts_.small, kBubbleInk, 6, 3);
    lv_label_set_long_mode(bubble_text_, LV_LABEL_LONG_CLIP);
    lv_obj_set_size(bubble_text_, kBubbleW - 12, 6 * 14 + 2);
    lv_obj_set_style_text_line_space(bubble_text_, 3, 0);
    page_tag_ = box(frame_.body, 0, 0, 10, 13, kVoiceTheme.edge);
    lv_obj_set_style_radius(page_tag_, 3, 0);
    lv_obj_set_size(page_tag_, LV_SIZE_CONTENT, 13);
    lv_obj_set_style_pad_hor(page_tag_, 3, 0);
    text(page_tag_, "", fonts_.small, kWhite, 0, 0);
}

void VoiceView::render(const viewmodel::ChipModule& chip) {
    const auto& voice = static_cast<const VoiceChip&>(chip);
    const auto state = voice.state();
    const bool answer = state == VoiceChip::State::Answer || state == VoiceChip::State::Error;
    const bool live = state == VoiceChip::State::Listening || state == VoiceChip::State::Sending;

    set_visible(wave_, !answer);
    set_visible(status_, !answer);
    set_visible(hint_, !answer);
    set_visible(you_, answer && !voice.transcript().empty());
    set_visible(bubble_, answer);
    lv_obj_set_y(alice_img_, answer ? 26 : 8);

    // Bar tag: mic hint while idle, speaking/answered after a reply.
    if (state == VoiceChip::State::Answer && voice.speaking()) {
        lv_label_set_text(frame_.tag, "speaking");
    } else if (state == VoiceChip::State::Answer) {
        lv_label_set_text(frame_.tag, voice.tts_enabled() ? (voice.muted() ? "muted" : "answered") : "answered");
    } else if (state == VoiceChip::State::Listening) {
        lv_label_set_text(frame_.tag, voice.simulated_mic() ? "REC (wav)" : "REC");
    } else {
        lv_label_set_text(frame_.tag, "hold [SPACE]");
    }
    lv_obj_update_layout(frame_.tag);
    set_visible(mic_icon_, !answer);
    if (mic_icon_) lv_obj_set_x(mic_icon_, lv_obj_get_x(frame_.tag) - 11);

    for (size_t i = 0; i < bars_.size(); ++i) {
        const float level = live && i < voice.levels().size() ? voice.levels()[i] : 0.0f;
        const int h = 3 + static_cast<int>(level * 27.0f);
        lv_obj_set_size(bars_[i], 3, h);
        lv_obj_set_y(bars_[i], 15 - h / 2);
        lv_obj_set_style_bg_opa(bars_[i], live ? LV_OPA_COVER : LV_OPA_40, 0);
    }

    switch (state) {
    case VoiceChip::State::Idle:
        lv_label_set_text(status_, "Hold [SPACE] and talk");
        lv_label_set_text(hint_, "mic is OFF unless you hold the key");
        lv_label_set_text(frame_.footer, "[SPACE]talk [T]type [ESC]chips");
        break;
    case VoiceChip::State::Listening: {
        char line[48];
        std::snprintf(line, sizeof(line), "Listening... %u.%us", voice.listen_ms() / 1000, (voice.listen_ms() / 100) % 10);
        lv_label_set_text(status_, line);
        lv_label_set_text(hint_, "release to send · ESC cancels");
        lv_label_set_text(frame_.footer, "release [SPACE] to send");
        break;
    }
    case VoiceChip::State::Sending: {
        static const char* kDots[] = {"", ".", "..", "..."};
        lv_label_set_text(status_, (model::navi_name() + " is listening" + kDots[voice.thinking_dots() & 3]).c_str());
        lv_label_set_text(hint_, ("transcribing on your computer, then asking " + model::navi_name()).c_str());
        lv_label_set_text(frame_.footer, "mic is off · please wait");
        break;
    }
    case VoiceChip::State::Answer:
    case VoiceChip::State::Error: {
        // 44 glyphs fit across; "You: \"" + text + "\"".
        lv_label_set_text(you_, ("You: \"" + clip_cols(voice.transcript(), 37) + "\"").c_str());
        std::string joined;
        if (state == VoiceChip::State::Error) {
            for (const auto& line : viewmodel::AliceViewModel::wrap(voice.error(), VoiceChip::kBubbleCols)) {
                joined += (joined.empty() ? "" : "\n") + line;
            }
        } else {
            for (const auto& line : voice.reply_page_lines()) joined += (joined.empty() ? "" : "\n") + line;
        }
        lv_label_set_text(bubble_text_, joined.c_str());
        lv_obj_set_style_text_color(bubble_text_, lv_color_hex(state == VoiceChip::State::Error ? kErrorInk : kBubbleInk), 0);
        const size_t pages = state == VoiceChip::State::Answer ? voice.reply_pages() : 1;
        set_visible(page_tag_, pages > 1);
        if (pages > 1) {
            lv_obj_t* label = lv_obj_get_child(page_tag_, 0);
            lv_label_set_text_fmt(label, "%zu/%zu %s", voice.reply_page() + 1, pages,
                                  voice.reply_page() + 1 < pages ? "↓" : "↑");
            lv_obj_update_layout(page_tag_);
            lv_obj_set_pos(page_tag_, kBubbleX + kBubbleW - 8 - lv_obj_get_width(page_tag_), 20 + 6 * 14 + 10 - 4);
        }
        if (state == VoiceChip::State::Error) {
            lv_label_set_text(frame_.footer, "[SPACE]try again [T]type [ESC]chips");
        } else if (voice.tts_enabled()) {
            lv_label_set_text(frame_.footer, "[SPACE]reply [R]replay [M]mute voice");
        } else {
            lv_label_set_text(frame_.footer, "[SPACE]reply [↑↓]page [T]type [ESC]chips");
        }
        break;
    }
    }
    if (!answer) set_visible(page_tag_, false);
}

} // namespace view
