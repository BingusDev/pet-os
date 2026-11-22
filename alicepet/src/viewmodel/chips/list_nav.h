/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstddef>
#include <string>

namespace viewmodel::chips {

// List rules shared by the media chips (RETRO, JELLYFIN), so the same keys do the same
// thing on both: Left/Right step one row, Up/Down move a page, Esc clears a filter
// before it leaves the chip, Backspace drops a whole UTF-8 character.

// Rows that fit under the chip header on the 320x170 panel.
constexpr size_t kMediaListRows = 7;
// Rows one Up/Down (or PC mouse-wheel notch) moves by.
constexpr size_t kMediaPageStep = 3;

// The first row shown so `selected` stays in a `rows`-high window of `count` rows (centred, no overrun).
inline size_t media_list_first(size_t count, size_t selected, size_t rows) {
    if (rows == 0 || count <= rows) return 0;
    if (selected < rows / 2) return 0;
    const size_t first = selected - rows / 2;
    return first + rows > count ? count - rows : first;
}

// Removes the last UTF-8 character (all of its bytes), not one continuation byte.
inline void utf8_drop_last(std::string& text) {
    while (!text.empty()) {
        const bool continuation = (static_cast<unsigned char>(text.back()) & 0xC0) == 0x80;
        text.pop_back();
        if (!continuation) break;  // the lead byte ends the character
    }
}

} // namespace viewmodel::chips
