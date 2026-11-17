/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace model {

// One colour entry in a 16x16 sprite palette. '.' is always transparent.
struct PaletteEntry {
    char code;
    uint32_t rgb;
};

struct Sprite {
    const char* const* rows; // 16 rows, each 16 chars; '.' is transparent
    const PaletteEntry* palette;
    size_t palette_size;
};

// Battle chip module descriptor. Each chip declares the hardware it needs so
// the launcher can power the rest down on slot-in (see BUILD-PLAN.md).
struct ChipInfo {
    const char* id;          // stable key, e.g. "jellyfin"
    const char* name;        // label printed on the chip
    const char* number;      // folder number, "001".."007" (matches mockups/chips.html)
    const char* element;     // element tag from the mockup glyph
    uint32_t body_rgb;       // shell colour
    uint32_t dark_rgb;       // shell shadow colour
    uint32_t art_top_rgb;    // label art gradient, top
    uint32_t art_bottom_rgb; // label art gradient, bottom
    Sprite sprite;
    const char* needs[4];    // power profiles; null-terminated
};

constexpr size_t kChipCount = 7;

const ChipInfo& chip_at(size_t index);
size_t chip_count();
// Index of the chip with this id, or kChipCount if unknown.
size_t chip_index(const char* id);

// 16x16 avatar used by Alice on the home screen.
const Sprite& alice_sprite();

} // namespace model
