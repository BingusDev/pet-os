/*
 * SPDX-License-Identifier: MIT
 */

#include "view/chips/retro_view.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>

namespace view {
namespace {

using viewmodel::chips::RetroChip;

// mockups/media.html .rt: graphite radial, near-black bars, gold text and selection.
constexpr ChipTheme kRetroTheme{0x2a2f3a, 0x0a0c10, 0x101820, 0xffd200, 0xffd200};
constexpr uint32_t kGoldDim = 0x8a7420;
constexpr uint32_t kInkDark = 0x111111;
constexpr int kLeft = 8;

// Poster gradients (top, bottom) from the mockup; picked by game id.
constexpr std::array<std::array<uint32_t, 2>, 6> kPosterColors = {{
    {0x2fa35a, 0x0d3a1d}, {0x1d6fd8, 0x0a2a6e}, {0xe0a21a, 0x5a3a00},
    {0x7d2ad0, 0x1a0630}, {0x2f6fe0, 0x0a1a5e}, {0xe2434b, 0x5a0d12}}};

std::string upper(std::string value) {
    for (auto& c : value) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return value;
}

std::string label_for(const RetroChip& retro, const std::string& slug) {
    for (const auto& p : retro.platforms()) {
        if (p.slug == slug) return p.label;
    }
    return RetroChip::short_label(slug, slug);
}

} // namespace

RetroView::RetroView(lv_obj_t* root, const AliceFonts& fonts) : fonts_(fonts) {
    frame_ = make_chip_frame(root, kRetroTheme, fonts_);
    body_ = layer(frame_.body, 0, 0, 320, 139);
}

void RetroView::render(const viewmodel::ChipModule& chip) {
    const auto& retro = static_cast<const RetroChip&>(chip);
    lv_obj_clean(body_);
    lv_obj_set_style_text_color(frame_.tag, lv_color_hex(kRetroTheme.text), 0);
    switch (retro.view()) {
    case RetroChip::View::Library: render_library(retro); break;
    case RetroChip::View::Details:
    case RetroChip::View::Downloading: render_details(retro); break;
    case RetroChip::View::Loading:
    case RetroChip::View::Playing:
    case RetroChip::View::Error: render_message(retro); break;
    }
    lv_obj_align(frame_.tag, LV_ALIGN_TOP_RIGHT, -6, 2);
}

int RetroView::wrapped(const std::string& value, uint32_t color, int x, int y, int width) {
    lv_obj_t* label = text(body_, value, fonts_.small, color, x, y);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, width);
    lv_obj_set_style_text_line_space(label, 1, 0);
    lv_obj_update_layout(label);
    return y + lv_obj_get_height(label);
}

int RetroView::pill(const char* label, int x, int y, bool filled) {
    lv_obj_t* b = box(body_, x, y, 0, 15, filled ? kRetroTheme.text : kRetroTheme.bg_bottom);
    lv_obj_set_size(b, LV_SIZE_CONTENT, 15);
    lv_obj_set_style_pad_hor(b, 5, 0);
    set_border(b, 1, filled ? kRetroTheme.text : kGoldDim);
    lv_obj_set_style_radius(b, 3, 0);
    lv_obj_t* t = text(b, label, fonts_.small, filled ? kInkDark : kRetroTheme.text, 0, 0);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_update_layout(b);
    return x + lv_obj_get_width(b) + 6;
}

lv_obj_t* RetroView::poster(const RetroChip::Game& game, int x, int y, int w, int h, bool selected) {
    const auto& colors = kPosterColors[static_cast<size_t>(game.id) % kPosterColors.size()];
    lv_obj_t* p = box(body_, x, y, w, h, colors[0]);
    set_gradient(p, colors[1]);
    lv_obj_set_style_radius(p, 4, 0);
    set_border(p, selected ? 2 : 1, selected ? kRetroTheme.text : 0x55555f);
    lv_obj_set_style_clip_corner(p, true, 0);
    // Title at the bottom, white on a dark shadow line, like the mockup posters.
    // Wrapped here at spaces (LVGL would break inside a word), "..." only at the end.
    const int text_w = w - 6;
    const int lines = h >= 100 ? 4 : 3;
    lv_point_t glyph{};
    lv_text_get_size(&glyph, "MMMMMMMMMM", fonts_.small, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    const size_t cols = glyph.x > 0 ? static_cast<size_t>(text_w * 10 / glyph.x) : 10;
    lv_obj_t* title = text(p, RetroChip::wrap_title(game.name, cols, static_cast<size_t>(lines)), fonts_.small, kWhite, 0, 0);
    lv_label_set_long_mode(title, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(title, text_w);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(title, 0, 0);
    lv_obj_align(title, LV_ALIGN_BOTTOM_MID, 0, -3);
    lv_obj_t* badge = text(p, RetroChip::short_label(game.platform, game.platform), fonts_.small, 0xffffff, 0, 0);
    lv_obj_set_style_text_opa(badge, LV_OPA_70, 0);
    lv_obj_align(badge, LV_ALIGN_TOP_MID, 0, 3);
    return p;
}

void RetroView::render_library(const RetroChip& retro) {
    // Screen 1: system tabs, four posters, the selected game's line.
    lv_label_set_text(frame_.title, "RETRO · ROMM");
    const auto& platforms = retro.platforms();
    const auto& games = retro.games();
    const std::string tab = retro.platform() < platforms.size() ? platforms[retro.platform()].label : "ALL";
    std::string count = std::to_string(games.size());
    if (retro.search().empty() && retro.has_more()) count += "/" + std::to_string(retro.total_games());
    lv_label_set_text(frame_.tag, (tab + " · " + count).c_str());
    lv_label_set_text(frame_.footer, retro.searching() ? "type to search [↵]done [ESC]clear"
                                                       : "[↑↓]game [←→]page [TAB]system [/]find [↵]open");

    // Tabs: scroll so the current one is always visible.
    int x = 6;
    const size_t first_tab = retro.platform() > 5 ? retro.platform() - 5 : 0;
    for (size_t i = first_tab; i < platforms.size() && x < 300; ++i) {
        const bool sel = i == retro.platform();
        lv_obj_t* t = box(body_, x, 4, 0, 13, sel ? kRetroTheme.text : kRetroTheme.bg_bottom);
        lv_obj_set_size(t, LV_SIZE_CONTENT, 13);
        lv_obj_set_style_pad_hor(t, 4, 0);
        set_border(t, 1, sel ? kRetroTheme.text : kGoldDim);
        lv_obj_set_style_radius(t, 3, 0);
        lv_obj_t* l = text(t, platforms[i].label, fonts_.small, sel ? kInkDark : kRetroTheme.text, 0, 0);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_update_layout(t);
        x += lv_obj_get_width(t) + 4;
    }

    if (games.empty() && retro.loading_more()) {
        wrapped(retro.search().empty() ? "loading games..." : "searching more games...", kRetroTheme.text, kLeft, 40, 304);
    } else if (games.empty()) {
        wrapped(retro.search().empty() ? "No games on this system in RomM." : "Nothing matches \"" + retro.search() + "\".",
                kRetroTheme.text, kLeft, 40, 304);
    } else {
        // Dense list: one row per game (system badge + whole-word title), the selected row filled gold.
        // Rows scroll with the selection; each row is its own tap target.
        constexpr int kRowTop = 18;
        constexpr int kRowStep = 15;
        constexpr int kRowH = 14;
        constexpr int kBadgeW = 30;
        const size_t sel = retro.selected();
        const size_t first = RetroChip::list_first(games.size(), sel, RetroChip::kListRows);
        lv_point_t glyph{};
        lv_text_get_size(&glyph, "MMMMMMMMMM", fonts_.small, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        const int title_w = 304 - kBadgeW - 6;
        const size_t cols = glyph.x > 0 ? static_cast<size_t>(title_w * 10 / glyph.x) : 30;
        for (size_t row = 0; row < RetroChip::kListRows && first + row < games.size(); ++row) {
            const size_t i = first + row;
            const RetroChip::Game& game = *games[i];
            const bool on = i == sel;
            const int y = kRowTop + static_cast<int>(row) * kRowStep;
            tap_target(body_, kLeft, y, 304, kRowH, static_cast<int>(i) - static_cast<int>(sel), true);
            lv_obj_t* r = box(body_, kLeft, y, 304, kRowH, on ? kRetroTheme.text : kRetroTheme.bg_bottom);
            set_border(r, 1, on ? kRetroTheme.text : 0x55555f);
            lv_obj_set_style_radius(r, 3, 0);
            lv_obj_t* badge = text(r, RetroChip::short_label(game.platform, game.platform), fonts_.small, on ? kInkDark : kGoldDim, 4, 1);
            lv_obj_t* title = text(r, RetroChip::wrap_title(game.name, cols, 1), fonts_.small, on ? kInkDark : kWhite, kBadgeW + 2, 1);
            lv_label_set_long_mode(title, LV_LABEL_LONG_CLIP);
            lv_obj_set_width(title, title_w);
        }
        const auto* g = games[sel];
        std::string line = g->name;
        if (g->year > 0) line += " · " + std::to_string(g->year);
        if (retro.downloaded(*g)) line += " · on PET";
        if (retro.loading_more() && sel + 1 >= games.size()) line = "loading more...";
        text(body_, clip_cols(line, 43), fonts_.small, kWhite, kLeft, 126);
    }
    if (retro.searching() || !retro.search().empty()) {
        lv_obj_t* s = box(body_, 180, 126, 132, 14, kRetroTheme.bar);
        set_border(s, 1, kRetroTheme.text);
        lv_obj_set_style_radius(s, 3, 0);
        text(s, clip_cols("/" + retro.search() + (retro.searching() ? "_" : ""), 17), fonts_.small, kRetroTheme.text, 4, 0);
    }
}

void RetroView::render_details(const RetroChip& retro) {
    // Screen 2: poster, facts, Play / Keep offline.
    const auto* g = retro.game();
    if (!g) return;
    const bool downloading = retro.view() == RetroChip::View::Downloading;
    const std::string label = label_for(retro, g->platform);
    lv_label_set_text(frame_.title, clip_cols(upper(g->name), 34).c_str());
    lv_label_set_text(frame_.tag, label.c_str());
    poster(*g, 10, 6, 80, 104, false);
    constexpr int kX = 100;
    constexpr int kW = 212;
    int y = wrapped(clip_cols(g->name, 58), kWhite, kX, 4, kW);
    std::string facts = label;
    if (g->year > 0) facts += " · " + std::to_string(g->year);
    if (g->size > 0) facts += " · " + RetroChip::size_text(g->size);
    text(body_, facts, fonts_.small, kRetroTheme.text, kX, y + 1);
    y += 14;
    const bool on_pet = retro.downloaded(*g);
    lv_obj_t* l = text(body_, "On PET: ", fonts_.small, kRetroTheme.text, kX, y);
    lv_obj_update_layout(l);
    text(body_, on_pet ? "yes, plays offline" : "no, downloads first", fonts_.small, on_pet ? kGreen : kYellow,
         kX + lv_obj_get_width(l), y);
    y += 14;
    bool save_good = false;
    const std::string save = retro.save_text(save_good);
    l = text(body_, "Save: ", fonts_.small, kRetroTheme.text, kX, y);
    lv_obj_update_layout(l);
    text(body_, clip_cols(save, 24), fonts_.small, save_good ? kGreen : kWhite, kX + lv_obj_get_width(l), y);
    y += 14;
    const std::string fit = RetroChip::fit_text(g->platform);
    l = text(body_, "Fit: ", fonts_.small, kRetroTheme.text, kX, y);
    lv_obj_update_layout(l);
    text(body_, fit, fonts_.small, fit.rfind("1:1", 0) == 0 ? kGreen : kYellow, kX + lv_obj_get_width(l), y);
    y += 18;
    if (downloading) {
        text(body_, retro.status(), fonts_.small, kYellow, kX, y);
        lv_label_set_text(frame_.footer, "[ESC]cancel download");
        return;
    }
    const int next = pill("↵ Play", kX, y, retro.details_play());
    if (!on_pet) pill("O Keep offline", next, y, !retro.details_play());
    if (!retro.status().empty()) wrapped(clip_cols(retro.status(), 60), kRetroTheme.text, kX, y + 18, kW);
    lv_label_set_text(frame_.footer, on_pet ? "[↵]play [ESC]back" : "[←→]pick [↵]go [O]offline [ESC]back");
}

void RetroView::render_message(const RetroChip& retro) {
    lv_label_set_text(frame_.title, "RETRO · ROMM");
    switch (retro.view()) {
    case RetroChip::View::Loading:
        lv_label_set_text(frame_.tag, "loading");
        wrapped(retro.status(), kRetroTheme.text, kLeft, 50, 304);
        lv_label_set_text(frame_.footer, "[ESC]chips");
        break;
    case RetroChip::View::Playing: {
        const auto* g = retro.game();
        const std::string fit = g ? RetroChip::fit_text(g->platform) : "";
        lv_label_set_text(frame_.title, g ? clip_cols(upper(g->name), 26).c_str() : "PLAYING");
        lv_label_set_text(frame_.tag, "keyboard");
        wrapped("Playing" + (g ? " " + g->name : std::string()) + " (" + fit + ").", kWhite, kLeft, 40, 304);
        wrapped("Quit from the RetroArch menu (F1); the save syncs to RomM after. Hold ESC to force it closed.", kRetroTheme.text, kLeft, 76, 304);
        lv_label_set_text(frame_.footer, "F1: menu · save state · quit");
        break;
    }
    default:
        lv_label_set_text(frame_.tag, "error");
        lv_obj_set_style_text_color(frame_.tag, lv_color_hex(kRed), 0);
        wrapped(retro.status(), kRed, kLeft, 30, 304);
        lv_label_set_text(frame_.footer, "[↵]retry [ESC]back");
        break;
    }
}

} // namespace view
