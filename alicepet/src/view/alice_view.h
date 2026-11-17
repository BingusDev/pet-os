/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "lvgl.h"

#include "model/alice_art.h"
#include "model/chip_registry.h"
#include "viewmodel/alice_viewmodel.h"
#include "view/widgets.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace view {

// Renders one chip module's own screen (view/chips/). Implementations build
// their LVGL tree on construction (hidden) and redraw in render().
class ChipView {
public:
    virtual ~ChipView() = default;
    virtual lv_obj_t* screen() const = 0;
    virtual void render(const viewmodel::ChipModule& chip) = 0;
};

// Builds the four AlicePET screens on a 320x170 root and renders the view model.
class AliceView {
public:
    // chip_art_dir holds the chip PNGs rendered from mockups/chips.html (assets/images/chips).
    AliceView(lv_obj_t* root, const AliceFonts& fonts, const std::filesystem::path& chip_art_dir);

    void render(const viewmodel::AliceViewModel& vm);
    // Clock + battery for the status bar. Cheap to call every frame: the clock is
    // formatted once a second and the battery/sysfs status is read at most every
    // kStatusPollMs (each read can touch the fuel gauge over I2C).
    void update_clock();
    static constexpr uint32_t kStatusPollMs = 10000;
    // Registers the screen for chip `index`; it is shown while that chip is open.
    void add_chip_view(size_t index, std::unique_ptr<ChipView> chip_view);
    const AliceFonts& fonts() const { return fonts_; }
    const ImageAsset& alice_mini() const { return alice_mini_; }

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
    std::array<std::unique_ptr<ChipView>, viewmodel::AliceViewModel::kMaxChips> chip_views_{};
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
    uint32_t last_clock_ms_{0};
    uint32_t last_status_ms_{0};
    bool status_read_once_{false};
};

} // namespace view
