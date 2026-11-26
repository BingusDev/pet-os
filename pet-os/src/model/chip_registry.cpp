/*
 * SPDX-License-Identifier: MIT
 */

#include "model/chip_registry.h"

#include <string_view>

namespace model {

namespace {

// Palettes and pixel rows are ported from mockups/sprites.js.
constexpr PaletteEntry kJellyfinPal[] = {
    {'k', 0x141414}, {'w', 0xf2f2f2}, {'p', 0xb14be0}, {'c', 0x3fc7e8}, {'g', 0x555555}};
constexpr PaletteEntry kJackInPal[] = {
    {'k', 0x111111}, {'s', 0xc9d2e3}, {'d', 0x7a8499}, {'y', 0xffd200}, {'r', 0xe2434b}, {'w', 0xffffff}};
constexpr PaletteEntry kRemotePal[] = {
    {'k', 0x141414}, {'b', 0x2b2b33}, {'l', 0x4a4a55}, {'r', 0xff4a4a}, {'w', 0xf2f2f2}, {'c', 0x7fd3ff}, {'g', 0x3ad17a}};
constexpr PaletteEntry kLanPal[] = {
    {'k', 0x0d1b3d}, {'c', 0xbfe9ff}, {'y', 0xffd200}, {'b', 0x2a5bd7}, {'w', 0xffffff}};
constexpr PaletteEntry kVoicePal[] = {
    {'k', 0x141414}, {'s', 0xc9d2e3}, {'d', 0x7a8499}, {'p', 0xff6fb0}, {'w', 0xffffff}};
constexpr PaletteEntry kRetroPal[] = {
    {'k', 0x141414}, {'g', 0x9aa0ab}, {'d', 0x5d636e}, {'r', 0xe2434b}, {'b', 0x2f6fe0}, {'w', 0xe8e8ee}};
constexpr PaletteEntry kTerminalPal[] = {
    {'k', 0x0a0a0a}, {'f', 0x2b2f3a}, {'g', 0x47f08a}, {'s', 0x9aa0ab}};
constexpr PaletteEntry kAlicePal[] = {
    {'k', 0x0d1b3d}, {'h', 0x3fc7e8}, {'s', 0xffd7b5}, {'e', 0x1a1a2e}, {'r', 0xe2434b}, {'b', 0x2b5bd7}, {'y', 0xffd200}};

constexpr const char* kJellyfinRows[16] = {
    "................",
    "..kk............",
    ".kwwkk..........",
    ".kwkkwkk........",
    "..kwwkkwkk......",
    "...kkwwkkwkk....",
    ".....kkwwkkwk...",
    ".kkkkkkkkkkkkkk.",
    ".kwwwwwwwwwwwwk.",
    ".kwppppwwccccwk.",
    ".kwppppwwccccwk.",
    ".kwwwwwwwwwwwwk.",
    ".kwgggwwwwggwwk.",
    ".kwwwwwwwwwwwwk.",
    ".kkkkkkkkkkkkkk.",
    "................"};

constexpr const char* kJackInRows[16] = {
    "................",
    "......kkkk......",
    ".....kssssk.....",
    ".....kswwsk.....",
    ".....kssssk.....",
    "....kkkkkkkk....",
    "....krrrrrrk....",
    "....krrrrrrk....",
    "....krrwwrrk....",
    "....krrrrrrk....",
    ".....kkkkkk.....",
    "......kddk......",
    "..y...kddk...y..",
    "...y..kddk..y...",
    "y.....kddk.....y",
    ".......kk......."};

constexpr const char* kRemoteRows[16] = {
    "......c..c......",
    ".......cc.......",
    ".....kkkkkk.....",
    "....kbbrrbbk....",
    "....kbbbbbbk....",
    "....kblwwlbk....",
    "....kbwlbwbk....",
    "....kblwwlbk....",
    "....kbbbbbbk....",
    "....kbgbgbgk....",
    "....kbbbbbbk....",
    "....kbgbgbgk....",
    "....kbbbbbbk....",
    "....kbgbgbgk....",
    "....kbbbbbbk....",
    ".....kkkkkk....."};

constexpr const char* kLanRows[16] = {
    "................",
    "...kkkkkkkkkk...",
    "...kccccccccck..",
    "...kcykykykyck..",
    "...kcykykykyck..",
    "...kccccccccck..",
    "...kcccckccccck.",
    "....kccckcccck..",
    ".....kkkkkkkk...",
    "......kbbbbk....",
    "......kbbbbk....",
    ".......kbbk.....",
    ".......kbbk.....",
    ".......kbbk.....",
    ".......kbbk.....",
    "........kk......"};

constexpr const char* kVoiceRows[16] = {
    "................",
    "......kkkk......",
    ".....kssssk.....",
    ".....kswdsk.....",
    ".....ksdsdk.....",
    "..k..kssssk..k..",
    "..k..ksdsdk..k..",
    "..k..kssssk..k..",
    "...k..kkkk..k...",
    "....kk....kk....",
    "......kkkk......",
    ".......kk.......",
    ".......kk.......",
    ".....kkkkkk.....",
    "..p..........p..",
    ".p............p."};

constexpr const char* kRetroRows[16] = {
    "................",
    "................",
    "................",
    "..kkkkkkkkkkkk..",
    ".kggggggggggggk.",
    "kggkggggggggrggk",
    "kgkkkgggggggggbk",
    "kggkgggdgdgrgggk",
    "kggggggggggggbgk",
    ".kggggggggggggk.",
    "..kkkkkkkkkkkk..",
    "................",
    "................",
    "................",
    "................",
    "................"};

constexpr const char* kTerminalRows[16] = {
    "................",
    ".kkkkkkkkkkkkkk.",
    ".kffffffffffffk.",
    ".kfkkkkkkkkkkfk.",
    ".kfkgkkkkkkkkfk.",
    ".kfkkgkkkkkkkfk.",
    ".kfkgkkggggkkfk.",
    ".kfkkkkkkkkkkfk.",
    ".kfkkkkkkkkkkfk.",
    ".kffffffffffffk.",
    ".kkkkkkkkkkkkkk.",
    "......kssk......",
    "....kkkkkkkk....",
    "....kssssssk....",
    "....kkkkkkkk....",
    "................"};

constexpr ChipInfo kChips[kChipCount] = {
    {"jack-in", "JACK IN", "001", "elec", 0x2b2b33, 0x111116, 0xffe37a, 0xe0a21a,
     {kJackInRows, kJackInPal, sizeof(kJackInPal) / sizeof(kJackInPal[0])},
     {"usb_hub", "wifi", nullptr}},
    {"jellyfin", "JELLYFIN", "002", "music", 0x7d4fd6, 0x432391, 0xff9ad1, 0xb0247a,
     {kJellyfinRows, kJellyfinPal, sizeof(kJellyfinPal) / sizeof(kJellyfinPal[0])},
     {"wifi", "audio", "cores:4"}},
    {"remote", "REMOTE", "003", "target", 0xc0353f, 0x7a1720, 0x8fd8ff, 0x2a74d0,
     {kRemoteRows, kRemotePal, sizeof(kRemotePal) / sizeof(kRemotePal[0])},
     {"ir", "wifi", nullptr}},
    {"lan-tech", "LAN TECH", "004", "diamond", 0x2fa35a, 0x17643a, 0xbff0ff, 0x3a8fd0,
     {kLanRows, kLanPal, sizeof(kLanPal) / sizeof(kLanPal[0])},
     {"wifi", nullptr}},
    {"voice", "VOICE", "005", "heart", 0xe85a9a, 0x9a2a5e, 0xffd0e8, 0xe2578f,
     {kVoiceRows, kVoicePal, sizeof(kVoicePal) / sizeof(kVoicePal[0])},
     {"audio", "wifi", nullptr}},
    {"retro", "RETRO", "006", "star", 0x8a909c, 0x4a4f5c, 0xffb0b0, 0x7a2ad0,
     {kRetroRows, kRetroPal, sizeof(kRetroPal) / sizeof(kRetroPal[0])},
     {"wifi", "hdmi", "cores:2"}},
    {"terminal", "TERMINAL", "007", "square", 0x3a3f4c, 0x1a1d24, 0x2a3a2a, 0x0a120a,
     {kTerminalRows, kTerminalPal, sizeof(kTerminalPal) / sizeof(kTerminalPal[0])},
     {"wifi", nullptr}},
};

constexpr const char* kAliceRows[16] = {
    "................",
    ".....kkkkkk.....",
    "...kkhhhhhhkk...",
    "..khhhhhhhhhhk..",
    ".khhhhhhhhhhhhk.",
    ".khhssssssssshk.",
    ".khssssssssssshk",
    ".khsseesssseehk.",
    ".khsseesssseehk.",
    ".khssssssssssshk",
    ".kkssssrrrssskk.",
    "..kssssssssssk..",
    "....kbbbbbbk....",
    "...kbbbbbbbbk...",
    "..kbbbyyyybbbk..",
    "..kkkkkkkkkkkk.."};

constexpr Sprite kAliceSprite = {
    kAliceRows,
    kAlicePal,
    sizeof(kAlicePal) / sizeof(kAlicePal[0])};

} // namespace

const ChipInfo& chip_at(size_t index) {
    return kChips[index % kChipCount];
}

size_t chip_count() {
    return kChipCount;
}

size_t chip_index(const char* id) {
    for (size_t i = 0; i < kChipCount; ++i) {
        if (std::string_view(kChips[i].id) == id) return i;
    }
    return kChipCount;
}

const Sprite& alice_sprite() {
    return kAliceSprite;
}

} // namespace model
