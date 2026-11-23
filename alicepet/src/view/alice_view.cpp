/*
 * SPDX-License-Identifier: MIT
 */

#include "view/alice_view.h"
#include "view/widgets.h"
#include "logger/logger.h"

#include "platform/device_status.h"
#include "platform/jack_link.h"

#include <png.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <sys/stat.h>
#include <ctime>

#ifdef __APPLE__
#define ALICEPET_MTIM st_mtimespec
#else
#define ALICEPET_MTIM st_mtim
#endif

namespace view {

namespace {

// Slotted-screen copy per chip, in registry order (001-007). Description is the
// chip's purpose from CHIPS.md; the two status lines are PLACEHOLDERS until each
// chip module reports real state.
struct SlotCopy {
    const char* description;
    const char* link;   // placeholder status line
    const char* ready;  // placeholder status line (shown in green with a check)
};
constexpr SlotCopy kSlotCopy[model::kChipCount] = {
    {"Plug PET into a PC, open the app", "Linked: LAPTOP-FRONT", "read-only scan ready"},
    {"Stream your Jellyfin library to the PET", "Server: JELLYFIN", "library synced"},
    {"Learn + replay IR remotes", "Remotes: 3 learned", "IR blaster ready"},
    {"Test a jack: link, DHCP, DNS, ping", "Link: 1000 Mb/s full", "DHCP lease OK"},
    {"Push-to-talk with Alice", "Mic: hold [SPACE]", "push-to-talk only"},
    {"Play your RomM library on the PET", "Library: RomM", "save sync ready"},
    {"SSH bookmarks over Tailscale", "Hosts: 6 bookmarks", "tailnet connected"},
};

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
    load_png(chip_art_dir / "alice-mini.png", alice_mini_);

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
    // How the PET is online (updated with the battery): Wi-Fi, LAN, PC net (JACK IN sharing) or offline.
    route_dot_ = box(top, 6 + 9 * 7 + 5, 5, 5, 5, kGreen);
    lv_obj_set_style_radius(route_dot_, LV_RADIUS_CIRCLE, 0);
    route_label_ = text(top, "...", fonts_.tiny, kGreen, 6 + 9 * 7 + 14, 2);

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
    bubble_greeting_ = bubble_text;
    bubble_greeting_icon_ = place_inline_icon(bubble_text, 11, icon_bolt_large_);

    // Chat reply: the view model pre-wraps lines to the monospace grid, so clip instead of re-wrapping.
    bubble_chat_ = text(bubble, "", fonts_.tiny, kPanelBlue, 7, 4);
    lv_label_set_long_mode(bubble_chat_, LV_LABEL_LONG_CLIP);
    lv_obj_set_size(bubble_chat_, 188 - 4 - 10, 74 - 4 - 6);
    lv_obj_set_style_text_line_space(bubble_chat_, 3, 0);
    lv_obj_add_flag(bubble_chat_, LV_OBJ_FLAG_HIDDEN);

    // Page tag riding on the bubble's bottom edge: "1/3 [↓]".
    bubble_page_tag_ = box(home_, 0, 0, 10, 13, kEdgeBlue);
    lv_obj_set_style_radius(bubble_page_tag_, 3, 0);
    lv_obj_set_size(bubble_page_tag_, LV_SIZE_CONTENT, 13);
    lv_obj_set_style_pad_hor(bubble_page_tag_, 3, 0);
    bubble_page_label_ = text(bubble_page_tag_, "", fonts_.tiny, 0xffffff, 0, 0);
    lv_obj_align(bubble_page_label_, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_add_flag(bubble_page_tag_, LV_OBJ_FLAG_HIDDEN);

    prompt_idle_ = text(home_, "> type or hold   to talk_", fonts_.tiny, kPaleCyan, 126, 108);
    prompt_idle_icon_ = place_inline_icon(prompt_idle_, 15, icon_mic_);
    prompt_input_ = text(home_, "", fonts_.tiny, kPaleCyan, 126, 108);
    lv_obj_add_flag(prompt_input_, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* home_footer = box(home_, 0, 154, 320, 16, kPanelBlue);
    set_border(home_footer, 1, kEdgeBlue);
    lv_obj_set_style_border_side(home_footer, LV_BORDER_SIDE_TOP, 0);
    text(home_footer, "[1]CHIP FOLDER [2]NET [3]VOICE [ESC]sleep", fonts_.tiny, kHintBlue, 6, 1);

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

    // NET SCAN: host list with status dots, n/N ONLINE counter, details panel.
    netscan_ = make_screen_background(root, kDeepBlue, kPanelBlue);
    lv_obj_add_flag(netscan_, LV_OBJ_FLAG_HIDDEN);
    chip_bar(netscan_, "");
    netscan_title_ = text(netscan_, "NET SCAN", fonts_.small, kPaleCyan, 6, 2);
    netscan_counter_ = text(netscan_, "-/- ONLINE", fonts_.small, kPaleCyan, 0, 2);
    lv_obj_align(netscan_counter_, LV_ALIGN_TOP_RIGHT, -6, 2);
    chip_footer(netscan_, "[↑↓]select [↵]details [R]rescan [ESC]home");
    make_image(netscan_, alice_mini_, 320 - 4 - 44, 156 - 2 - 44);
    netscan_rows_ = lv_obj_create(netscan_);
    lv_obj_remove_style_all(netscan_rows_);
    lv_obj_remove_flag(netscan_rows_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(netscan_rows_, 0, 20);
    lv_obj_set_size(netscan_rows_, 320, 134);
    netscan_details_ = box(netscan_, 24, 22, 244, 128, kPanelBlue);
    set_border(netscan_details_, 2, kEdgeBlue);
    lv_obj_set_style_radius(netscan_details_, 6, 0);
    lv_obj_add_flag(netscan_details_, LV_OBJ_FLAG_HIDDEN);

    // Sleep overlay (ESC on Home or idle timeout), above every screen. On the
    // device the backlight is off as well; the desktop only has this overlay.
    sleep_overlay_ = box(root, 0, 0, 320, 170, 0x000000);
    lv_obj_t* z_big = text(sleep_overlay_, "Z", fonts_.body, 0x1d3f73, 232, 104);
    lv_obj_t* z_small = text(sleep_overlay_, "z", fonts_.tiny, 0x16325c, 252, 92);
    (void)z_big;
    (void)z_small;
    text(sleep_overlay_, "z", fonts_.tiny, 0x10264a, 264, 82);
    text(sleep_overlay_, "ALICE.EXE napping · any key wakes", fonts_.tiny, 0x16325c, 0, 0);
    lv_obj_align(lv_obj_get_child(sleep_overlay_, -1), LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_add_flag(sleep_overlay_, LV_OBJ_FLAG_HIDDEN);

    // Volume bar, bottom centre, on LVGL's top layer so it draws over every screen
    // and chip view (see update_volume_osd).
    volume_osd_ = box(lv_layer_top(), 60, 128, 200, 30, kPanelBlue);
    set_border(volume_osd_, 2, kEdgeBlue);
    lv_obj_set_style_radius(volume_osd_, 6, 0);
    volume_label_ = text(volume_osd_, "VOL", fonts_.tiny, 0xffffff, 8, 7);
    volume_bar_ = lv_bar_create(volume_osd_);
    lv_obj_set_size(volume_bar_, 120, 10);
    lv_obj_set_pos(volume_bar_, 66, 8);
    lv_bar_set_range(volume_bar_, 0, 100);
    lv_obj_add_flag(volume_osd_, LV_OBJ_FLAG_HIDDEN);
    if (const char* run = std::getenv("XDG_RUNTIME_DIR")) volume_path_ = std::string(run) + "/alicepet-volume";
    struct stat st {};
    LOG_INFO("volume osd watching {}", volume_path_.empty() ? std::string("(no XDG_RUNTIME_DIR)") : volume_path_);
    if (!volume_path_.empty() && ::stat(volume_path_.c_str(), &st) == 0) {
        volume_mtime_ns_ = int64_t(st.ALICEPET_MTIM.tv_sec) * 1000000000 + st.ALICEPET_MTIM.tv_nsec;
    }
}

void AliceView::update_volume_osd(uint32_t now_ms) {
    if (!volume_osd_ || volume_path_.empty()) return;
    struct stat st {};
    if (::stat(volume_path_.c_str(), &st) == 0) {
        const int64_t mtime = int64_t(st.ALICEPET_MTIM.tv_sec) * 1000000000 + st.ALICEPET_MTIM.tv_nsec;
        if (mtime != volume_mtime_ns_) {
            volume_mtime_ns_ = mtime;
            int percent = 0, muted = 0;
            std::ifstream(volume_path_) >> percent >> muted;
            percent = std::clamp(percent, 0, 100);
            lv_bar_set_value(volume_bar_, muted ? 0 : percent, LV_ANIM_OFF);
            lv_label_set_text(volume_label_, muted ? "MUTE" : ("VOL " + std::to_string(percent)).c_str());
            lv_obj_remove_flag(volume_osd_, LV_OBJ_FLAG_HIDDEN);
            LOG_INFO("volume osd {}{}", percent, muted ? " muted" : "");
            volume_hide_at_ms_ = now_ms + kVolumeOsdMs;
            volume_shown_ = true;
        }
    }
    if (volume_shown_ && int32_t(now_ms - volume_hide_at_ms_) >= 0) {
        lv_obj_add_flag(volume_osd_, LV_OBJ_FLAG_HIDDEN);
        volume_shown_ = false;
    }
}

void AliceView::add_chip_view(size_t index, std::unique_ptr<ChipView> chip_view) {
    if (index >= chip_views_.size() || !chip_view) {
        return;
    }
    lv_obj_add_flag(chip_view->screen(), LV_OBJ_FLAG_HIDDEN);
    chip_views_[index] = std::move(chip_view);
    lv_obj_move_foreground(sleep_overlay_);
    rendered_revision_ = UINT32_MAX;
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
lv_obj_t* AliceView::place_inline_icon(lv_obj_t* label, uint32_t char_index, const ImageAsset& icon) {
    if (!icon.loaded) {
        return nullptr;
    }
    lv_obj_update_layout(label);
    lv_point_t pos{};
    lv_label_get_letter_pos(label, char_index, &pos);
    const lv_font_t* font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
    const int line_h = lv_font_get_line_height(font);
    const int x = lv_obj_get_x(label) + pos.x;
    const int y = lv_obj_get_y(label) + pos.y + (line_h - static_cast<int>(icon.desc.header.h)) / 2;
    return make_image(lv_obj_get_parent(label), icon, x, y);
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
    show(netscan_, vm.screen() == viewmodel::Screen::NetScan);
    for (size_t i = 0; i < chip_views_.size(); ++i) {
        if (chip_views_[i]) {
            show(chip_views_[i]->screen(), vm.screen() == viewmodel::Screen::Chip && vm.selected_chip() == i);
        }
    }
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
        render_chat(vm);
        break;
    case viewmodel::Screen::NetScan:
        render_netscan(vm);
        break;
    case viewmodel::Screen::Chip:
        if (const auto* chip = vm.active_chip(); chip && vm.selected_chip() < chip_views_.size() &&
                                                chip_views_[vm.selected_chip()]) {
            chip_views_[vm.selected_chip()]->render(*chip);
        }
        break;
    }
}

void AliceView::render_chat(const viewmodel::AliceViewModel& vm) {
    const auto set_hidden = [](lv_obj_t* obj, bool hidden) {
        if (!obj) return;
        if (hidden) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    };
    const auto state = vm.chat_state();
    const bool greeting = state == viewmodel::ChatState::Greeting;
    set_hidden(bubble_greeting_, !greeting);
    set_hidden(bubble_greeting_icon_, !greeting);
    set_hidden(bubble_chat_, greeting);

    if (state == viewmodel::ChatState::Thinking) {
        static const char* kDots[] = {"thinking", "thinking.", "thinking..", "thinking..."};
        lv_label_set_text(bubble_chat_, kDots[vm.thinking_dots() & 3]);
        lv_obj_set_style_text_color(bubble_chat_, lv_color_hex(kGridBlue), 0);
    } else if (!greeting) {
        std::string joined;
        for (const auto& line : vm.bubble_page_lines()) {
            if (!joined.empty()) joined += '\n';
            joined += line;
        }
        lv_label_set_text(bubble_chat_, joined.c_str());
        lv_obj_set_style_text_color(bubble_chat_,
                                    lv_color_hex(state == viewmodel::ChatState::Error ? 0xb3263c : kPanelBlue), 0);
    }

    const size_t pages = greeting || state == viewmodel::ChatState::Thinking ? 1 : vm.bubble_pages();
    set_hidden(bubble_page_tag_, pages <= 1);
    if (pages > 1) {
        const bool more = vm.bubble_page() + 1 < pages;
        lv_label_set_text_fmt(bubble_page_label_, "%zu/%zu %s", vm.bubble_page() + 1, pages, more ? "↓" : "↑");
        lv_obj_update_layout(bubble_page_tag_);
        lv_obj_set_pos(bubble_page_tag_, 124 + 188 - 8 - lv_obj_get_width(bubble_page_tag_), 24 + 74 - 3);
    }

    const bool typing = !vm.input().empty();
    set_hidden(prompt_idle_, typing);
    set_hidden(prompt_idle_icon_, typing);
    set_hidden(prompt_input_, !typing);
    if (typing) {
        // Show the tail that fits: 320 - 126 px at 7 px per glyph, minus "> " and the cursor.
        constexpr size_t kVisible = 25;
        const std::string& input = vm.input();
        std::vector<size_t> starts;
        for (size_t i = 0; i < input.size(); ++i) {
            if ((static_cast<unsigned char>(input[i]) & 0xC0) != 0x80) starts.push_back(i);
        }
        const size_t from = starts.size() > kVisible ? starts[starts.size() - kVisible] : 0;
        const std::string shown = std::string(from > 0 ? "<" : ">") + " " + input.substr(from) + "_";
        lv_label_set_text(prompt_input_, shown.c_str());
    }
}

void AliceView::render_netscan(const viewmodel::AliceViewModel& vm) {
    const auto& hosts = vm.hosts();
    lv_label_set_text_fmt(netscan_counter_, "%zu/%zu ONLINE", vm.online_count(), hosts.size());
    lv_label_set_text(netscan_title_, vm.scanning() ? "NET SCAN · scanning..." : "NET SCAN");

    const auto status_of = [](const viewmodel::HostRow& h, uint32_t& color) -> const char* {
        switch (h.state) {
        case viewmodel::HostState::Online: color = kGreen; return "OK";
        case viewmodel::HostState::Rest: color = kYellow; return "REST";
        case viewmodel::HostState::Offline: color = kRed; return "OFFLINE";
        case viewmodel::HostState::Checking: color = kHintBlue; return "...";
        case viewmodel::HostState::Unknown: break;
        }
        color = kHintBlue;
        return "?";
    };

    lv_obj_clean(netscan_rows_);
    constexpr int kRowH = 15;
    constexpr size_t kNameCols = 18;
    for (size_t i = 0; i < hosts.size() && i < 8; ++i) {
        const auto& h = hosts[i];
        const int y = static_cast<int>(i) * kRowH;
        if (i == vm.selected_host()) {
            lv_obj_t* hl = box(netscan_rows_, 4, y, 262, kRowH, kEdgeBlue);
            lv_obj_set_style_bg_opa(hl, LV_OPA_50, 0);
            lv_obj_set_style_radius(hl, 3, 0);
        }
        uint32_t color = kHintBlue;
        const char* status = status_of(h, color);
        lv_obj_t* dot = box(netscan_rows_, 10, y + 5, 6, 6, color);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);

        // "Mac mini ........." padded with dot leaders to a fixed column (monospace).
        std::string name = h.name.substr(0, kNameCols - 2) + " ";
        while (name.size() < kNameCols) name += '.';
        text(netscan_rows_, name.c_str(), fonts_.small, kScreenText, 20, y + 1);
        const int status_x = 20 + static_cast<int>(kNameCols + 1) * 7;
        text(netscan_rows_, status, fonts_.small, color, status_x, y + 1);
        if (h.latency_ms >= 0 && h.state != viewmodel::HostState::Checking) {
            char ms[16];
            std::snprintf(ms, sizeof(ms), "%dms", h.latency_ms);
            text(netscan_rows_, ms, fonts_.small, kHintBlue, status_x + 8 * 7, y + 1);
        }
    }
    if (hosts.empty()) {
        text(netscan_rows_, "No hosts. Add [netscan] host= lines\nto /etc/alicepet.conf", fonts_.small, kScreenText, 10, 4);
    }

    lv_obj_clean(netscan_details_);
    if (vm.host_details_open() && vm.selected_host() < hosts.size()) {
        lv_obj_remove_flag(netscan_details_, LV_OBJ_FLAG_HIDDEN);
        const auto& h = hosts[vm.selected_host()];
        uint32_t color = kHintBlue;
        const char* status = status_of(h, color);
        text(netscan_details_, h.name.c_str(), fonts_.small, kGold, 8, 4);
        text(netscan_details_, ("addr   " + h.address).c_str(), fonts_.small, kScreenText, 8, 22);
        text(netscan_details_, ("probe  " + h.probe).c_str(), fonts_.small, kScreenText, 8, 37);
        lv_obj_t* st = text(netscan_details_, "state  ", fonts_.small, kScreenText, 8, 52);
        lv_obj_update_layout(st);
        text(netscan_details_, status, fonts_.small, color, 8 + lv_obj_get_width(st), 52);
        if (h.latency_ms >= 0) {
            char ms[24];
            std::snprintf(ms, sizeof(ms), "%d ms", h.latency_ms);
            text(netscan_details_, ms, fonts_.small, kHintBlue, 8 + 7 * 15, 52);
        }
        lv_obj_t* detail = text(netscan_details_, h.detail.empty() ? "-" : h.detail.c_str(), fonts_.small, kPaleCyan, 8, 67);
        lv_label_set_long_mode(detail, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(detail, 244 - 16);
        text(netscan_details_, "[↵/ESC] back", fonts_.small, kHintBlue, 8, 106);
    } else {
        lv_obj_add_flag(netscan_details_, LV_OBJ_FLAG_HIDDEN);
    }
}

void AliceView::update_clock() {
    const uint32_t tick = lv_tick_get();
    if (status_read_once_ && lv_tick_diff(tick, last_clock_ms_) < 1000) {
        return;
    }
    last_clock_ms_ = tick;
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
    if (status_read_once_ && lv_tick_diff(tick, last_status_ms_) < kStatusPollMs) {
        return;
    }
    status_read_once_ = true;
    last_status_ms_ = tick;
    const platform::DeviceStatus status = platform::read_device_status();
    const auto route = platform::net_route(status, platform::jack::proxy_active());
    if (route != route_ || !route_shown_) {
        route_ = route;
        route_shown_ = true;
        const uint32_t color = route == platform::NetRoute::Offline ? kRed
                               : route == platform::NetRoute::PcNet ? kEdgeBlue
                                                                    : kGreen;
        lv_label_set_text(route_label_, platform::net_route_label(route));
        lv_obj_set_style_text_color(route_label_, lv_color_hex(color), 0);
        lv_obj_set_style_bg_color(route_dot_, lv_color_hex(color), 0);
    }
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
