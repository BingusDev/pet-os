/*
 * SPDX-License-Identifier: MIT
 */

#include "viewmodel/chips/vt_screen.h"

#include <algorithm>
#include <cstdlib>

namespace viewmodel::chips {
namespace {
constexpr size_t kHistoryLines = 300;
}

VtScreen::VtScreen(size_t cols, size_t rows) : cols_(cols), rows_(rows), bottom_(rows - 1) {
    clear();
}

void VtScreen::clear() {
    grid_.assign(rows_, Row(cols_, " "));
    cx_ = cy_ = 0;
    top_ = 0;
    bottom_ = rows_ - 1;
    wrap_pending_ = false;
}

std::string VtScreen::row_text(size_t row) const {
    if (row >= rows_) return {};
    std::string out;
    size_t last = 0;
    for (size_t c = 0; c < cols_; ++c) {
        if (grid_[row][c] != " ") last = c + 1;
    }
    for (size_t c = 0; c < last; ++c) out += grid_[row][c];
    return out;
}

void VtScreen::scroll_up(size_t top, size_t bottom, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        if (top == 0 && bottom == rows_ - 1) {
            history_.push_back(row_text(0));
            if (history_.size() > kHistoryLines) history_.erase(history_.begin());
        }
        grid_.erase(grid_.begin() + static_cast<long>(top));
        grid_.insert(grid_.begin() + static_cast<long>(bottom), Row(cols_, " "));
    }
}

void VtScreen::scroll_down(size_t top, size_t bottom, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        grid_.erase(grid_.begin() + static_cast<long>(bottom));
        grid_.insert(grid_.begin() + static_cast<long>(top), Row(cols_, " "));
    }
}

void VtScreen::newline() {
    if (cy_ == bottom_) {
        scroll_up(top_, bottom_, 1);
    } else if (cy_ + 1 < rows_) {
        ++cy_;
    }
}

void VtScreen::put(const std::string& cp) {
    if (wrap_pending_) {
        cx_ = 0;
        newline();
        wrap_pending_ = false;
    }
    grid_[cy_][cx_] = cp;
    if (cx_ + 1 >= cols_) {
        wrap_pending_ = true;
    } else {
        ++cx_;
    }
}

void VtScreen::erase(size_t row, size_t from, size_t to) {
    for (size_t c = from; c < to && c < cols_; ++c) grid_[row][c] = " ";
}

int VtScreen::param(size_t index, int fallback) const {
    // seq_ holds the CSI parameter bytes, e.g. "?25" or "1;5".
    std::string body = seq_;
    if (!body.empty() && (body[0] == '?' || body[0] == '>' || body[0] == '=')) body.erase(0, 1);
    size_t start = 0;
    for (size_t i = 0; i < index; ++i) {
        start = body.find(';', start);
        if (start == std::string::npos) return fallback;
        ++start;
    }
    const size_t end = body.find(';', start);
    const std::string field = body.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (field.empty()) return fallback;
    return std::atoi(field.c_str());
}

void VtScreen::csi(char f) {
    const bool private_mode = !seq_.empty() && seq_[0] == '?';
    const int n = std::max(1, param(0, 1));
    wrap_pending_ = false;
    switch (f) {
    case 'A': cy_ = cy_ >= static_cast<size_t>(n) ? cy_ - n : 0; break;
    case 'B': cy_ = std::min(rows_ - 1, cy_ + n); break;
    case 'C': cx_ = std::min(cols_ - 1, cx_ + n); break;
    case 'D': cx_ = cx_ >= static_cast<size_t>(n) ? cx_ - n : 0; break;
    case 'E': cy_ = std::min(rows_ - 1, cy_ + n); cx_ = 0; break;
    case 'F': cy_ = cy_ >= static_cast<size_t>(n) ? cy_ - n : 0; cx_ = 0; break;
    case 'G': cx_ = std::min(cols_ - 1, static_cast<size_t>(n - 1)); break;
    case 'd': cy_ = std::min(rows_ - 1, static_cast<size_t>(n - 1)); break;
    case 'H':
    case 'f':
        cy_ = std::min(rows_ - 1, static_cast<size_t>(std::max(1, param(0, 1)) - 1));
        cx_ = std::min(cols_ - 1, static_cast<size_t>(std::max(1, param(1, 1)) - 1));
        break;
    case 'J': {
        const int mode = param(0, 0);
        if (mode == 0) {
            erase(cy_, cx_, cols_);
            for (size_t r = cy_ + 1; r < rows_; ++r) erase(r, 0, cols_);
        } else if (mode == 1) {
            erase(cy_, 0, cx_ + 1);
            for (size_t r = 0; r < cy_; ++r) erase(r, 0, cols_);
        } else {
            for (size_t r = 0; r < rows_; ++r) erase(r, 0, cols_);
        }
        break;
    }
    case 'K': {
        const int mode = param(0, 0);
        if (mode == 0) erase(cy_, cx_, cols_);
        else if (mode == 1) erase(cy_, 0, cx_ + 1);
        else erase(cy_, 0, cols_);
        break;
    }
    case 'X': erase(cy_, cx_, cx_ + n); break;
    case 'P': {
        auto& row = grid_[cy_];
        for (int i = 0; i < n && cx_ < cols_; ++i) {
            row.erase(row.begin() + static_cast<long>(cx_));
            row.push_back(" ");
        }
        break;
    }
    case '@': {
        auto& row = grid_[cy_];
        for (int i = 0; i < n && cx_ < cols_; ++i) {
            row.insert(row.begin() + static_cast<long>(cx_), " ");
            row.pop_back();
        }
        break;
    }
    case 'L': if (cy_ >= top_ && cy_ <= bottom_) scroll_down(cy_, bottom_, n); break;
    case 'M': if (cy_ >= top_ && cy_ <= bottom_) scroll_up(cy_, bottom_, n); break;
    case 'S': scroll_up(top_, bottom_, n); break;
    case 'T': scroll_down(top_, bottom_, n); break;
    case 'r': {
        const size_t t = static_cast<size_t>(std::max(1, param(0, 1)) - 1);
        const size_t b = static_cast<size_t>(std::max(1, param(1, static_cast<int>(rows_))) - 1);
        if (t < b && b < rows_) {
            top_ = t;
            bottom_ = b;
        }
        cx_ = cy_ = 0;
        break;
    }
    case 's': saved_cx_ = cx_; saved_cy_ = cy_; break;
    case 'u': cx_ = saved_cx_; cy_ = saved_cy_; break;
    case 'h':
    case 'l':
        if (private_mode) {
            const int mode = param(0, 0);
            if (mode == 25) cursor_visible_ = f == 'h';
            // 1049/47/1047: alternate screen. We keep one buffer and clear it on switch.
            if (mode == 1049 || mode == 47 || mode == 1047) {
                for (size_t r = 0; r < rows_; ++r) erase(r, 0, cols_);
                if (f == 'l') { cx_ = saved_cx_; cy_ = saved_cy_; } else { saved_cx_ = cx_; saved_cy_ = cy_; }
            }
        }
        break;
    default:
        break;  // SGR 'm', device status, etc.: ignored
    }
}

void VtScreen::feed(const std::string& bytes) {
    for (const char ch : bytes) {
        const auto c = static_cast<unsigned char>(ch);
        switch (mode_) {
        case Mode::Escape:
            if (c == '[') {
                mode_ = Mode::Csi;
                seq_.clear();
            } else if (c == ']') {
                mode_ = Mode::Osc;
            } else if (c == '(' || c == ')') {
                mode_ = Mode::Charset;
            } else {
                if (c == '7') { saved_cx_ = cx_; saved_cy_ = cy_; }
                else if (c == '8') { cx_ = saved_cx_; cy_ = saved_cy_; }
                else if (c == 'D') newline();
                else if (c == 'E') { cx_ = 0; newline(); }
                else if (c == 'M') { if (cy_ == top_) scroll_down(top_, bottom_, 1); else if (cy_ > 0) --cy_; }
                else if (c == 'c') clear();
                mode_ = Mode::Text;
            }
            continue;
        case Mode::Csi:
            if (c >= 0x40 && c <= 0x7e) {
                csi(static_cast<char>(c));
                mode_ = Mode::Text;
            } else if (seq_.size() < 32) {
                seq_ += static_cast<char>(c);
            }
            continue;
        case Mode::Osc:
            if (c == 0x07) mode_ = Mode::Text;
            else if (c == 0x1b) mode_ = Mode::Escape;  // ESC \ terminator
            continue;
        case Mode::Charset:
            mode_ = Mode::Text;
            continue;
        case Mode::Text:
            break;
        }

        if (utf8_need_ > 0) {
            if ((c & 0xC0) == 0x80) {
                utf8_ += static_cast<char>(c);
                if (--utf8_need_ == 0) put(utf8_);
                continue;
            }
            utf8_need_ = 0;  // malformed: drop and reprocess this byte
        }
        if (c >= 0xC0) {
            utf8_.assign(1, static_cast<char>(c));
            utf8_need_ = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
            continue;
        }
        switch (c) {
        case 0x1b: mode_ = Mode::Escape; break;
        case '\r': cx_ = 0; wrap_pending_ = false; break;
        case '\n':
        case 0x0b:
        case 0x0c: newline(); wrap_pending_ = false; break;
        case '\b': if (cx_ > 0) --cx_; wrap_pending_ = false; break;
        case '\t': cx_ = std::min(cols_ - 1, (cx_ / 8 + 1) * 8); break;
        case 0x07: break;
        default:
            if (c >= 0x20 && c < 0x7f) put(std::string(1, static_cast<char>(c)));
            break;
        }
    }
}

} // namespace viewmodel::chips
