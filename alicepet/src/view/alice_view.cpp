/*
 * SPDX-License-Identifier: MIT
 */

#include "view/alice_view.h"

#include "platform/device_status.h"

#include <png.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace view {

namespace {

constexpr uint32_t kInk = 0x0d1b3d;
constexpr uint32_t kPanelBlue = 0x0a2350;
constexpr uint32_t kDeepBlue = 0x061633;
constexpr uint32_t kEdgeBlue = 0x2f7de0;
constexpr uint32_t kGridBlue = 0x1d5fb8;
constexpr uint32_t kHintBlue = 0x7fb6ff;
constexpr uint32_t kCyan = 0x7fd3ff;
constexpr uint32_t kPaleCyan = 0x9fd8ff;
constexpr uint32_t kGold = 0xffd200;
constexpr uint32_t kGreen = 0x47f08a;
constexpr uint32_t kBubble = 0xe9f4ff;
constexpr uint32_t kScreenText = 0xcfe8ff;
constexpr uint32_t kBarBlue = 0x47b3ff;
constexpr uint32_t kSubtitle = 0x9fc7ff;

// Slotted-screen copy per chip, in registry order (001-007). Description is the
// chip's purpose from CHIPS.md; the two status lines are PLACEHOLDERS until each
// chip module reports real state.
struct SlotCopy {
    const char* description;
    const char* link;   // placeholder status line
    const char* ready;  // placeholder status line (shown in green with a check)
};
constexpr SlotCopy kSlotCopy[model::kChipCount] = {
    {"Stream your Jellyfin library to the PET", "Server: JELLYFIN", "library synced"},
    {"Plug PET into a PC, open the app", "Linked: LAPTOP-FRONT", "read-only scan ready"},
    {"Learn + replay IR remotes", "Remotes: 3 learned", "IR blaster ready"},
    {"Test a jack: link, DHCP, DNS, ping", "Link: 1000 Mb/s full", "DHCP lease OK"},
    {"Push-to-talk with Alice", "Mic: hold [SPACE]", "push-to-talk only"},
    {"Play your RomM library on the PET", "Library: RomM", "save sync ready"},
    {"SSH bookmarks over Tailscale", "Hosts: 6 bookmarks", "tailnet connected"},
};

// Decodes a PNG into LVGL's ARGB8888 byte order (B, G, R, A, straight alpha).
bool load_png(const std::filesystem::path& path, ImageAsset& out) {
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_file(&image, path.string().c_str())) {
        return false;
    }
    image.format = PNG_FORMAT_BGRA;
    out.pixels.assign(PNG_IMAGE_SIZE(image), 0);
    if (!png_image_finish_read(&image, nullptr, out.pixels.data(), 0, nullptr)) {
        png_image_free(&image);
        return false;
    }
    out.desc = lv_image_dsc_t{};
    out.desc.header.magic = LV_IMAGE_HEADER_MAGIC;
    out.desc.header.cf = LV_COLOR_FORMAT_ARGB8888;
    out.desc.header.w = image.width;
    out.desc.header.h = image.height;
    out.desc.header.stride = image.width * 4;
    out.desc.data_size = static_cast<uint32_t>(out.pixels.size());
    out.desc.data = out.pixels.data();
    out.loaded = true;
    return true;
}

lv_obj_t* box(lv_obj_t* parent, int x, int y, int w, int h, uint32_t bg) {
    lv_obj_t* obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    return obj;
}

void set_gradient(lv_obj_t* obj, uint32_t bottom) {
    lv_obj_set_style_bg_grad_color(obj, lv_color_hex(bottom), 0);
    lv_obj_set_style_bg_grad_dir(obj, LV_GRAD_DIR_VER, 0);
}

void set_border(lv_obj_t* obj, int width, uint32_t color) {
    lv_obj_set_style_border_width(obj, width, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(color), 0);
}

lv_obj_t* text(lv_obj_t* parent, const char* value, const lv_font_t* font, uint32_t color, int x, int y) {
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, value);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_pos(label, x, y);
    return label;
}

// Faint 20px grid across a full screen; add it right after the background.
void add_grid(lv_obj_t* parent) {
    for (int x = 0; x < 320; x += 20) {
        lv_obj_t* line = box(parent, x, 0, 1, 170, kGridBlue);
        lv_obj_set_style_bg_opa(line, LV_OPA_40, 0);
    }
    for (int y = 0; y < 170; y += 20) {
        lv_obj_t* line = box(parent, 0, y, 320, 1, kGridBlue);
        lv_obj_set_style_bg_opa(line, LV_OPA_40, 0);
    }
}

// Soft glow: a transparent disc whose box shadow spreads outward.
void add_glow(lv_obj_t* parent, int cx, int cy, int radius, uint32_t color, lv_opa_t opa) {
    lv_obj_t* glow = box(parent, cx - radius, cy - radius, radius * 2, radius * 2, color);
    lv_obj_set_style_bg_opa(glow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(glow, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_shadow_width(glow, radius * 2, 0);
    lv_obj_set_style_shadow_spread(glow, 0, 0);
    lv_obj_set_style_shadow_color(glow, lv_color_hex(color), 0);
    lv_obj_set_style_shadow_opa(glow, opa, 0);
}

// Blue grid background with a gradient and a central glow.
lv_obj_t* make_screen_background(lv_obj_t* root, uint32_t top, uint32_t bottom) {
    lv_obj_t* screen = box(root, 0, 0, 320, 170, top);
    set_gradient(screen, bottom);
    add_grid(screen);
    return screen;
}

// Expands a palette-indexed bitmap into ARGB8888 (LVGL byte order: B, G, R, A).
void fill_bitmap(std::vector<uint8_t>& out, const model::IndexedBitmap& bitmap) {
    const size_t count = static_cast<size_t>(bitmap.width) * bitmap.height;
    out.assign(count * 4, 0xff);
    for (size_t i = 0; i < count; ++i) {
        const uint8_t index = bitmap.pixels[i];
        const model::PaletteRgb& color = bitmap.palette[index < bitmap.palette_size ? index : 0];
        out[i * 4] = color.b;
        out[i * 4 + 1] = color.g;
        out[i * 4 + 2] = color.r;
    }
}

} // namespace

AliceView::AliceView(lv_obj_t* root, const AliceFonts& fonts, const std::filesystem::path& chip_art_dir)
    : fonts_(fonts) {
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    if (!fonts_.small) {
        fonts_.small = fonts_.tiny;
    }
    load_chip_art(chip_art_dir);
    load_png(chip_art_dir / "icon-bolt-11.png", icon_bolt_large_);
    load_png(chip_art_dir / "icon-mic-11.png", icon_mic_);
    load_png(chip_art_dir / "icon-signal.png", icon_signal_);
    load_png(chip_art_dir / "alice-home.png", alice_home_);

    // BOOT: glowing P.E.T. title, subtitle, 2s progress bar, Navi connect line.
    // All text is solid and full-opacity at 11/33 px so the pixel font stays crisp.
    boot_ = make_screen_background(root, 0x0f3b7a, kDeepBlue);
    // Hard 2px drop shadow, as in the mockup, instead of a blurred halo.
    lv_obj_t* title_shadow = text(boot_, "P.E.T.", fonts_.title, kPanelBlue, 0, 0);
    lv_obj_align(title_shadow, LV_ALIGN_TOP_MID, 3, 32 + 2);
    // Departure Mono has one weight; a second copy 1px right gives the mockup's bold title.
    lv_obj_align(text(boot_, "P.E.T.", fonts_.title, kCyan, 0, 0), LV_ALIGN_TOP_MID, 1, 32);
    lv_obj_t* title = text(boot_, "P.E.T.", fonts_.title, kCyan, 0, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 32);
    lv_obj_t* subtitle = text(boot_, "PERSONAL TERMINAL · ALICE.EXE", fonts_.tiny, kSubtitle, 0, 0);
    lv_obj_align(subtitle, LV_ALIGN_TOP_MID, 0, 72);
    boot_bar_ = lv_bar_create(boot_);
    lv_obj_set_size(boot_bar_, 180, 8);
    lv_obj_align(boot_bar_, LV_ALIGN_TOP_MID, 0, 96);
    lv_bar_set_range(boot_bar_, 0, 100);
    lv_obj_set_style_bg_color(boot_bar_, lv_color_hex(kPanelBlue), 0);
    lv_obj_set_style_bg_opa(boot_bar_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(boot_bar_, 4, 0);
    set_border(boot_bar_, 1, kBarBlue);
    lv_obj_set_style_bg_color(boot_bar_, lv_color_hex(kBarBlue), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(boot_bar_, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(boot_bar_, 4, LV_PART_INDICATOR);
    lv_obj_t* status = text(boot_, "Connecting to Navi via Tailscale...", fonts_.tiny, kGreen, 0, 0);
    lv_obj_align(status, LV_ALIGN_TOP_MID, 0, 112);

    // NAVI HOME: status bar, Alice close-up, speech bubble, prompt, key footer.
    home_ = make_screen_background(root, 0x0f3b7a, kDeepBlue);
    lv_obj_add_flag(home_, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* top = box(home_, 0, 0, 320, 16, kPanelBlue);
    set_border(top, 1, kEdgeBlue);
    lv_obj_set_style_border_side(top, LV_BORDER_SIDE_BOTTOM, 0);
    text(top, "ALICE.EXE", fonts_.tiny, kPaleCyan, 6, 2);
    lv_obj_t* dot = box(top, 6 + 9 * 7 + 5, 5, 5, 5, kGreen);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    text(top, "ONLINE", fonts_.tiny, kGreen, 6 + 9 * 7 + 14, 2);

    // Right cluster: signal bars, battery gauge, battery %, clock (right to left via flex).
    lv_obj_t* status_right = lv_obj_create(top);
    lv_obj_remove_style_all(status_right);
    lv_obj_remove_flag(status_right, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(status_right, LV_SIZE_CONTENT, 15);
    lv_obj_set_flex_flow(status_right, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status_right, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(status_right, 4, 0);
    lv_obj_align(status_right, LV_ALIGN_TOP_RIGHT, -6, 0);
    make_image(status_right, icon_signal_, 0, 0);
    lv_obj_t* battery = lv_obj_create(status_right);
    lv_obj_remove_style_all(battery);
    lv_obj_set_size(battery, 15, 8);
    lv_obj_t* battery_body = box(battery, 0, 0, 13, 8, kPanelBlue);
    set_border(battery_body, 1, kGreen);
    box(battery, 13, 2, 2, 4, kGreen);
    battery_fill_ = box(battery, 2, 2, 0, 4, kGreen);
    battery_label_ = text(status_right, "--%", fonts_.tiny, kPaleCyan, 0, 0);
    clock_label_ = text(status_right, "--:--", fonts_.tiny, kPaleCyan, 0, 0);
    lv_obj_set_style_margin_left(clock_label_, 3, 0);

    // Alice close-up (head to knees), cropped from the card art like mockups/pet.html.
    make_image(home_, alice_home_, 8, 26);

    lv_obj_t* bubble = box(home_, 124, 24, 188, 74, kBubble);
    lv_obj_set_style_radius(bubble, 8, 0);
    set_border(bubble, 2, kEdgeBlue);
    // Two spaces after "Hi Holden!" reserve room for the inline bolt icon.
    lv_obj_t* bubble_text = text(bubble, "Hi Holden!   All systems green. Backups ran OK last night. Want me to do anything?",
                                 fonts_.tiny, kPanelBlue, 7, 4);
    lv_label_set_long_mode(bubble_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(bubble_text, 188 - 4 - 14);
    lv_obj_set_style_text_line_space(bubble_text, 3, 0);
    place_inline_icon(bubble_text, 11, icon_bolt_large_);

    lv_obj_t* prompt = text(home_, "> type or hold   to talk_", fonts_.tiny, kPaleCyan, 126, 108);
    place_inline_icon(prompt, 15, icon_mic_);

    lv_obj_t* home_footer = box(home_, 0, 154, 320, 16, kPanelBlue);
    set_border(home_footer, 1, kEdgeBlue);
    lv_obj_set_style_border_side(home_footer, LV_BORDER_SIDE_TOP, 0);
    text(home_footer, "[1]NET [2]CHIP FOLDER [3]VOICE [ESC]sleep", fonts_.tiny, kHintBlue, 6, 1);

    // Bar + footer layout for the two chip screens follows mockups/chips.html:
    // 16px top bar, 14px footer, 11px Departure Mono text.
    const auto chip_bar = [&](lv_obj_t* screen, const char* title) {
        lv_obj_t* bar = box(screen, 0, 0, 320, 16, kPanelBlue);
        set_border(bar, 1, kEdgeBlue);
        lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
        text(screen, title, fonts_.small, kPaleCyan, 6, 2);
    };
    const auto chip_footer = [&](lv_obj_t* screen, const char* hint) {
        lv_obj_t* foot = box(screen, 0, 156, 320, 14, kPanelBlue);
        set_border(foot, 1, kEdgeBlue);
        lv_obj_set_style_border_side(foot, LV_BORDER_SIDE_TOP, 0);
        return text(foot, hint, fonts_.small, kHintBlue, 6, 0);
    };

    // CHIP FOLDER: selected chip large with a gold outline, neighbours small and dimmed.
    folder_ = make_screen_background(root, kDeepBlue, kPanelBlue);
    lv_obj_add_flag(folder_, LV_OBJ_FLAG_HIDDEN);
    chip_bar(folder_, "BATTLE CHIPS · FOLDER");
    folder_counter_ = text(folder_, "1/7", fonts_.small, kPaleCyan, 0, 2);
    lv_obj_align(folder_counter_, LV_ALIGN_TOP_RIGHT, -6, 2);
    chip_footer(folder_, "[←→]browse [↵]SLOT IN [ESC]home");
    folder_carousel_ = lv_obj_create(folder_);
    lv_obj_remove_style_all(folder_carousel_);
    lv_obj_remove_flag(folder_carousel_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(folder_carousel_, 0, 17);
    lv_obj_set_size(folder_carousel_, 320, 139);

    // BATTLE CHIP IN!: tilted chip, EXECUTE tag, chip name, description, status lines, Alice line.
    slotted_ = make_screen_background(root, kDeepBlue, kPanelBlue);
    lv_obj_add_flag(slotted_, LV_OBJ_FLAG_HIDDEN);
    chip_bar(slotted_, "BATTLE CHIP IN!");
    lv_obj_t* execute = text(slotted_, "EXECUTE", fonts_.small, kGold, 0, 2);
    lv_obj_align(execute, LV_ALIGN_TOP_RIGHT, -6, 2);
    lv_obj_t* alice_line = chip_footer(slotted_, "Alice: \"Jack in, Alice! Execute!\"");
    lv_obj_update_layout(alice_line);
    make_image(lv_obj_get_parent(alice_line), icon_bolt_large_, 6 + lv_obj_get_width(alice_line) + 5, 1);
    slotted_holder_ = lv_obj_create(slotted_);
    lv_obj_remove_style_all(slotted_holder_);
    lv_obj_remove_flag(slotted_holder_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(slotted_holder_, 0, 17);
    lv_obj_set_size(slotted_holder_, 320, 139);

    // Sleep overlay (ESC on home), above every screen.
    sleep_overlay_ = box(root, 0, 0, 320, 170, 0x000000);
    lv_obj_add_flag(sleep_overlay_, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t* AliceView::make_bitmap(lv_obj_t* parent, size_t slot, const model::IndexedBitmap& bitmap, int x, int y) {
    fill_bitmap(pixels_[slot], bitmap);
    lv_image_dsc_t& desc = descriptors_[slot];
    desc = lv_image_dsc_t{};
    desc.header.magic = LV_IMAGE_HEADER_MAGIC;
    desc.header.cf = LV_COLOR_FORMAT_ARGB8888;
    desc.header.w = static_cast<uint32_t>(bitmap.width);
    desc.header.h = static_cast<uint32_t>(bitmap.height);
    desc.header.stride = static_cast<uint32_t>(bitmap.width * 4);
    desc.data_size = static_cast<uint32_t>(pixels_[slot].size());
    desc.data = pixels_[slot].data();

    lv_obj_t* image = lv_image_create(parent);
    lv_image_set_src(image, &desc);
    lv_image_set_antialias(image, false);
    lv_obj_set_pos(image, x, y);
    return image;
}

lv_obj_t* AliceView::make_image(lv_obj_t* parent, const ImageAsset& asset, int x, int y) {
    if (!asset.loaded) {
        return nullptr;
    }
    lv_obj_t* image = lv_image_create(parent);
    lv_image_set_src(image, &asset.desc);
    lv_image_set_antialias(image, false);
    lv_obj_set_pos(image, x, y);
    return image;
}

// Labels cannot embed images, so an icon is drawn over blank spaces reserved in the text.
void AliceView::place_inline_icon(lv_obj_t* label, uint32_t char_index, const ImageAsset& icon) {
    if (!icon.loaded) {
        return;
    }
    lv_obj_update_layout(label);
    lv_point_t pos{};
    lv_label_get_letter_pos(label, char_index, &pos);
    const lv_font_t* font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
    const int line_h = lv_font_get_line_height(font);
    const int x = lv_obj_get_x(label) + pos.x;
    const int y = lv_obj_get_y(label) + pos.y + (line_h - static_cast<int>(icon.desc.header.h)) / 2;
    make_image(lv_obj_get_parent(label), icon, x, y);
}

void AliceView::load_chip_art(const std::filesystem::path& dir) {
    for (size_t i = 0; i < model::kChipCount; ++i) {
        const std::string id = model::chip_at(i).id;
        ChipArt& art = chip_art_[i];
        const bool ok = load_png(dir / (id + "-big.png"), art.big) && load_png(dir / (id + "-small.png"), art.small) &&
                        load_png(dir / (id + "-tilt.png"), art.tilt) && load_png(dir / (id + "-title.png"), art.title);
        if (!ok) {
            LV_LOG_WARN("chip art missing for %s in %s", id.c_str(), dir.string().c_str());
        }
    }
    load_png(dir / "icon-check.png", icon_check_);
    load_png(dir / "icon-bolt.png", icon_bolt_);
}

// Positions are the mockup's PET-screen coordinates minus the 17px top bar.
void AliceView::rebuild_folder(size_t selected) {
    lv_obj_clean(folder_carousel_);
    const size_t count = model::chip_count();
    const size_t prev = (selected + count - 1) % count;
    const size_t next = (selected + 1) % count;

    for (const auto& [index, x] : {std::pair{prev, 14}, std::pair{next, 240}}) {
        lv_obj_t* side = make_image(folder_carousel_, chip_art_[index].small, x, 46 - 17);
        if (side) {
            lv_obj_set_style_image_opa(side, 140, 0);
        }
    }
    make_image(folder_carousel_, chip_art_[selected].big, 110, 22 - 17);

    lv_obj_t* outline = box(folder_carousel_, 104, 20 - 17, 100, 124, kGold);
    lv_obj_set_style_bg_opa(outline, LV_OPA_TRANSP, 0);
    set_border(outline, 2, kGold);
    lv_obj_set_style_radius(outline, 8, 0);
    lv_obj_set_style_shadow_width(outline, 10, 0);
    lv_obj_set_style_shadow_color(outline, lv_color_hex(kGold), 0);
    lv_obj_set_style_shadow_opa(outline, LV_OPA_COVER, 0);

    lv_label_set_text_fmt(folder_counter_, "%zu/%zu", selected + 1, count);
}

void AliceView::rebuild_slotted(size_t selected) {
    lv_obj_clean(slotted_holder_);
    const ChipArt& art = chip_art_[selected];
    const SlotCopy& copy = kSlotCopy[selected];
    // Tilted chip is pre-rotated -8 degrees in the asset (no runtime transform).
    make_image(slotted_holder_, art.tilt, 30, 11 - 17);

    constexpr int kTextX = 150;
    make_image(slotted_holder_, art.title, kTextX, 35 - 17);
    lv_obj_t* desc = text(slotted_holder_, copy.description, fonts_.small, kPaleCyan, kTextX, 51 - 17);
    lv_label_set_long_mode(desc, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(desc, 320 - 8 - kTextX);
    lv_obj_set_style_text_line_space(desc, 2, 0);

    lv_obj_update_layout(desc);
    const int status_y = lv_obj_get_y(desc) + lv_obj_get_height(desc) + 6;
    lv_label_set_text_fmt(text(slotted_holder_, "", fonts_.small, kScreenText, kTextX, status_y), "► %s", copy.link);
    make_image(slotted_holder_, icon_check_, kTextX, status_y + 17);
    text(slotted_holder_, copy.ready, fonts_.small, kGreen, kTextX + 14, status_y + 14);
}

void AliceView::render(const viewmodel::AliceViewModel& vm) {
    if (vm.revision() == rendered_revision_) {
        return;
    }
    rendered_revision_ = vm.revision();
    rendered_screen_ = vm.screen();

    const auto show = [](lv_obj_t* obj, bool visible) {
        if (visible) {
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
        }
    };
    show(boot_, vm.screen() == viewmodel::Screen::Boot);
    show(home_, vm.screen() == viewmodel::Screen::Home);
    show(folder_, vm.screen() == viewmodel::Screen::Folder);
    show(slotted_, vm.screen() == viewmodel::Screen::Slotted);
    show(sleep_overlay_, vm.sleeping());

    switch (vm.screen()) {
    case viewmodel::Screen::Boot:
        lv_bar_set_value(boot_bar_, vm.boot_progress(), LV_ANIM_OFF);
        break;
    case viewmodel::Screen::Folder:
        rebuild_folder(vm.selected_chip());
        break;
    case viewmodel::Screen::Slotted:
        rebuild_slotted(vm.selected_chip());
        break;
    case viewmodel::Screen::Home:
        break;
    }
}

void AliceView::update_clock() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    char buffer[8]{};
    std::strftime(buffer, sizeof(buffer), "%H:%M", &local);
    if (std::strcmp(buffer, clock_text_) != 0) {
        std::memcpy(clock_text_, buffer, sizeof(clock_text_));
        lv_label_set_text(clock_label_, clock_text_);
    }

    // Real battery percentage on device; desktop reports no battery, so it shows "--%".
    // TODO: desktop battery readout (no /sys/class/power_supply on macOS).
    const platform::DeviceStatus status = platform::read_device_status();
    char battery[8]{};
    if (status.battery_present) {
        std::snprintf(battery, sizeof(battery), "%d%%", status.battery_percent);
    } else {
        std::snprintf(battery, sizeof(battery), "--%%");
    }
    if (std::strcmp(battery, battery_text_) != 0) {
        std::memcpy(battery_text_, battery, sizeof(battery_text_));
        lv_label_set_text(battery_label_, battery_text_);
        const int pct = status.battery_present ? std::clamp(status.battery_percent, 0, 100) : 0;
        lv_obj_set_width(battery_fill_, (9 * pct + 50) / 100);
    }
}

} // namespace view
