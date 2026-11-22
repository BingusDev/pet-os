/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "lvgl.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// Shared drawing helpers and palette for AlicePET screens (Home, folder, chip views).
namespace view {

constexpr uint32_t kInk = 0x0d1b3d;
constexpr uint32_t kPanelBlue = 0x0a2350;
constexpr uint32_t kDeepBlue = 0x061633;
constexpr uint32_t kEdgeBlue = 0x2f7de0;
constexpr uint32_t kGridBlue = 0x1d5fb8;
constexpr uint32_t kHintBlue = 0x7fb6ff;
constexpr uint32_t kCyan = 0x7fd3ff;
constexpr uint32_t kPaleCyan = 0x9fd8ff;
constexpr uint32_t kGold = 0xffd200;
constexpr uint32_t kGreen = 0x47f08a;
constexpr uint32_t kBubble = 0xe9f4ff;
constexpr uint32_t kScreenText = 0xcfe8ff;
constexpr uint32_t kBarBlue = 0x47b3ff;
constexpr uint32_t kSubtitle = 0x9fc7ff;
constexpr uint32_t kYellow = 0xffd54a;
constexpr uint32_t kRed = 0xff5a6e;
constexpr uint32_t kWhite = 0xffffff;

// Fonts are supplied by the app so the view uses the bundled Departure Mono face.
struct AliceFonts {
    const lv_font_t* tiny{nullptr};  // status bar, footers, prompt
    const lv_font_t* body{nullptr};  // bubbles, chip names
    const lv_font_t* title{nullptr}; // boot title
    const lv_font_t* small{nullptr}; // chip folder / slotted bars and detail text (11 px grid)
};

// A decoded PNG kept alive for LVGL (ARGB8888, LVGL byte order B, G, R, A).
struct ImageAsset {
    lv_image_dsc_t desc{};
    std::vector<uint8_t> pixels;
    bool loaded{false};
};

// Per-chip screen colours, taken from mockups/{comms,media,tools}.html.
struct ChipTheme {
    uint32_t bg_top;     // gradient / radial centre
    uint32_t bg_bottom;
    uint32_t bar;        // top bar and footer fill
    uint32_t edge;       // bar / footer rule, selection
    uint32_t text;       // bar + footer text
};

// Departure Mono 11 px: every glyph is 7 px wide.
constexpr int kGlyphW = 7;

bool load_png(const std::filesystem::path& path, ImageAsset& out);
lv_obj_t* box(lv_obj_t* parent, int x, int y, int w, int h, uint32_t bg);
// An invisible click target over a list row/tile for the Jack In mouse: a PC
// click on it moves the selection `steps` rows (negative = up/left) along the
// given axis, then presses Enter. Found later with find_tap_target().
lv_obj_t* tap_target(lv_obj_t* parent, int x, int y, int w, int h, int steps, bool horizontal);
// The tap target under screen point (x, y), if any.
bool find_tap_target(lv_obj_t* root, int x, int y, int& steps, bool& horizontal);
void set_gradient(lv_obj_t* obj, uint32_t bottom);
void set_border(lv_obj_t* obj, int width, uint32_t color);
lv_obj_t* text(lv_obj_t* parent, const char* value, const lv_font_t* font, uint32_t color, int x, int y);
lv_obj_t* text(lv_obj_t* parent, const std::string& value, const lv_font_t* font, uint32_t color, int x, int y);
void add_grid(lv_obj_t* parent);
void add_glow(lv_obj_t* parent, int cx, int cy, int radius, uint32_t color, lv_opa_t opa);
lv_obj_t* make_screen_background(lv_obj_t* root, uint32_t top, uint32_t bottom);
lv_obj_t* image(lv_obj_t* parent, const ImageAsset& asset, int x, int y);
// Transparent full-size container (no style, no scroll).
lv_obj_t* layer(lv_obj_t* parent, int x, int y, int w, int h);
void set_visible(lv_obj_t* obj, bool visible);

// A themed chip screen: background, 16 px bar with a left title and right tag,
// 14 px footer with key hints. Content goes into `body` (y 17..155).
struct ChipFrame {
    lv_obj_t* screen{nullptr};
    lv_obj_t* body{nullptr};
    lv_obj_t* title{nullptr};
    lv_obj_t* tag{nullptr};
    lv_obj_t* footer{nullptr};
};
ChipFrame make_chip_frame(lv_obj_t* root, const ChipTheme& theme, const AliceFonts& fonts);
// Pads `name` with dot leaders to `cols` glyphs: "mox ........".
std::string dot_leader(const std::string& name, size_t cols);
// Cuts a UTF-8 string to at most `cols` code points (adds "~" when cut).
std::string clip_cols(const std::string& value, size_t cols);

} // namespace view
