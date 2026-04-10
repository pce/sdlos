#include "grid_panel.h"

#include <any>
#include <cassert>
#include <cmath>
#include <memory>

namespace pce::sdlos::widgets {

namespace {

// Absolute position of a node (walk to root).
static std::pair<float, float> absPos(const RenderTree &t, NodeHandle h) noexcept {
    float ax = 0.f, ay = 0.f;
    for (NodeHandle cur = h; cur.valid();) {
        const RenderNode *n = t.node(cur);
        if (!n)
            break;
        ax  += n->x;
        ay  += n->y;
        cur  = n->parent;
    }
    return {ax, ay};
}

// Map pixel position to flat cell index (1D).
// Returns SIZE_MAX if outside the grid or beyond the value array.
static std::size_t
hitTest1D(const GridPanelState &s, float ax, float ay, float mx, float my) noexcept {
    const float lx = mx - ax;
    const float ly = my - ay;
    if (lx < 0.f || ly < 0.f)
        return SIZE_MAX;

    const float cw = s.cfg.cell_w + s.cfg.gap;
    const float ch = s.cfg.cell_h + s.cfg.gap;
    const int col  = static_cast<int>(lx / cw);
    const int row  = static_cast<int>(ly / ch);

    if (col < 0 || col >= static_cast<int>(s.cfg.cols))
        return SIZE_MAX;
    if (row < 0 || row >= static_cast<int>(s.cfg.rows))
        return SIZE_MAX;

    const std::size_t idx = static_cast<std::size_t>(row) * static_cast<std::size_t>(s.cfg.cols)
                          + static_cast<std::size_t>(col);
    return (idx < s.values.size()) ? idx : SIZE_MAX;
}

// 2D hit test: returns (row, col) or (SIZE_MAX, SIZE_MAX) if outside.
static std::pair<std::size_t, std::size_t>
hitTest2D(const GridPanelState &s, float ax, float ay, float mx, float my) noexcept {
    constexpr std::size_t kNone = SIZE_MAX;

    const float lx = mx - ax;
    const float ly = my - ay;
    if (lx < 0.f || ly < 0.f)
        return {kNone, kNone};

    const float cw = s.cfg.cell_w + s.cfg.gap;
    const float ch = s.cfg.cell_h + s.cfg.gap;
    const int c    = static_cast<int>(lx / cw);
    const int r    = static_cast<int>(ly / ch);

    if (c < 0 || c >= static_cast<int>(s.cfg.cols))
        return {kNone, kNone};
    if (r < 0 || r >= static_cast<int>(s.cfg.rows))
        return {kNone, kNone};
    if (static_cast<std::size_t>(c) >= s.col_values.size())
        return {kNone, kNone};
    if (static_cast<std::size_t>(r) >= s.row_values.size())
        return {kNone, kNone};

    return {static_cast<std::size_t>(r), static_cast<std::size_t>(c)};
}

/// drawCell1D  —  draw a single cell in 1D grid mode with text label
///
// Handles background color (selected/hovered/normal) and center-aligned label.
// Reusable for any 1D grid-like widget.
static void
drawCell1D(RenderContext &ctx, const GridPanelState &s, std::size_t idx, float cx, float cy) {
    const float cw = s.cfg.cell_w;
    const float ch = s.cfg.cell_h;

    // Determine colors based on selection state
    const bool sel     = (idx == s.selected_idx);
    const bool hovered = (idx == s.hovered_idx);
    const Color &bg    = sel ? s.cfg.bg_selected : hovered ? s.cfg.bg_hover : s.cfg.bg_cell;
    const Color &tc    = sel ? s.cfg.text_selected : s.cfg.text_color;

    // Draw cell background
    ctx.drawRect(cx, cy, cw, ch, bg.r, bg.g, bg.b, bg.a);

    // Draw centered text label
    const std::string_view lbl = s.label_for(idx);
    const float tw =
        ctx.text_renderer
            ? static_cast<float>(
                  ctx.text_renderer->measureText(std::string(lbl), s.cfg.font_size).first)
            : static_cast<float>(lbl.size()) * s.cfg.font_size * 0.6f;

    ctx.drawText(
        std::string(lbl),
        cx + (cw - tw) * 0.5f,
        cy + (ch - s.cfg.font_size) * 0.5f,
        s.cfg.font_size,
        tc.r,
        tc.g,
        tc.b,
        tc.a);
}

/// drawCell2D  —  draw a single cell in 2D matrix mode with crosshair indicator
///
// Handles background color (selected/hovered/normal) and selection crosshair.
// Reusable for any 2D grid-like widget.
static void drawCell2D(
    RenderContext &ctx,
    const GridPanelState &s,
    std::size_t row,
    std::size_t col,
    float cx,
    float cy) {
    const float cw = s.cfg.cell_w;
    const float ch = s.cfg.cell_h;

    // Determine colors based on selection state
    const bool sel     = (row == s.selected_row && col == s.selected_col);
    const bool hovered = (row == s.hovered_row && col == s.hovered_col);
    const Color &bg    = sel ? s.cfg.bg_selected : hovered ? s.cfg.bg_hover : s.cfg.bg_cell;
    const Color &dc    = sel ? s.cfg.text_selected : s.cfg.text_color;

    // Draw cell background
    ctx.drawRect(cx, cy, cw, ch, bg.r, bg.g, bg.b, bg.a);

    // Draw center dot / crosshair as selection indicator
    ctx.drawRect(cx + cw * 0.5f - 2.f, cy + ch * 0.5f - 2.f, 4.f, 4.f, dc.r, dc.g, dc.b, 0.8f);
}

/// drawGrid1D  —  draw entire 1D flat grid
static void drawGrid1D(RenderContext &ctx, const GridPanelState &s, float ax, float ay) {
    const float cw  = s.cfg.cell_w;
    const float ch  = s.cfg.cell_h;
    const float gap = s.cfg.gap;

    for (std::size_t i = 0; i < s.values.size(); ++i) {
        const std::size_t row = i / static_cast<std::size_t>(s.cfg.cols);
        const std::size_t col = i % static_cast<std::size_t>(s.cfg.cols);

        const float cx = ax + static_cast<float>(col) * (cw + gap);
        const float cy = ay + static_cast<float>(row) * (ch + gap);

        drawCell1D(ctx, s, i, cx, cy);
    }
}

/// drawGrid2D  —  draw entire 2D matrix grid
static void drawGrid2D(RenderContext &ctx, const GridPanelState &s, float ax, float ay) {
    const float cw  = s.cfg.cell_w;
    const float ch  = s.cfg.cell_h;
    const float gap = s.cfg.gap;

    for (std::size_t r = 0; r < s.row_values.size(); ++r) {
        for (std::size_t c = 0; c < s.col_values.size(); ++c) {
            const float cx = ax + static_cast<float>(c) * (cw + gap);
            const float cy = ay + static_cast<float>(r) * (ch + gap);

            drawCell2D(ctx, s, r, c, cx, cy);
        }
    }
}

static void installCallbacks(RenderTree &tree, NodeHandle h, std::shared_ptr<GridPanelState> st) {
    RenderNode *n = tree.node(h);
    if (!n)
        return;

    // Draw callback — delegates to extracted helper functions
    n->draw = [st, h, &tree](RenderContext &ctx) {
        GridPanelState &s      = *st;
        const RenderNode *self = tree.node(h);
        if (!self)
            return;

        const auto [ax, ay] = absPos(tree, h);

        if (!s.is_2d) {
            drawGrid1D(ctx, s, ax, ay);
        } else {
            drawGrid2D(ctx, s, ax, ay);
        }
    };

    // keep dirty while hovered
    //     -> so cursor changes repaint
    n->update = [st, h, &tree]() {
        const bool needs = (st->hovered_idx != SIZE_MAX) || (st->hovered_row != SIZE_MAX);
        if (needs)
            if (RenderNode *self = tree.node(h))
                self->dirty_render = true;
    };

    n->dirty_render = true;
}

}  // namespace

/// Factory
GridPanel makeGridPanel(RenderTree &tree, GridPanelConfig cfg) {
    const NodeHandle h = tree.alloc();
    RenderNode *n      = tree.node(h);
    assert(n);

    auto st = std::make_shared<GridPanelState>();
    st->cfg = cfg;  // copy config (spans are non-owning; values are copied below)

    // Copy span contents into owned vectors
    st->values.assign(cfg.values.begin(), cfg.values.end());

    st->labels.reserve(cfg.labels.size());
    for (const char *lbl : cfg.labels)
        st->labels.emplace_back(lbl ? lbl : "");

    st->col_values.assign(cfg.col_values.begin(), cfg.col_values.end());
    st->row_values.assign(cfg.row_values.begin(), cfg.row_values.end());

    st->col_labels.reserve(cfg.col_labels.size());
    for (const char *lbl : cfg.col_labels)
        st->col_labels.emplace_back(lbl ? lbl : "");

    st->row_labels.reserve(cfg.row_labels.size());
    for (const char *lbl : cfg.row_labels)
        st->row_labels.emplace_back(lbl ? lbl : "");

    // Determine mode: 2D if both col_values and row_values were supplied
    st->is_2d = !st->col_values.empty() && !st->row_values.empty();

    // Clamp initial selection indices to valid range
    if (!st->is_2d) {
        st->selected_idx =
            st->values.empty() ? std::size_t(0) : std::min(cfg.initial_idx, st->values.size() - 1);
    } else {
        st->selected_row = st->row_values.empty()
                             ? std::size_t(0)
                             : std::min(cfg.initial_row, st->row_values.size() - 1);
        st->selected_col = st->col_values.empty()
                             ? std::size_t(0)
                             : std::min(cfg.initial_col, st->col_values.size() - 1);
    }

    // Compute layout size from cell dimensions and count
    const uint8_t cols = std::max<uint8_t>(1u, cfg.cols);
    const uint8_t rows = std::max<uint8_t>(1u, cfg.rows);
    n->w               = cols * cfg.cell_w + (cols - 1) * cfg.gap;
    n->h               = rows * cfg.cell_h + (rows - 1) * cfg.gap;
    n->layout_kind     = LayoutKind::None;

    // Notify signal with initial value
    if (cfg.value_signal)
        cfg.value_signal->set(st->current_value());

    // Store state and wire callbacks
    n->state = st;
    installCallbacks(tree, h, st);

    return GridPanel{tree, h};
}

/// View methods
float GridPanel::getValue() const noexcept {
    const auto *s = getState();
    return s ? s->current_value() : 0.f;
}

/**
 * @brief Returns value2d
 *
 * @return Integer result; negative values indicate an error code
 */
std::pair<float, float> GridPanel::getValue2D() const noexcept {
    const auto *s = getState();
    return s ? std::make_pair(s->current_x(), s->current_y()) : std::make_pair(0.f, 0.f);
}

/**
 * @brief Select by value
 *
 * @param value  Operand value
 */
void GridPanel::selectByValue(float value) {
    auto *s = getState();
    if (!s || s->is_2d)
        return;
    if (s->values.empty())
        return;

    // Find the cell whose value is closest to the requested value.
    std::size_t best = 0;
    float best_dist  = std::abs(s->values[0] - value);
    for (std::size_t i = 1; i < s->values.size(); ++i) {
        const float d = std::abs(s->values[i] - value);
        if (d < best_dist) {
            best_dist = d;
            best      = i;
        }
    }

    if (best == s->selected_idx)
        return;
    s->selected_idx = best;

    if (s->cfg.value_signal)
        s->cfg.value_signal->set(s->current_value());
    if (s->cfg.on_change)
        s->cfg.on_change(s->current_value());
    if (RenderNode *n = tree.node(handle))
        n->dirty_render = true;
}

/**
 * @brief Select by value2d
 *
 * @param x_value  Operand value
 * @param y_value  Operand value
 */
void GridPanel::selectByValue2D(float x_value, float y_value) {
    auto *s = getState();
    if (!s || !s->is_2d)
        return;

    // Find the closest column and row independently.
    auto closest = [](const std::vector<float> &v, float target) -> std::size_t {
        if (v.empty())
            return 0;
        std::size_t best = 0;
        float bd         = std::abs(v[0] - target);
        for (std::size_t i = 1; i < v.size(); ++i) {
            const float d = std::abs(v[i] - target);
            if (d < bd) {
                bd   = d;
                best = i;
            }
        }
        return best;
    };

    s->selected_col = closest(s->col_values, x_value);
    s->selected_row = closest(s->row_values, y_value);

    if (s->cfg.on_change_2d)
        s->cfg.on_change_2d(s->current_x(), s->current_y());
    if (RenderNode *n = tree.node(handle))
        n->dirty_render = true;
}

/**
 * @brief Handles event
 *
 * @param ev  SDL3 input or window event
 *
 * @return true on success, false on failure
 */
bool GridPanel::handleEvent(const SDL_Event &ev) {
    auto *s       = getState();
    RenderNode *n = tree.node(handle);
    if (!s || !n)
        return false;

    const auto [ax, ay] = absPos(tree, handle);

    switch (ev.type) {
    case SDL_EVENT_MOUSE_MOTION: {
        // Update hover state — never consumes the event so other widgets
        // can see it too (e.g. a NumberDragger in the same panel).
        if (!s->is_2d) {
            const std::size_t idx = hitTest1D(*s, ax, ay, ev.motion.x, ev.motion.y);
            if (idx != s->hovered_idx) {
                s->hovered_idx  = idx;
                n->dirty_render = true;
            }
        } else {
            const auto [r, c] = hitTest2D(*s, ax, ay, ev.motion.x, ev.motion.y);
            if (r != s->hovered_row || c != s->hovered_col) {
                s->hovered_row  = r;
                s->hovered_col  = c;
                n->dirty_render = true;
            }
        }
        return false;
    }

    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        if (ev.button.button != SDL_BUTTON_LEFT)
            return false;

        if (!s->is_2d) {
            const std::size_t idx = hitTest1D(*s, ax, ay, ev.button.x, ev.button.y);
            if (idx == SIZE_MAX)
                return false;

            if (idx != s->selected_idx) {
                s->selected_idx = idx;
                if (s->cfg.value_signal)
                    s->cfg.value_signal->set(s->current_value());
                if (s->cfg.on_change)
                    s->cfg.on_change(s->current_value());
                n->dirty_render = true;
            }
            return true;  // consumed — click landed inside the grid
        } else {
            const auto [r, c] = hitTest2D(*s, ax, ay, ev.button.x, ev.button.y);
            if (r == SIZE_MAX)
                return false;

            if (r != s->selected_row || c != s->selected_col) {
                s->selected_row = r;
                s->selected_col = c;
                if (s->cfg.on_change_2d)
                    s->cfg.on_change_2d(s->current_x(), s->current_y());
                n->dirty_render = true;
            }
            return true;
        }
    }

    default:
        break;
    }

    return false;
}

}  // namespace pce::sdlos::widgets
