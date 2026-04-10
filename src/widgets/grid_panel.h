#pragma once
// GridPanel widget
//
// A flat NxM button grid that maps visual cells to real float values.
// Designed for quantized param selection: octave counts, SDF step presets,
// frequency tiers — anything where discrete labeled buttons are more useful
// than a continuous dragger.
//
// 1D usage (single row of N value buttons):
//
//   static constexpr float kOctaves[] = {1,2,3,4,5,6,7,8};
//   auto gp = makeGridPanel(tree, {
//       .values  = kOctaves,
//       .cols    = 8,
//       .rows    = 1,
//       .initial_idx = 3,     // selects the cell whose value == 4
//       .on_change = [](float v) { /* v is the real float, e.g. 4.0 */ },
//   });
//
// 2D usage (row × col = two independent params):
//
//   static constexpr float kScales[]  = {0.5f, 1.f, 2.f, 4.f};
//   static constexpr float kOcts[]    = {2.f,  4.f, 6.f, 8.f};
//   auto gp = makeGridPanel(tree, {
//       .col_values    = kScales,   // X axis
//       .row_values    = kOcts,     // Y axis
//       .cols          = 4,
//       .rows          = 4,
//       .on_change_2d  = [](float scale, float oct) { ... },
//   });
//
// State tracking:
//   The widget stores selected_idx (1D) or selected_row/selected_col (2D).
//   current_value()   → the REAL float corresponding to selected_idx
//   current_x_value() → col_values[selected_col]  (2D only)
//   current_y_value() → row_values[selected_row]  (2D only)
//
//   The behavior should read current_value() (not selected_idx) when building
//   the pipeline.pug string — the index is visual state, the value is the param.
//

#include "../render_tree.h"
#include "widget.h"

#include <SDL3/SDL.h>

#include <functional>
#include <span>
#include <string>
#include <vector>

namespace pce::sdlos::widgets {

struct GridPanelConfig {
    // 1D mode
    // Set `values` for a flat list of N buttons (single row or wrapped rows).
    // The cell at index i has value values[i] and displays labels[i] if present.
    std::span<const float> values;        // e.g. {1,2,3,4,5,6,7,8} octaves
    std::span<const char *const> labels;  // optional; falls back to fmtVal(values[i])

    // 2D mode
    // Set both col_values AND row_values for a matrix picker.
    // Selecting cell (r, c) fires on_change_2d(col_values[c], row_values[r]).
    std::span<const float> col_values;  // X axis (columns)
    std::span<const float> row_values;  // Y axis (rows)
    std::span<const char *const> col_labels;
    std::span<const char *const> row_labels;

    /// Layout
    uint8_t cols = 8;  // cells per row
    uint8_t rows = 1;  // number of rows
    float cell_w = 32.f;
    float cell_h = 28.f;
    float gap    = 2.f;

    // Initial selection
    /// 1D: index of initially selected cell
    std::size_t initial_idx = 0;
    /// 2D: initial row / col
    std::size_t initial_row = 0;
    std::size_t initial_col = 0;

    // Palette
    Color bg_cell       = Color::hex(0x2c, 0x2c, 0x2e, 0xff);
    Color bg_selected   = Color::hex(0x63, 0x66, 0xf1, 0xff);
    Color bg_hover      = Color::hex(0x3a, 0x3a, 0x3c, 0xff);
    Color text_color    = Color::hex(0xcc, 0xcc, 0xcc, 0xff);
    Color text_selected = Color::hex(0xff, 0xff, 0xff, 0xff);
    float font_size     = 11.f;
    float border_radius = 4.f;  // informational; used by drawRect fallback

    // Two-way binding
    Signal<float> *value_signal = nullptr;

    // Callbacks
    /// 1D: value is values[selected_idx]
    std::function<void(float value)> on_change;
    /// 2D: x = col_values[selected_col], y = row_values[selected_row]
    std::function<void(float x_value, float y_value)> on_change_2d;
};

// ============================================================================
// GridPanelState
// ============================================================================
struct GridPanelState {
    // Owned copies of the value/label arrays (from Config spans):
    std::vector<float> values;
    std::vector<std::string> labels;
    std::vector<float> col_values;
    std::vector<float> row_values;
    std::vector<std::string> col_labels;
    std::vector<std::string> row_labels;

    bool is_2d = false;

    // 1D selection:
    std::size_t selected_idx = 0;
    std::size_t hovered_idx  = SIZE_MAX;

    // 2D selection:
    std::size_t selected_row = 0;
    std::size_t selected_col = 0;
    std::size_t hovered_row  = SIZE_MAX;
    std::size_t hovered_col  = SIZE_MAX;

    GridPanelConfig cfg;

    // Value accessors
    // ALWAYS use these in behavior code — not selected_idx directly.
    // selected_idx is visual state; current_value() is the param value.

    /// The real float value for the current 1D selection.
    [[nodiscard]]
    float current_value() const noexcept {
        if (values.empty())
            return 0.f;
        const std::size_t i = std::min(selected_idx, values.size() - 1);
        return values[i];
    }

    /// X (column) value for 2D selection.
    [[nodiscard]]
    float current_x() const noexcept {
        if (col_values.empty())
            return 0.f;
        return col_values[std::min(selected_col, col_values.size() - 1)];
    }

    /// Y (row) value for 2D selection.
    [[nodiscard]]
    float current_y() const noexcept {
        if (row_values.empty())
            return 0.f;
        return row_values[std::min(selected_row, row_values.size() - 1)];
    }

    /// Display label for cell i (1D).
    [[nodiscard]]
    std::string_view label_for(std::size_t i) const noexcept {
        if (i < labels.size() && !labels[i].empty())
            return labels[i];
        if (i < values.size()) {
            // Format the float value: strip trailing zeros
            static thread_local char buf[16];
            const float v = values[i];
            if (v == std::floor(v))
                std::snprintf(buf, sizeof(buf), "%.0f", static_cast<double>(v));
            else
                std::snprintf(buf, sizeof(buf), "%.2g", static_cast<double>(v));
            return buf;
        }
        return "";
    }

    /// Total computed widget width.
    [[nodiscard]]
    float total_w() const noexcept {
        return cfg.cols * cfg.cell_w + (cfg.cols - 1) * cfg.gap;
    }
    /// Total computed widget height.
    [[nodiscard]]
    float total_h() const noexcept {
        return cfg.rows * cfg.cell_h + (cfg.rows - 1) * cfg.gap;
    }
};

// GridPanel — lightweight non-owning view

struct GridPanel : WidgetView<GridPanelState> {
    /// Current value (1D).
    [[nodiscard]]
    float getValue() const noexcept;

    /// Current (x, y) values (2D).
    [[nodiscard]]
    std::pair<float, float> getValue2D() const noexcept;

    /// Set selection by value — finds the closest matching cell.
    void selectByValue(float value);

    /// Set 2D selection by (x, y) values.
    void selectByValue2D(float x_value, float y_value);

    /// Feed a raw SDL event. Returns true when consumed.
    bool handleEvent(const SDL_Event &ev);
};

/// Factory.
[[nodiscard]]
GridPanel makeGridPanel(RenderTree &tree, GridPanelConfig cfg);

}  // namespace pce::sdlos::widgets
