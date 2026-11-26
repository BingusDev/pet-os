/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "view/alice_view.h"
#include "viewmodel/chips/remote_chip.h"

#include <string>

namespace view {

// REMOTE screens from mockups/tools.html: 1 devices, 2 remote pad, 3 learn mode, 4 macros.
class RemoteView : public ChipView {
public:
    RemoteView(lv_obj_t* root, const AliceFonts& fonts);
    lv_obj_t* screen() const override { return frame_.screen; }
    void render(const viewmodel::ChipModule& chip) override;

private:
    using RemoteChip = viewmodel::chips::RemoteChip;
    void render_devices(const RemoteChip& remote);
    void render_name(const RemoteChip& remote);
    void render_pad(const RemoteChip& remote);
    void render_learn(const RemoteChip& remote);
    void render_macros(const RemoteChip& remote);
    void render_macro_run(const RemoteChip& remote);
    int wrapped(const std::string& value, uint32_t color, int x, int y, int width);
    void row(const std::string& left, const std::string& right, int y, bool selected, uint32_t color);

    AliceFonts fonts_;
    ChipFrame frame_;
    lv_obj_t* body_{nullptr};
};

} // namespace view
