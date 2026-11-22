/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "viewmodel/alice_viewmodel.h"
#include "viewmodel/chips/jack_in_chip.h"

#include <functional>

namespace viewmodel {

// What one drain of the Jack In link did (for the PET log).
struct PcInputBatch {
    int keys{0};
    int chars{0};
    int pointers{0};
    int dropped{0};  // input that arrived while no PC chip was open
    std::string chip;
};

// PC requests from the Jack In link, applied on the UI thread: open one of the
// PC-capable chips (keeping the link up), then forward keys, characters and mouse
// events to it. `pointer` handles mouse events (LVGL hit-testing lives in the app).
PcInputBatch drain_pc_requests(AliceViewModel& vm,
                               const std::function<void(const chips::JackInChip::Input&)>& pointer);

} // namespace viewmodel
