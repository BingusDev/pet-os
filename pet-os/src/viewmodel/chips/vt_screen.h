/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace viewmodel::chips {

// A small VT100-ish screen for the TERMINAL chip: enough for shells, ls, top
// and less on a 45x11 grid. Handles CR/LF/BS/TAB, CSI cursor moves, erase,
// scroll regions, insert/delete line, save/restore cursor and alt-screen
// switches; colours and other SGR attributes are parsed and ignored (the
// screen is monochrome green per mockups/comms.html). UTF-8 is kept per cell.
class VtScreen {
public:
    VtScreen(size_t cols, size_t rows);

    void feed(const std::string& bytes);
    void clear();

    size_t cols() const { return cols_; }
    size_t rows() const { return rows_; }
    size_t cursor_col() const { return cx_; }
    size_t cursor_row() const { return cy_; }
    bool cursor_visible() const { return cursor_visible_; }
    // Row text with trailing spaces trimmed.
    std::string row_text(size_t row) const;
    // Lines that scrolled off the top (newest last), for scrollback.
    const std::vector<std::string>& history() const { return history_; }

private:
    using Row = std::vector<std::string>;  // one UTF-8 code point per cell

    void put(const std::string& cp);
    void newline();
    void scroll_up(size_t top, size_t bottom, size_t n);
    void scroll_down(size_t top, size_t bottom, size_t n);
    void csi(char final_byte);
    void erase(size_t row, size_t from, size_t to);
    int param(size_t index, int fallback) const;

    size_t cols_;
    size_t rows_;
    std::vector<Row> grid_;
    std::vector<std::string> history_;
    size_t cx_{0};
    size_t cy_{0};
    size_t saved_cx_{0};
    size_t saved_cy_{0};
    size_t top_{0};
    size_t bottom_;
    bool wrap_pending_{false};
    bool cursor_visible_{true};

    enum class Mode { Text, Escape, Csi, Osc, Charset } mode_{Mode::Text};
    std::string seq_;
    std::string utf8_;
    size_t utf8_need_{0};
};

} // namespace viewmodel::chips
