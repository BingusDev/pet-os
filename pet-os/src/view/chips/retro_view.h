/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "view/alice_view.h"
#include "viewmodel/chips/retro_chip.h"

#include <string>

namespace view {

// RETRO screens from mockups/media.html: 1 library, 2 game + save, 3/4 playing.
class RetroView : public ChipView {
public:
    RetroView(lv_obj_t* root, const AliceFonts& fonts);
    lv_obj_t* screen() const override { return frame_.screen; }
    void render(const viewmodel::ChipModule& chip) override;

private:
    using RetroChip = viewmodel::chips::RetroChip;
    void render_library(const RetroChip& retro);
    void render_details(const RetroChip& retro);
    void render_message(const RetroChip& retro);
    lv_obj_t* poster(const RetroChip::Game& game, int x, int y, int w, int h, bool selected);
    int wrapped(const std::string& value, uint32_t color, int x, int y, int width);
    int pill(const char* label, int x, int y, bool filled);

    AliceFonts fonts_;
    ChipFrame frame_;
    lv_obj_t* body_{nullptr};
};

} // namespace view
