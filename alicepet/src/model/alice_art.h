#pragma once

#include <cstdint>

namespace model {

struct PaletteRgb {
    uint8_t r;
    uint8_t g;
    uint8_t b;
};

// Palette-indexed bitmap; pixels are row-major, one palette index per byte.
struct IndexedBitmap {
    int width;
    int height;
    const uint8_t* pixels;
    const PaletteRgb* palette;
    int palette_size;
};

// Alice card art downscaled from avatars/alice.png (generated, see alice_art.cpp).
const IndexedBitmap& alice_home();
const IndexedBitmap& alice_slot();

} // namespace model
