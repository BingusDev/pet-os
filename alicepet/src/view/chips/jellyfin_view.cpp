/*
 * SPDX-License-Identifier: MIT
 */

#include "view/chips/jellyfin_view.h"

#include "viewmodel/alice_viewmodel.h"

#include <algorithm>
#include <cstdio>
#include <string>

namespace view {
namespace {

using viewmodel::chips::JellyfinChip;

constexpr ChipTheme kJfTheme{0x3a1a5e, 0x0d0618, 0x1a0d2e, 0xaa5cc3, 0xe0b8ff};
constexpr uint32_t kSel = 0xaa5cc3;
constexpr uint32_t kText = 0xe8dcff;
// Poster tiles cycle through the mockup's colours (top, bottom).
constexpr uint32_t kTiles[][2] = {
    {0x2f7de0, 0x123a8a}, {0x2fa35a, 0x145230}, {0x8a3ad8, 0x3e1470}, {0xe0484f, 0x7a1a20}, {0xe0a21a, 0x7a520a},
};
constexpr int kTileW = 58;
constexpr int kTileH = 84;
constexpr int kTileGap = 4;
constexpr size_t kTileCount = 5;
constexpr size_t kEpisodeRows = 8;

std::string episode_label(const JellyfinChip::Item& item) {
    char head[24];
    std::snprintf(head, sizeof(head), "%02d ", item.index);
    return head + item.name;
}

} // namespace

JellyfinView::JellyfinView(lv_obj_t* root, const AliceFonts& fonts, const std::filesystem::path& art_dir)
    : fonts_(fonts) {
    frame_ = make_chip_frame(root, kJfTheme, fonts_);
    load_png(art_dir / "icon-check.png", check_);
    load_png(art_dir / "icon-play.png", play_);
}

void JellyfinView::render(const viewmodel::ChipModule& chip) {
    const auto& jf = static_cast<const JellyfinChip&>(chip);
    lv_obj_clean(frame_.body);
    lv_obj_set_style_text_color(frame_.tag, lv_color_hex(kJfTheme.text), 0);
    switch (jf.view()) {
    case JellyfinChip::View::Loading:
    case JellyfinChip::View::Error: {
        lv_label_set_text(frame_.title, "JELLYFIN");
        lv_label_set_text(frame_.tag, jf.view() == JellyfinChip::View::Error ? "offline" : "...");
        lv_obj_t* msg = text(frame_.body, jf.status(), fonts_.small,
                             jf.view() == JellyfinChip::View::Error ? kRed : kText, 0, 0);
        lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(msg, 290);
        lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(msg, LV_ALIGN_CENTER, 0, 0);
        lv_label_set_text(frame_.footer, jf.view() == JellyfinChip::View::Error ? "[↵]retry [ESC]back" : "[ESC]back");
        break;
    }
    case JellyfinChip::View::Library: render_library(jf); break;
    case JellyfinChip::View::Episodes: render_episodes(jf); break;
    case JellyfinChip::View::Resume: render_resume(jf); break;
    case JellyfinChip::View::Playing: render_playing(jf); break;
    }
    lv_obj_align(frame_.tag, LV_ALIGN_TOP_RIGHT, -6, 2);
}

void JellyfinView::render_library(const JellyfinChip& jf) {
    const bool shows = jf.kind() == JellyfinChip::Kind::Shows;
    lv_label_set_text(frame_.title, shows ? "JELLYFIN · TV SHOWS" : "JELLYFIN · MOVIES");
    lv_label_set_text_fmt(frame_.tag, "%zu %s", jf.library_total(), shows ? "shows" : "movies");
    lv_label_set_text(frame_.footer, shows ? "[←→]browse [↵]open [/]search [TAB]movies" : "[←→]browse [↵]play [/]search [TAB]shows");

    const auto items = jf.library();
    if (items.empty()) {
        text(frame_.body, jf.search().empty() ? "Nothing here yet." : "No match for \"" + jf.search() + "\"", fonts_.small,
             kText, 8, 40);
    }
    const size_t sel = std::min(jf.selected(), items.empty() ? 0 : items.size() - 1);
    size_t first = sel >= 2 ? sel - 2 : 0;
    if (items.size() > kTileCount) first = std::min(first, items.size() - kTileCount);
    else first = 0;
    for (size_t slot = 0; slot < kTileCount && first + slot < items.size(); ++slot) {
        const size_t i = first + slot;
        const auto* item = items[i];
        const int x = 6 + static_cast<int>(slot) * (kTileW + kTileGap);
        const auto& colors = kTiles[i % 5];
        tap_target(frame_.body, x, 4, kTileW, kTileH, static_cast<int>(i) - static_cast<int>(sel), true);
        lv_obj_t* tile = box(frame_.body, x, 4, kTileW, kTileH, colors[0]);
        set_gradient(tile, colors[1]);
        lv_obj_set_style_radius(tile, 4, 0);
        set_border(tile, i == sel ? 2 : 1, i == sel ? kGold : 0x5a4a7a);
        if (i == sel) {
            lv_obj_set_style_shadow_width(tile, 8, 0);
            lv_obj_set_style_shadow_color(tile, lv_color_hex(kGold), 0);
            lv_obj_set_style_shadow_opa(tile, LV_OPA_60, 0);
        }
        // Title, word-wrapped to the tile (8 glyphs), bottom-aligned like a poster caption.
        const auto lines = viewmodel::AliceViewModel::wrap(item->name, 8);
        std::string caption;
        for (size_t l = 0; l < lines.size() && l < 4; ++l) caption += (l ? "\n" : "") + lines[l];
        lv_obj_t* label = text(tile, caption, fonts_.small, kWhite, 0, 0);
        lv_obj_set_width(label, 8 * kGlyphW + 1);
        lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -3);
    }
    if (!items.empty()) {
        const auto* item = items[sel];
        std::string line = item->name;
        if (item->year) line += " · " + std::to_string(item->year);
        if (shows && item->child_count) line += " · " + std::to_string(item->child_count) + (item->child_count == 1 ? " season" : " seasons");
        text(frame_.body, clip_cols(line, 44), fonts_.small, kWhite, 6, 96);
    }
    if (jf.searching() || !jf.search().empty()) {
        text(frame_.body, "/ " + jf.search() + (jf.searching() ? "_" : ""), fonts_.small, kGold, 6, 114);
    } else if (!items.empty()) {
        text(frame_.body, std::to_string(sel + 1) + "/" + std::to_string(items.size()), fonts_.small, kJfTheme.edge, 6, 114);
    }
}

void JellyfinView::render_episodes(const JellyfinChip& jf) {
    const auto* series = jf.series();
    lv_label_set_text(frame_.title, clip_cols(series ? series->name : "", 28).c_str());
    const auto& seasons = jf.seasons();
    lv_label_set_text(frame_.tag, jf.season() < seasons.size() ? ("< " + clip_cols(seasons[jf.season()].name, 12) + " >").c_str() : "");
    lv_label_set_text(frame_.footer, "[↑↓]episode [←→]season [↵]play [ESC]back");

    const auto& eps = jf.episodes();
    if (eps.empty()) {
        text(frame_.body, jf.status().empty() ? "No episodes." : jf.status(), fonts_.small, kText, 8, 6);
        return;
    }
    const size_t first = jf.episode() >= kEpisodeRows ? jf.episode() - kEpisodeRows + 1 : 0;
    for (size_t i = first; i < eps.size() && i < first + kEpisodeRows; ++i) {
        const int y = 2 + static_cast<int>(i - first) * 15;
        const bool selected = i == jf.episode();
        tap_target(frame_.body, 4, y, 312, 14, static_cast<int>(i) - static_cast<int>(jf.episode()), false);
        if (selected) {
            lv_obj_t* hl = box(frame_.body, 4, y, 312, 14, kSel);
            lv_obj_set_style_radius(hl, 3, 0);
        }
        if (eps[i].played) image(frame_.body, check_, 9, y + 4);
        const bool partial = eps[i].position_ticks > 0;
        text(frame_.body, clip_cols(episode_label(eps[i]), partial ? 34 : 41), fonts_.small, selected ? kWhite : kText, 19, y + 1);
        if (partial) {
            lv_obj_t* at = text(frame_.body, JellyfinChip::clock_text(eps[i].position_ticks), fonts_.small, kGold, 0, y + 1);
            lv_obj_align(at, LV_ALIGN_TOP_RIGHT, -8, y + 1);
        }
    }
}

void JellyfinView::render_resume(const JellyfinChip& jf) {
    lv_label_set_text(frame_.title, "RESUME?");
    lv_label_set_text(frame_.tag, "");
    lv_label_set_text(frame_.footer, "progress syncs with every Jellyfin app");
    const auto& item = jf.playing();
    std::string heading = item.name;
    if (item.type == "Episode" && jf.series()) {
        heading = jf.series()->name + " · S" + std::to_string(item.season) + "·E" + std::to_string(item.index);
    }
    text(frame_.body, clip_cols(heading, 42), fonts_.small, kWhite, 12, 16);
    lv_obj_t* lead = text(frame_.body, "You stopped at ", fonts_.small, kText, 12, 34);
    lv_obj_update_layout(lead);
    text(frame_.body, JellyfinChip::clock_text(item.position_ticks), fonts_.small, kGold, 12 + lv_obj_get_width(lead), 34);
    if (item.runtime_ticks > 0) {
        text(frame_.body, "of " + JellyfinChip::clock_text(item.runtime_ticks), fonts_.small, kText, 12, 50);
    }
    const auto button = [&](const char* label, int x, bool selected, bool icon) {
        lv_obj_t* b = box(frame_.body, x, 72, 0, 18, selected ? kSel : kJfTheme.bg_bottom);
        lv_obj_set_size(b, LV_SIZE_CONTENT, 18);
        lv_obj_set_style_pad_left(b, icon ? 15 : 6, 0);
        lv_obj_set_style_pad_right(b, 6, 0);
        set_border(b, 1, selected ? kWhite : kSel);
        lv_obj_set_style_radius(b, 3, 0);
        lv_obj_t* t = text(b, label, fonts_.small, kWhite, 0, 0);
        lv_obj_align(t, LV_ALIGN_LEFT_MID, 0, 0);
        if (icon) {
            lv_obj_t* img = image(b, play_, 0, 0);
            if (img) {
                lv_obj_add_flag(img, LV_OBJ_FLAG_IGNORE_LAYOUT);
                lv_obj_set_pos(img, -10, 4);
            }
        }
        lv_obj_update_layout(b);
        return x + lv_obj_get_width(b) + 8;
    };
    const int x = button("Resume", 12, jf.resume_choice(), true);
    button("Start over", x, !jf.resume_choice(), false);
}

void JellyfinView::render_playing(const JellyfinChip& jf) {
    const auto& item = jf.playing();
    lv_label_set_text(frame_.title, item.type == "Episode"
                                        ? ("S" + std::to_string(item.season) + "·E" + std::to_string(item.index)).c_str()
                                        : clip_cols(item.name, 24).c_str());
    lv_label_set_text(frame_.tag, jf.has_player() ? (jf.paused() ? "paused" : "360p→PET") : "STUB");
    lv_label_set_text(frame_.footer, "[SPACE]pause [←→]±10s [↑↓]vol [ESC]stop");

    // Placeholder "video" area (on the device the player covers the panel).
    lv_obj_t* video = box(frame_.body, 0, 0, 320, 112, 0x2a6fd0);
    set_gradient(video, 0x0d2a66);
    lv_obj_t* msg = text(video, jf.has_player() ? "playing on the PET panel" : "(video plays here on the PET)", fonts_.small, kWhite, 0, 0);
    lv_obj_align(msg, LV_ALIGN_CENTER, 0, -10);
    lv_obj_t* info = text(video, clip_cols(jf.stream_info(), 44), fonts_.small, 0xbfe0ff, 0, 0);
    lv_obj_align(info, LV_ALIGN_CENTER, 0, 8);
    lv_obj_t* name = text(video, clip_cols(item.name, 44), fonts_.small, 0xbfe0ff, 0, 0);
    lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 6);

    const long long runtime = std::max(1LL, item.runtime_ticks);
    const int fill = static_cast<int>(std::clamp(jf.position_ticks() * 300 / runtime, 0LL, 300LL));
    lv_obj_t* track = box(frame_.body, 10, 118, 300, 4, 0x5a4a7a);
    lv_obj_set_style_radius(track, 2, 0);
    lv_obj_t* bar = box(frame_.body, 10, 118, std::max(1, fill), 4, kSel);
    lv_obj_set_style_radius(bar, 2, 0);
    text(frame_.body, JellyfinChip::clock_text(jf.position_ticks()), fonts_.small, kWhite, 10, 124);
    lv_obj_t* total = text(frame_.body, JellyfinChip::clock_text(item.runtime_ticks), fonts_.small, kWhite, 0, 124);
    lv_obj_align(total, LV_ALIGN_TOP_RIGHT, -10, 124);
}

} // namespace view
