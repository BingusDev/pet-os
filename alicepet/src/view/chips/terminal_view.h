/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "view/alice_view.h"
#include "viewmodel/chips/terminal_chip.h"

#include <array>

namespace view {

// TERMINAL screens from mockups/comms.html: 3 host list, 4 shell, 5 Ask Alice.
class TerminalView : public ChipView {
public:
    TerminalView(lv_obj_t* root, const AliceFonts& fonts);
    lv_obj_t* screen() const override { return frame_.screen; }
    void render(const viewmodel::ChipModule& chip) override;

private:
    void render_hosts(const viewmodel::chips::TerminalChip& term);
    void render_shell(const viewmodel::chips::TerminalChip& term);
    void render_ask(const viewmodel::chips::TerminalChip& term);

    AliceFonts fonts_;
    ChipFrame frame_;
    lv_obj_t* hosts_{nullptr};
    lv_obj_t* shell_{nullptr};
    std::array<lv_obj_t*, viewmodel::chips::TerminalChip::kRows> rows_{};
    lv_obj_t* cursor_{nullptr};
    lv_obj_t* ask_{nullptr};
};

} // namespace view
