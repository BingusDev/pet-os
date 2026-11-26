/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "view/alice_view.h"
#include "viewmodel/chips/voice_chip.h"

#include <array>
#include <filesystem>

namespace view {

// VOICE screens from mockups/comms.html: 1 push-to-talk, 2 Alice answers.
class VoiceView : public ChipView {
public:
    VoiceView(lv_obj_t* root, const AliceFonts& fonts, const std::filesystem::path& art_dir);
    lv_obj_t* screen() const override { return frame_.screen; }
    void render(const viewmodel::ChipModule& chip) override;

private:
    AliceFonts fonts_;
    ChipFrame frame_;
    ImageAsset alice_{};
    ImageAsset mic_{};
    lv_obj_t* mic_icon_{nullptr};
    lv_obj_t* alice_img_{nullptr};
    lv_obj_t* wave_{nullptr};
    std::array<lv_obj_t*, viewmodel::chips::VoiceChip::kWaveBars> bars_{};
    lv_obj_t* status_{nullptr};
    lv_obj_t* hint_{nullptr};
    lv_obj_t* you_{nullptr};
    lv_obj_t* bubble_{nullptr};
    lv_obj_t* bubble_text_{nullptr};
    lv_obj_t* page_tag_{nullptr};
};

} // namespace view
