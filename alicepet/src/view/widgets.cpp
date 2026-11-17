/*
 * SPDX-License-Identifier: MIT
 */

#include "view/widgets.h"

#include <png.h>

namespace view {

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

lv_obj_t* text(lv_obj_t* parent, const std::string& value, const lv_font_t* font, uint32_t color, int x, int y) {
    return text(parent, value.c_str(), font, color, x, y);
}

lv_obj_t* image(lv_obj_t* parent, const ImageAsset& asset, int x, int y) {
    if (!asset.loaded) {
        return nullptr;
    }
    lv_obj_t* img = lv_image_create(parent);
    lv_image_set_src(img, &asset.desc);
    lv_image_set_antialias(img, false);
    lv_obj_set_pos(img, x, y);
    return img;
}

lv_obj_t* layer(lv_obj_t* parent, int x, int y, int w, int h) {
    lv_obj_t* obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    return obj;
}

void set_visible(lv_obj_t* obj, bool visible) {
    if (!obj) return;
    if (visible) {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

ChipFrame make_chip_frame(lv_obj_t* root, const ChipTheme& theme, const AliceFonts& fonts) {
    ChipFrame frame;
    frame.screen = box(root, 0, 0, 320, 170, theme.bg_top);
    set_gradient(frame.screen, theme.bg_bottom);
    lv_obj_add_flag(frame.screen, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* bar = box(frame.screen, 0, 0, 320, 16, theme.bar);
    set_border(bar, 1, theme.edge);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    frame.title = text(bar, "", fonts.small, theme.text, 6, 2);
    frame.tag = text(bar, "", fonts.small, theme.text, 0, 2);
    lv_obj_align(frame.tag, LV_ALIGN_TOP_RIGHT, -6, 2);

    frame.body = layer(frame.screen, 0, 17, 320, 139);

    lv_obj_t* foot = box(frame.screen, 0, 156, 320, 14, theme.bar);
    set_border(foot, 1, theme.edge);
    lv_obj_set_style_border_side(foot, LV_BORDER_SIDE_TOP, 0);
    frame.footer = text(foot, "", fonts.small, theme.text, 6, 0);
    return frame;
}

namespace {
size_t cp_len(unsigned char lead) {
    if (lead < 0x80) return 1;
    if ((lead >> 5) == 0x6) return 2;
    if ((lead >> 4) == 0xE) return 3;
    if ((lead >> 3) == 0x1E) return 4;
    return 1;
}
size_t count_cols(const std::string& value) {
    size_t n = 0;
    for (size_t i = 0; i < value.size(); i += cp_len(static_cast<unsigned char>(value[i]))) ++n;
    return n;
}
} // namespace

std::string clip_cols(const std::string& value, size_t cols) {
    if (count_cols(value) <= cols) return value;
    std::string out;
    size_t n = 0;
    for (size_t i = 0; i < value.size() && n + 1 < cols; ++n) {
        const size_t len = cp_len(static_cast<unsigned char>(value[i]));
        out += value.substr(i, len);
        i += len;
    }
    return out + "~";
}

std::string dot_leader(const std::string& name, size_t cols) {
    std::string out = clip_cols(name, cols > 2 ? cols - 2 : cols) + " ";
    for (size_t n = count_cols(out); n < cols; ++n) out += '.';
    return out;
}

} // namespace view
