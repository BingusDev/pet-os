/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "view/alice_view.h"
#include "viewmodel/chips/lan_tech_chip.h"

#include <filesystem>

namespace view {

// LAN TECH screens from mockups/tools.html: 5 jack test, 6 diagnosis, 7 devices.
class LanTechView : public ChipView {
public:
    LanTechView(lv_obj_t* root, const AliceFonts& fonts, const std::filesystem::path& art_dir);
    lv_obj_t* screen() const override { return frame_.screen; }
    void render(const viewmodel::ChipModule& chip) override;

private:
    AliceFonts fonts_;
    ChipFrame frame_;
    ImageAsset check_{};
    lv_obj_t* tag_dot_{nullptr};
};

} // namespace view
