/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "lvgl.h"

#include "model/alice_art.h"
#include "model/chip_registry.h"
#include "viewmodel/alice_viewmodel.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace view {

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

// Builds the four AlicePET screens on a 320x170 root and renders the view model.
class AliceView {
public:
    // chip_art_dir holds the chip PNGs rendered from mockups/chips.html (assets/images/chips).
    AliceView(lv_obj_t* root, const AliceFonts& fonts, const std::filesystem::path& chip_art_dir);

    void render(const viewmodel::AliceViewModel& vm);
    void update_clock();

private:
    // Sprite slots keep pixel buffers and image descriptors alive for LVGL.
    static constexpr size_t kSpriteSlots = 1;
    enum Slot : size_t { kHomeAlice };

    // Pre-rendered chip art per chip: large (selected), small (neighbour), tilted (slotted), title text.
    struct ChipArt {
        ImageAsset big;
        ImageAsset small;
        ImageAsset tilt;
        ImageAsset title;
    };

    lv_obj_t* make_bitmap(lv_obj_t* parent, size_t slot, const model::IndexedBitmap& bitmap, int x, int y);
    lv_obj_t* make_image(lv_obj_t* parent, const ImageAsset& asset, int x, int y);
    lv_obj_t* place_inline_icon(lv_obj_t* label, uint32_t char_index, const ImageAsset& icon);
    void load_chip_art(const std::filesystem::path& dir);
    void rebuild_folder(size_t selected);
    void rebuild_slotted(size_t selected);
    void render_chat(const viewmodel::AliceViewModel& vm);
    void render_netscan(const viewmodel::AliceViewModel& vm);

    AliceFonts fonts_{};

    lv_obj_t* boot_{nullptr};
    lv_obj_t* boot_bar_{nullptr};
    lv_obj_t* home_{nullptr};
    lv_obj_t* battery_label_{nullptr};
    lv_obj_t* battery_fill_{nullptr};
    lv_obj_t* clock_label_{nullptr};
    lv_obj_t* folder_{nullptr};
    lv_obj_t* folder_carousel_{nullptr};
    lv_obj_t* folder_counter_{nullptr};
    lv_obj_t* slotted_{nullptr};
    lv_obj_t* slotted_holder_{nullptr};
    lv_obj_t* sleep_overlay_{nullptr};

    // Home chat: greeting label (with its inline bolt), reply label, page tag, prompt.
    lv_obj_t* bubble_greeting_{nullptr};
    lv_obj_t* bubble_greeting_icon_{nullptr};
    lv_obj_t* bubble_chat_{nullptr};
    lv_obj_t* bubble_page_tag_{nullptr};
    lv_obj_t* bubble_page_label_{nullptr};
    lv_obj_t* prompt_idle_{nullptr};
    lv_obj_t* prompt_idle_icon_{nullptr};
    lv_obj_t* prompt_input_{nullptr};

    // NET SCAN.
    lv_obj_t* netscan_{nullptr};
    lv_obj_t* netscan_counter_{nullptr};
    lv_obj_t* netscan_title_{nullptr};
    lv_obj_t* netscan_rows_{nullptr};
    lv_obj_t* netscan_details_{nullptr};
    ImageAsset alice_mini_{};

    std::array<lv_image_dsc_t, kSpriteSlots> descriptors_{};
    std::array<std::vector<uint8_t>, kSpriteSlots> pixels_{};
    std::array<ChipArt, model::kChipCount> chip_art_{};
    ImageAsset icon_check_{};
    ImageAsset icon_bolt_{};
    ImageAsset icon_bolt_large_{};
    ImageAsset icon_mic_{};
    ImageAsset icon_signal_{};
    ImageAsset alice_home_{};

    uint32_t rendered_revision_{UINT32_MAX};
    viewmodel::Screen rendered_screen_{viewmodel::Screen::Boot};
    size_t rendered_selected_{0};
    bool rendered_sleeping_{false};
    char clock_text_[8]{};
    char battery_text_[8]{};
};

} // namespace view
