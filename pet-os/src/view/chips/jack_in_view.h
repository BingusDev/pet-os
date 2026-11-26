/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "view/alice_view.h"
#include "viewmodel/chips/jack_in_chip.h"

#include <string>

namespace view {

// JACK IN on the PET: waiting for the PC, the session (mockups/comms.html
// screen 6, pet.html screen 5) and the full command log.
class JackInView : public ChipView {
public:
    JackInView(lv_obj_t* root, const AliceFonts& fonts);
    lv_obj_t* screen() const override { return frame_.screen; }
    void render(const viewmodel::ChipModule& chip) override;

private:
    using JackInChip = viewmodel::chips::JackInChip;
    void render_waiting(const JackInChip& jack);
    void render_session(const JackInChip& jack);
    void render_log(const JackInChip& jack);
    void pills(const JackInChip& jack);
    // A wrapped label; max_h > 0 cuts it with "..." at that height. Returns the y below it.
    int wrapped(const std::string& value, uint32_t color, int x, int y, int w, int max_h);

    AliceFonts fonts_;
    ChipFrame frame_;
    lv_obj_t* body_{nullptr};
    lv_obj_t* pills_{nullptr};
};

} // namespace view
