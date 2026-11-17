/*
 * SPDX-License-Identifier: MIT
 */

#include "viewmodel/alice_viewmodel.h"

#include "model/chip_registry.h"

#include <algorithm>

namespace viewmodel {

void AliceViewModel::tick(uint32_t elapsed_ms) {
    if (screen_ != Screen::Boot) {
        return;
    }

    boot_elapsed_ms_ = std::min(boot_elapsed_ms_ + elapsed_ms, kBootDurationMs);
    const int progress = static_cast<int>(boot_elapsed_ms_ * 100 / kBootDurationMs);
    if (progress != boot_progress_) {
        boot_progress_ = progress;
        ++revision_;
    }
    if (boot_elapsed_ms_ >= kBootDurationMs) {
        show(Screen::Home);
    }
}

void AliceViewModel::handle_key(Key key) {
    // Any key skips the boot animation.
    if (screen_ == Screen::Boot) {
        show(Screen::Home);
        return;
    }

    switch (screen_) {
    case Screen::Home:
        if (key == Key::Escape) {
            sleeping_ = !sleeping_;
            ++revision_;
        } else if (key == Key::Digit2) {
            show(Screen::Folder);
        }
        break;

    case Screen::Folder: {
        const size_t count = model::chip_count();
        if (key == Key::Left) {
            selected_chip_ = (selected_chip_ + count - 1) % count;
            ++revision_;
        } else if (key == Key::Right) {
            selected_chip_ = (selected_chip_ + 1) % count;
            ++revision_;
        } else if (key == Key::Enter) {
            show(Screen::Slotted);
        } else if (key == Key::Escape) {
            show(Screen::Home);
        }
        break;
    }

    case Screen::Slotted:
        if (key == Key::Escape) {
            show(Screen::Folder);
        }
        break;

    case Screen::Boot:
        break;
    }
}

void AliceViewModel::show(Screen next) {
    if (screen_ == next) {
        return;
    }
    screen_ = next;
    ++revision_;
}

} // namespace viewmodel
