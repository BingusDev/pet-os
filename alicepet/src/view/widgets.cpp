/*
 * SPDX-License-Identifier: MIT
 */

#include "view/widgets.h"

#include "logger/logger.h"
#include "model/navi.h"

#include <png.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>

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

bool load_png_fit(const std::filesystem::path& path, int w, int h, ImageAsset& out) {
    ImageAsset src;
    if (w <= 0 || h <= 0 || !load_png(path, src)) return false;
    const int sw = static_cast<int>(src.desc.header.w);
    const int sh = static_cast<int>(src.desc.header.h);
    if (sw <= 0 || sh <= 0) return false;
    // Fit inside w x h, keeping the aspect ratio.
    const double scale = std::min(static_cast<double>(w) / sw, static_cast<double>(h) / sh);
    const int dw = std::max(1, static_cast<int>(sw * scale + 0.5));
    const int dh = std::max(1, static_cast<int>(sh * scale + 0.5));
    const int ox = (w - dw) / 2;
    const int oy = h - dh;
    out.pixels.assign(static_cast<size_t>(w) * h * 4, 0);
    for (int y = 0; y < dh; ++y) {
        // Source rows/columns covered by this destination pixel (at least one).
        const int y0 = y * sh / dh;
        const int y1 = std::max(y0 + 1, (y + 1) * sh / dh);
        for (int x = 0; x < dw; ++x) {
            const int x0 = x * sw / dw;
            const int x1 = std::max(x0 + 1, (x + 1) * sw / dw);
            // Box filter with premultiplied alpha, so transparent edges don't darken.
            uint64_t b = 0, g = 0, r = 0, a = 0, n = 0;
            for (int sy = y0; sy < y1; ++sy) {
                const uint8_t* row = src.pixels.data() + static_cast<size_t>(sy) * sw * 4;
                for (int sx = x0; sx < x1; ++sx) {
                    const uint8_t* p = row + sx * 4;
                    b += p[0] * p[3];
                    g += p[1] * p[3];
                    r += p[2] * p[3];
                    a += p[3];
                    ++n;
                }
            }
            uint8_t* d = out.pixels.data() + (static_cast<size_t>(oy + y) * w + ox + x) * 4;
            if (a > 0) {
                d[0] = static_cast<uint8_t>(b / a);
                d[1] = static_cast<uint8_t>(g / a);
                d[2] = static_cast<uint8_t>(r / a);
            }
            d[3] = static_cast<uint8_t>(a / n);
        }
    }
    out.desc = lv_image_dsc_t{};
    out.desc.header.magic = LV_IMAGE_HEADER_MAGIC;
    out.desc.header.cf = LV_COLOR_FORMAT_ARGB8888;
    out.desc.header.w = w;
    out.desc.header.h = h;
    out.desc.header.stride = w * 4;
    out.desc.data_size = static_cast<uint32_t>(out.pixels.size());
    out.desc.data = out.pixels.data();
    out.loaded = true;
    return true;
}

void load_navi_picture(const std::filesystem::path& fallback, int w, int h, ImageAsset& out) {
    std::string picture = model::navi().picture;
    if (!picture.empty() && picture[0] == '~') {
        if (const char* home = std::getenv("HOME")) picture = std::string(home) + picture.substr(1);
    }
    if (!picture.empty() && load_png_fit(picture, w, h, out)) return;
    if (!picture.empty()) LOG_WARN("[navi] picture {} could not be read; using the packaged art", picture);
    load_png(fallback, out);
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

lv_obj_t* tap_target(lv_obj_t* parent, int x, int y, int w, int h, int steps, bool horizontal) {
    lv_obj_t* obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_USER_1);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    // steps in the high bits, axis in bit 0 (offset keeps the value non-null).
    const intptr_t tag = (static_cast<intptr_t>(steps + 4096) << 1) | (horizontal ? 1 : 0);
    lv_obj_set_user_data(obj, reinterpret_cast<void*>(tag));
    return obj;
}

bool find_tap_target(lv_obj_t* root, int x, int y, int& steps, bool& horizontal) {
    if (!root || lv_obj_has_flag(root, LV_OBJ_FLAG_HIDDEN)) return false;
    lv_area_t a;
    lv_obj_get_coords(root, &a);
    const bool inside = x >= a.x1 && x <= a.x2 && y >= a.y1 && y <= a.y2;
    if (inside && lv_obj_has_flag(root, LV_OBJ_FLAG_USER_1)) {
        const intptr_t tag = reinterpret_cast<intptr_t>(lv_obj_get_user_data(root));
        steps = static_cast<int>(tag >> 1) - 4096;
        horizontal = (tag & 1) != 0;
        return true;
    }
    // Children may overflow their parent, so search them regardless (topmost first).
    for (int32_t i = static_cast<int32_t>(lv_obj_get_child_count(root)) - 1; i >= 0; --i) {
        if (find_tap_target(lv_obj_get_child(root, i), x, y, steps, horizontal)) return true;
    }
    return false;
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
