// Part of sdlos — SDL3 GPU render core.
//
// SelectBox — dropdown option picker
//
// The dropdown is drawn inline by the draw() callback (no extra tree nodes).
// When open the dropdown panel "overflows" the node bounds downward; it
// renders on top of siblings that appear earlier in tree order.  Append the
// SelectBox last among siblings when visual overlap matters.

#include "select_box.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <any>
#include <cassert>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

namespace pce::sdlos::widgets {

// Anonymous-namespace helpers
namespace {

void drawArrow(RenderContext& ctx,
               float cx, float cy, float size,
               bool  pointing_up,
               const Color& c) noexcept
{
    // Three horizontal lines of decreasing width simulate a triangle.
    // Closed: wide bar on top, narrow on bottom.
    // Open: narrow bar on top, wide on bottom.
    const int steps = static_cast<int>(size / 2.f);
    for (int i = 0; i < steps; ++i) {
        const float frac  = static_cast<float>(i) / static_cast<float>(steps);
        const float half  = pointing_up
                            ? size * 0.5f * frac           // grows downward
                            : size * 0.5f * (1.f - frac);  // shrinks downward
        const float row_y = cy + static_cast<float>(i) * 2.f;
        ctx.drawRect(cx - half, row_y, half * 2.f, 2.f,
                     c.r, c.g, c.b, c.a);
    }
}

// 1-pixel border rectangle (top / bottom / left / right strokes).
void drawBorder(RenderContext& ctx,
                float x, float y, float w, float h,
                const Color& c) noexcept
{
    ctx.drawRect(x,           y,           w,      1.f,     c.r, c.g, c.b, c.a);
    ctx.drawRect(x,           y + h - 1.f, w,      1.f,     c.r, c.g, c.b, c.a);
    ctx.drawRect(x,           y + 1.f,     1.f,    h - 2.f, c.r, c.g, c.b, c.a);
    ctx.drawRect(x + w - 1.f, y + 1.f,    1.f,    h - 2.f, c.r, c.g, c.b, c.a);
}

// Hit-test a local-space point against the open dropdown panel.
// Returns SIZE_MAX when the point is outside the panel.
[[nodiscard]] std::size_t
hitTestDropdown(const SelectBoxState& s, float lx, float ly) noexcept
{
    const float panel_y = s.cfg.h;  // panel starts right below the control
    const float panel_w = s.cfg.w;
    const float panel_h = s.dropdownH();

    if (lx < 0.f || lx > panel_w) return SIZE_MAX;
    if (ly < panel_y || ly > panel_y + panel_h) return SIZE_MAX;

    const std::size_t idx =
        static_cast<std::size_t>((ly - panel_y) / s.cfg.item_h);
    return (idx < s.options.size()) ? idx : SIZE_MAX;
}

} // anonymous namespace


/// Factory
SelectBox makeSelectBox(RenderTree& tree, SelectBoxConfig cfg)
{
    const NodeHandle h = tree.alloc();
    RenderNode*      n = tree.node(h);
    assert(n && "makeSelectBox: alloc returned invalid handle");

    n->w            = cfg.w;
    n->h            = cfg.h;
    n->dirty_render = true;

    // Build state — copy options from the span, resolve initial selection.
    auto st = std::make_shared<SelectBoxState>();

    st->options.reserve(cfg.options.size());
    for (const auto& opt : cfg.options)
        st->options.push_back(opt);

    // Resolve initial selection by value string.
    st->selected_idx = 0;
    if (!cfg.selected.empty()) {
        for (std::size_t i = 0; i < st->options.size(); ++i) {
            if (st->options[i].value == cfg.selected) {
                st->selected_idx = i;
                break;
            }
        }
    }

    // Push initial value into the Signal if bound.
    if (cfg.value && !st->options.empty())
        cfg.value->set(st->options[st->selected_idx].value);

    st->cfg = std::move(cfg);   // moves move_only_function members too
    n->state = st;              // std::any holds a shared_ptr copy

    // TODO refactor extract draw
    n->draw = [st](RenderContext& ctx) {
        const SelectBoxState&  s = *st;
        const SelectBoxConfig& c = s.cfg;

        const float w = c.w;
        const float h = c.h;

        // Control (always visible)
        const Color& bg   = s.is_open ? c.bg_open : c.bg;
        ctx.drawRect(0.f, 0.f, w, h, bg.r, bg.g, bg.b, bg.a);

        const Color& bord = s.is_open ? c.border_open : c.border;
        drawBorder(ctx, 1.f, 1.f, w - 2.f, h - 2.f, bord);

        // Selected label
        const std::string_view lbl = s.selectedLabel();
        if (!lbl.empty()) {
            const float ty = (h - c.font_size) * 0.5f;
            ctx.drawText(lbl,
                         c.padding.left, ty,
                         c.font_size,
                         c.text_color.r, c.text_color.g,
                         c.text_color.b, c.text_color.a);
        }

        // Arrow glyph — right-aligned, vertically centred.
        {
            const float arrow_size = c.font_size * 0.5f;
            const float ax = w - c.padding.right - arrow_size;
            const float ay = (h - arrow_size) * 0.5f;
            drawArrow(ctx, ax, ay, arrow_size, s.is_open, c.arrow_color);
        }

        // Dropdown panel (only when open)
        if (!s.is_open || s.options.empty()) return;

        const float panel_y = h;               // immediately below control
        const float panel_h = s.dropdownH();

        // Panel background + border
        ctx.drawRect(0.f, panel_y, w, panel_h,
                     c.dropdown_bg.r, c.dropdown_bg.g,
                     c.dropdown_bg.b, c.dropdown_bg.a);
        drawBorder(ctx, 1.f, panel_y, w - 2.f, panel_h, c.border_open);

        // Option rows
        for (std::size_t i = 0; i < s.options.size(); ++i) {
            const float row_y = panel_y + static_cast<float>(i) * c.item_h;

            // Row background: selected > hovered > transparent
            if (i == s.selected_idx) {
                ctx.drawRect(2.f, row_y + 1.f, w - 4.f, c.item_h - 2.f,
                             c.item_selected_bg.r, c.item_selected_bg.g,
                             c.item_selected_bg.b, c.item_selected_bg.a);
            } else if (i == s.hovered_idx) {
                ctx.drawRect(2.f, row_y + 1.f, w - 4.f, c.item_h - 2.f,
                             c.item_hover_bg.r, c.item_hover_bg.g,
                             c.item_hover_bg.b, c.item_hover_bg.a);
            }

            // Row text
            const Color& tc = (i == s.selected_idx)
                                ? c.item_selected_text
                                : c.item_text;
            const float text_y = row_y + (c.item_h - c.font_size) * 0.5f;
            ctx.drawText(s.options[i].displayLabel(),
                         c.padding.left, text_y,
                         c.font_size,
                         tc.r, tc.g, tc.b, tc.a);

            // Thin separator between rows (except last)
            if (i + 1 < s.options.size()) {
                const float sep_y = row_y + c.item_h - 1.f;
                ctx.drawRect(c.padding.left, sep_y,
                             w - c.padding.left - c.padding.right, 1.f,
                             0.5f, 0.5f, 0.5f, 0.15f);
            }
        }
    };

    // SelectBox does not self-dirty every frame — it only repaints on explicit
    // state changes (open/close, hover, selection).  This keeps the idle-frame
    // optimisation working for pages with static selects.
    // Nothing to do here; left as a no-op placeholder.
    n->update = []() {};

    return SelectBox{ tree, h };
}



/**
 * @brief Returns selected
 *
 * @return Integer result; negative values indicate an error code
 */
std::string_view SelectBox::getSelected() const noexcept
{
    const SelectBoxState* s = getState();
    return s ? s->selectedValue() : std::string_view{};
}

/**
 * @brief Select
 *
 * @param value  Operand value
 */
void SelectBox::select(std::string_view value)
{
    SelectBoxState* s = getState();
    if (!s) return;
    for (std::size_t i = 0; i < s->options.size(); ++i) {
        if (s->options[i].value == value) {
            selectAt(i);
            return;
        }
    }
    // Value not found — no-op.
}

/**
 * @brief Select at
 *
 * @param index  Zero-based index into the collection
 */
void SelectBox::selectAt(std::size_t index)
{
    SelectBoxState* s = getState();
    if (!s || index >= s->options.size()) return;
    if (s->selected_idx == index) return;  // already selected

    s->selected_idx = index;

    const std::string_view sv = s->selectedValue();
    if (s->cfg.value)     s->cfg.value->set(std::string{ sv });
    if (s->cfg.on_change) s->cfg.on_change(sv);

    tree.markDirty(handle);
}

/**
 * @brief Opens
 */
void SelectBox::open()
{
    SelectBoxState* s = getState();
    if (!s || s->is_open || s->options.empty()) return;
    s->is_open     = true;
    s->hovered_idx = SIZE_MAX;
    tree.markDirty(handle);
}

/**
 * @brief Closes
 */
void SelectBox::close()
{
    SelectBoxState* s = getState();
    if (!s || !s->is_open) return;
    s->is_open     = false;
    s->hovered_idx = SIZE_MAX;
    tree.markDirty(handle);
}

/**
 * @brief Toggle
 */
void SelectBox::toggle()
{
    SelectBoxState* s = getState();
    if (s && s->is_open) close();
    else                 open();
}

/**
 * @brief Checks whether open
 *
 * @return true on success, false on failure
 */
bool SelectBox::isOpen() const noexcept
{
    const SelectBoxState* s = getState();
    return s && s->is_open;
}

/**
 * @brief Handles event
 *
 * @param ev  SDL3 input or window event
 *
 * @return true on success, false on failure
 */
bool SelectBox::handleEvent(const SDL_Event& ev)
{
    RenderNode*     n = tree.node(handle);
    SelectBoxState* s = getState();
    if (!n || !s) return false;

    switch (ev.type) {

     // Mouse button
     case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        if (ev.button.button != SDL_BUTTON_LEFT) return false;

        // Coordinates local to this node.
        const float lx = ev.button.x - n->x;
        const float ly = ev.button.y - n->y;

        // Click on the control itself (closed or open header).
        const bool on_control = (lx >= 0.f && lx <= s->cfg.w &&
                                  ly >= 0.f && ly <= s->cfg.h);

        if (s->is_open) {
            // Click inside the dropdown — select the item.
            const std::size_t idx = hitTestDropdown(*s, lx, ly);
            if (idx != SIZE_MAX) {
                selectAt(idx);
                close();
                return true;
            }
            // Click on header while open — close.
            if (on_control) {
                close();
                return true;
            }
            // Click outside — close without selecting.
            close();
            return false;
        }

        // Dropdown is closed — open on click anywhere on the control.
        if (on_control) {
            open();
            return true;
        }
        return false;
    }

    // Mouse motion — track hover row in open dropdown
    case SDL_EVENT_MOUSE_MOTION: {
        if (!s->is_open) return false;

        const float lx = ev.motion.x - n->x;
        const float ly = ev.motion.y - n->y;
        const std::size_t idx = hitTestDropdown(*s, lx, ly);

        if (idx != s->hovered_idx) {
            s->hovered_idx = idx;
            tree.markDirty(handle);
        }
        // Return false so motion events propagate to the rest of the UI.
        return false;
    }

    //  Keyboard
    case SDL_EVENT_KEY_DOWN: {
        if (!s->is_open) return false;

        const SDL_Keycode key = ev.key.key;

        if (key == SDLK_ESCAPE) {
            close();
            return true;
        }

        if (key == SDLK_RETURN ||
            key == SDLK_RETURN2 ||
            key == SDLK_KP_ENTER) {
            if (s->hovered_idx != SIZE_MAX)
                selectAt(s->hovered_idx);
            close();
            return true;
        }

        if (key == SDLK_UP) {
            const std::size_t cur = (s->hovered_idx != SIZE_MAX)
                                    ? s->hovered_idx
                                    : s->selected_idx;
            if (cur > 0) {
                s->hovered_idx = cur - 1;
                tree.markDirty(handle);
            }
            return true;
        }

        if (key == SDLK_DOWN) {
            const std::size_t cur = (s->hovered_idx != SIZE_MAX)
                                    ? s->hovered_idx
                                    : s->selected_idx;
            if (cur + 1 < s->options.size()) {
                s->hovered_idx = cur + 1;
                tree.markDirty(handle);
            }
            return true;
        }

        return false;
    }

    default: return false;
    }
}

} // namespace pce::sdlos::widgets
