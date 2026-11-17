/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "view/alice_view.h"
#include "viewmodel/chips/lan_tech_chip.h"

#include <filesystem>
#include <string>

namespace view {

// LAN TECH screens from mockups/tools.html: 5 jack test, 6 diagnosis, 7 devices.
class LanTechView : public ChipView {
public:
    LanTechView(lv_obj_t* root, const AliceFonts& fonts, const std::filesystem::path& art_dir);
    lv_obj_t* screen() const override { return frame_.screen; }
    void render(const viewmodel::ChipModule& chip) override;

private:
    using LanTechChip = viewmodel::chips::LanTechChip;
    void render_test(const LanTechChip& lan);
    void render_alice(const LanTechChip& lan);
    void render_devices(const LanTechChip& lan);
    void set_headline(const LanTechChip& lan);
    int check_row(const LanTechChip::Check& check, int y);
    // A wrapped label; max_h > 0 cuts it with "..." at that height. Returns the y below it.
    int wrapped(const std::string& value, uint32_t color, int y, int max_h);

    AliceFonts fonts_;
    ChipFrame frame_;
    ImageAsset check_{};
    lv_obj_t* body_{nullptr};
};

} // namespace view
