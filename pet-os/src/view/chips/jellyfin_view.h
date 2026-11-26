/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "view/alice_view.h"
#include "viewmodel/chips/jellyfin_chip.h"

#include <filesystem>

namespace view {

// JELLYFIN screens from mockups/media.html: 1 library, 2 episodes, 3 playing, 4 resume.
class JellyfinView : public ChipView {
public:
    JellyfinView(lv_obj_t* root, const AliceFonts& fonts, const std::filesystem::path& art_dir);
    lv_obj_t* screen() const override { return frame_.screen; }
    void render(const viewmodel::ChipModule& chip) override;

private:
    void render_library(const viewmodel::chips::JellyfinChip& jf);
    void render_episodes(const viewmodel::chips::JellyfinChip& jf);
    void render_resume(const viewmodel::chips::JellyfinChip& jf);
    void render_playing(const viewmodel::chips::JellyfinChip& jf);

    AliceFonts fonts_;
    ChipFrame frame_;
    ImageAsset check_{};
    ImageAsset play_{};
};

} // namespace view
