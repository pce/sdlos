// RangeSlider widget implementation
//
// sdlos — SDL3 GPU render core (C++23).

#include "range_slider.h"

#include "../core/parse.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <any>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace pce::sdlos::widgets {

// Anonymous-namespace helpers
namespace {

// Walk the parent chain to accumulate the absolute screen position of node h.
// Mirrors the identical helper in input_text_box.cc and style_draw.cc.
[[nodiscard]]
static std::pair<float, float> absPos(const RenderTree &tree, NodeHandle h) noexcept {
    float ax = 0.f, ay = 0.f;
    for (NodeHandle cur = h; cur.valid();) {
        const RenderNode *n = tree.node(cur);
        if (!n)
            break;
        ax  += n->x;
        ay  += n->y;
        cur  = n->parent;
    }
    return {ax, ay};
}

// Snap `v` to the nearest multiple of `step` above `min_v`, then clamp.
// When step <= 0 the value is returned clamped only (fully continuous).
[[nodiscard]]
static float snapToStep(float v, float min_v, float max_v, float step) noexcept {
    const float clamped = std::clamp(v, min_v, max_v);
    if (step <= 0.f)
        return clamped;
    const float snapped = min_v + std::round((clamped - min_v) / step) * step;
    return std::clamp(snapped, min_v, max_v);
}

// How many decimal places are needed to display a step value accurately.
// e.g. step=0.01 → 2,  step=0.5 → 1,  step=1 → 0,  step=0 → 2 (fallback).
[[nodiscard]]
static int decimalPlaces(float step) noexcept {
    if (step <= 0.f)
        return 2;  // continuous — show 2 dp by default
    if (step >= 1.f)
        return 0;  // integer steps
    float s = step;
    int dp  = 0;
    while (s < 1.f - 1e-5f && dp < 6) {
        s *= 10.f;
        ++dp;
    }
    return dp;
}

// Format a float value for the optional value label.
[[nodiscard]]
static std::string fmtValue(float v, float step) noexcept {
    char buf[32];
    const int dp = decimalPlaces(step);
    std::snprintf(buf, sizeof(buf), "%.*f", dp, static_cast<double>(v));
    return buf;
}

// Parse a float from a string_view.  Returns nullopt on failure or empty input.
[[nodiscard]]
static std::optional<float> parseFloat(std::string_view sv) noexcept {
    if (sv.empty())
        return std::nullopt;
    float v{};
    const auto [ptr, ok] = pce::sdlos::parse_float(sv.data(), sv.data() + sv.size(), v);
    return ok ? std::optional<float>{v} : std::nullopt;
}

// Apply (clamp + snap) a new raw value, notify bindings, mark dirty.
// Returns true when the value actually changed.
static bool applyValue(RenderTree &tree, NodeHandle h, RangeSliderState &s, float raw) noexcept {
    const float next = snapToStep(raw, s.cfg.min, s.cfg.max, s.cfg.step);
    if (next == s.value)
        return false;

    s.value = next;

    if (s.cfg.value_signal)
        s.cfg.value_signal->set(next);

    if (s.cfg.on_change)
        s.cfg.on_change(next);

    if (RenderNode *n = tree.node(h))
        n->dirty_render = true;

    return true;
}

// Geometry helpers
// Absolute x coordinate of the thumb centre.
[[nodiscard]]
static float thumbCX(const RangeSliderState &s, float ax) noexcept {
    return ax + s.fraction() * s.trackW();
}

// Map a screen x to a slider raw value (before snap).
[[nodiscard]]
static float xToValue(const RangeSliderState &s, float ax, float screen_x) noexcept {
    const float tw   = s.trackW();
    const float frac = std::clamp((screen_x - ax) / tw, 0.f, 1.f);
    return s.cfg.min + frac * (s.cfg.max - s.cfg.min);
}

// Hit-test: does (mx, my) fall on the track / thumb hit area?
[[nodiscard]]
static bool hitTrack(const RangeSliderState &s, float ax, float ay, float mx, float my) noexcept {
    const float tw = s.trackW();
    const float cy = ay + s.cfg.h * 0.5f;
    const float tr = s.cfg.thumb_r;
    return mx >= ax && mx <= ax + tw && my >= cy - tr && my <= cy + tr;
}

// Hit-test: does (mx, my) fall on the thumb square specifically?
[[nodiscard]]
static bool hitThumb(const RangeSliderState &s, float ax, float ay, float mx, float my) noexcept {
    const float tx = thumbCX(s, ax);
    const float cy = ay + s.cfg.h * 0.5f;
    const float tr = s.cfg.thumb_r;
    return mx >= tx - tr && mx <= tx + tr && my >= cy - tr && my <= cy + tr;
}

// ── Shared draw + update installer ───────────────────────────────────────────
//
// Called by both makeRangeSlider() and the bindInputWidgets() walker so the
// visual/event logic is never duplicated.

static void
installSliderCallbacks(RenderTree &tree, NodeHandle h, std::shared_ptr<RangeSliderState> st) {
    RenderNode *n = tree.node(h);
    if (!n)
        return;

    // TODO draw refactor extract
    n->draw = [st, h, &tree](RenderContext &ctx) {
        const RangeSliderState &s  = *st;
        const RangeSliderConfig &c = s.cfg;

        const RenderNode *self = tree.node(h);
        if (!self || self->w <= 0.f || self->h <= 0.f)
            return;

        const auto [ax, ay] = absPos(tree, h);
        const float tw      = s.trackW();       // effective track pixel width
        const float cy      = ay + c.h * 0.5f;  // vertical centre of the widget

        // ── Track background ─────────────────────────────────────────────────
        const float track_y = cy - c.track_h * 0.5f;
        ctx.drawRect(
            ax,
            track_y,
            tw,
            c.track_h,
            c.track_bg.r,
            c.track_bg.g,
            c.track_bg.b,
            c.track_bg.a);

        // ── Filled portion (left of thumb) ───────────────────────────────────
        const float fill_w = s.fraction() * tw;
        if (fill_w > 0.5f) {
            ctx.drawRect(
                ax,
                track_y,
                fill_w,
                c.track_h,
                c.track_fill.r,
                c.track_fill.g,
                c.track_fill.b,
                c.track_fill.a);
        }

        //  Track end caps (thin vertical tick at min and max)
        const float cap_h = c.track_h + 4.f;
        const float cap_y = cy - cap_h * 0.5f;
        ctx.drawRect(
            ax,
            cap_y,
            1.f,
            cap_h,
            c.track_bg.r,
            c.track_bg.g,
            c.track_bg.b,
            c.track_bg.a * 0.6f);
        ctx.drawRect(
            ax + tw - 1.f,
            cap_y,
            1.f,
            cap_h,
            c.track_bg.r,
            c.track_bg.g,
            c.track_bg.b,
            c.track_bg.a * 0.6f);

        // Thumb
        const float tx = ax + fill_w;  // thumb centre x
        const float tr = c.thumb_r;

        // Pick colour based on interaction state.
        const Color &tc = s.dragging      ? c.thumb_drag
                        : s.thumb_hovered ? c.thumb_hover
                                          : c.thumb_color;

        // Drop shadow (1 px larger on every side, semi-transparent black).
        ctx.drawRect(
            tx - tr - 1.f,
            cy - tr - 1.f,
            tr * 2.f + 2.f,
            tr * 2.f + 2.f,
            0.f,
            0.f,
            0.f,
            0.30f);

        // Thumb body.
        ctx.drawRect(tx - tr, cy - tr, tr * 2.f, tr * 2.f, tc.r, tc.g, tc.b, tc.a);

        // Highlight: top-left gleam when not dragging (gives a subtle 3-D lift).
        if (!s.dragging) {
            ctx.drawRect(
                tx - tr + 1.f,
                cy - tr + 1.f,
                tr * 2.f - 2.f,
                tr - 1.f,
                1.f,
                1.f,
                1.f,
                0.15f);
        }

        // Centre pip: small 2×2 square so the thumb has a visible anchor point.
        ctx.drawRect(tx - 1.f, cy - 1.f, 2.f, 2.f, 0.f, 0.f, 0.f, 0.25f);

        // Value label
        if (c.show_value) {
            const std::string val_str = fmtValue(s.value, c.step);
            const float lx            = ax + tw + 8.f;
            const float ly            = cy - c.font_size * 0.5f;
            ctx.drawText(
                val_str,
                lx,
                ly,
                c.font_size,
                c.value_color.r,
                c.value_color.g,
                c.value_color.b,
                c.value_color.a);
        }

        // Focus ring: Drawn as a 1-px outline one pixel outside the widget bounds.
        if (s.focused) {
            const Color &f = c.track_fill;
            ctx.drawRectOutline(
                ax - 2.f,
                ay - 2.f,
                c.w + 4.f,
                c.h + 4.f,
                1.f,
                f.r,
                f.g,
                f.b,
                0.55f);
        }
    };

    // NOTE: Keep re-queuing dirty_render while dragging or focused so the cursor
    // blink / drag position is reflected on every frame without external nudge.
    n->update = [st, h, &tree]() {
        if (st->dragging || st->focused) {
            if (RenderNode *self = tree.node(h))
                self->dirty_render = true;
        }
    };

    n->dirty_render = true;
}

}  // anonymous namespace

/// Factory
RangeSlider makeRangeSlider(RenderTree &tree, RangeSliderConfig cfg) {
    // Clamp + snap the initial value before storing it.
    cfg.value = snapToStep(std::clamp(cfg.value, cfg.min, cfg.max), cfg.min, cfg.max, cfg.step);

    const NodeHandle h = tree.alloc();
    RenderNode *n      = tree.node(h);
    assert(n && "makeRangeSlider: alloc returned an invalid handle");

    n->w           = cfg.w;
    n->h           = cfg.h;
    n->layout_kind = LayoutKind::None;

    auto st   = std::make_shared<RangeSliderState>();
    st->value = cfg.value;
    st->cfg   = std::move(cfg);
    n->state  = st;

    // Push initial value into the Signal binding if one was provided.
    if (st->cfg.value_signal)
        st->cfg.value_signal->set(st->value);

    installSliderCallbacks(tree, h, st);

    return RangeSlider{tree, h};
}

/**
 * @brief Returns value
 *
 * @return Computed floating-point value
 */
float RangeSlider::getValue() const noexcept {
    const RangeSliderState *s = getState();
    return s ? s->value : 0.f;
}

/**
 * @brief Sets value
 *
 * @param v  32-bit floating-point scalar
 */
void RangeSlider::setValue(float v) {
    RangeSliderState *s = getState();
    if (!s)
        return;
    applyValue(tree, handle, *s, v);
}

/**
 * @brief Steps down
 *
 * @param multiplier  Red channel component [0, 1]
 */
void RangeSlider::stepDown(int multiplier) {
    RangeSliderState *s = getState();
    if (!s)
        return;
    const float effective = s->cfg.step > 0.f ? s->cfg.step : (s->cfg.max - s->cfg.min) * 0.01f;
    applyValue(tree, handle, *s, s->value - effective * static_cast<float>(multiplier));
}

/**
 * @brief Steps up
 *
 * @param multiplier  Red channel component [0, 1]
 */
void RangeSlider::stepUp(int multiplier) {
    RangeSliderState *s = getState();
    if (!s)
        return;
    const float effective = s->cfg.step > 0.f ? s->cfg.step : (s->cfg.max - s->cfg.min) * 0.01f;
    applyValue(tree, handle, *s, s->value + effective * static_cast<float>(multiplier));
}

/**
 * @brief Focus
 */
void RangeSlider::focus() {
    RangeSliderState *s = getState();
    if (!s || s->focused)
        return;
    s->focused = true;
    if (RenderNode *n = tree.node(handle))
        n->dirty_render = true;
}

/**
 * @brief Unfocus
 */
void RangeSlider::unfocus() {
    RangeSliderState *s = getState();
    if (!s || !s->focused)
        return;
    s->focused = false;
    if (RenderNode *n = tree.node(handle))
        n->dirty_render = true;
}

/**
 * @brief Checks whether focused
 *
 * @return true on success, false on failure
 */
bool RangeSlider::isFocused() const noexcept {
    const RangeSliderState *s = getState();
    return s && s->focused;
}

/**
 * @brief Checks whether dragging
 *
 * @return true on success, false on failure
 */
bool RangeSlider::isDragging() const noexcept {
    const RangeSliderState *s = getState();
    return s && s->dragging;
}

/**
 * @brief Handles event
 *
 * @param ev  SDL3 input or window event
 *
 * @return true on success, false on failure
 */
bool RangeSlider::handleEvent(const SDL_Event &ev) {
    RenderNode *n       = tree.node(handle);
    RangeSliderState *s = getState();
    if (!n || !s)
        return false;

    const auto [ax, ay] = absPos(tree, handle);

    switch (ev.type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        if (ev.button.button != SDL_BUTTON_LEFT)
            return false;

        const float mx = ev.button.x;
        const float my = ev.button.y;

        if (!hitTrack(*s, ax, ay, mx, my)) {
            // Clicked outside → drop focus but do NOT consume the event so
            // other widgets (e.g. a text field below) can claim it.
            if (s->focused) {
                s->focused      = false;
                n->dirty_render = true;
            }
            return false;
        }

        s->focused       = true;
        s->dragging      = true;
        s->thumb_hovered = hitThumb(*s, ax, ay, mx, my);
        applyValue(tree, handle, *s, xToValue(*s, ax, mx));
        return true;
    }

    // Mouse motion: hover highlight + drag seek
    case SDL_EVENT_MOUSE_MOTION: {
        const float mx = ev.motion.x;
        const float my = ev.motion.y;

        // Update thumb hover state (bounds-checked; cosmetic only).
        const bool hovered = hitThumb(*s, ax, ay, mx, my);
        if (hovered != s->thumb_hovered) {
            s->thumb_hovered = hovered;
            n->dirty_render  = true;
        }

        // Drag seek: no bounds check — drag capture keeps the value live
        // even when the pointer strays outside the widget.
        if (s->dragging) {
            applyValue(tree, handle, *s, xToValue(*s, ax, mx));
            return true;  // consume while dragging
        }
        return false;
    }

    // Mouse button up: end drag (no bounds check — drag capture)
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        if (ev.button.button != SDL_BUTTON_LEFT || !s->dragging)
            return false;
        s->dragging     = false;
        n->dirty_render = true;
        return true;
    }

    // Keyboard navigation (requires focus)
    case SDL_EVENT_KEY_DOWN: {
        if (!s->focused)
            return false;

        // Effective step: use cfg.step when set, else 1 % of the range.
        const float eff_step = s->cfg.step > 0.f ? s->cfg.step : (s->cfg.max - s->cfg.min) * 0.01f;

        // Shift key multiplies the step by 10 (matches browser behaviour).
        const bool shift = (ev.key.mod & SDL_KMOD_SHIFT) != 0;
        const float inc  = shift ? eff_step * 10.f : eff_step;

        switch (ev.key.key) {
        case SDLK_LEFT:
        case SDLK_DOWN:
            applyValue(tree, handle, *s, s->value - inc);
            return true;

        case SDLK_RIGHT:
        case SDLK_UP:
            applyValue(tree, handle, *s, s->value + inc);
            return true;

        case SDLK_HOME:
            applyValue(tree, handle, *s, s->cfg.min);
            return true;

        case SDLK_END:
            applyValue(tree, handle, *s, s->cfg.max);
            return true;

        case SDLK_PAGEDOWN:
            // Page step: 10 % of range regardless of step setting.
            applyValue(tree, handle, *s, s->value - (s->cfg.max - s->cfg.min) * 0.1f);
            return true;

        case SDLK_PAGEUP:
            applyValue(tree, handle, *s, s->value + (s->cfg.max - s->cfg.min) * 0.1f);
            return true;

        default:
            break;
        }
        return false;
    }

    default:
        break;
    }

    return false;
}

namespace {

/// Recursive walker used by bindInputWidgets.
///  - bindInputWidgets — auto-spawn sliders from jade  input[type=range]  nodes
static void walkBindInputs(RenderTree &tree, NodeHandle root) {
    if (!root.valid())
        return;

    RenderNode *n = tree.node(root);
    if (!n)
        return;

    // Recurse into children first so we process the whole subtree.
    for (NodeHandle c = n->child; c.valid();) {
        const RenderNode *cn = tree.node(c);
        if (!cn)
            break;
        const NodeHandle next = cn->sibling;
        walkBindInputs(tree, c);
        c = next;
    }

    // Is this node an  <input type="range">?
    const std::string_view tag  = n->style("tag");
    const std::string_view type = n->style("type");
    if (tag != "input" || type != "range")
        return;

    // Build config from jade style attributes.
    RangeSliderConfig cfg;

    if (const auto v = parseFloat(n->style("min")))
        cfg.min = *v;
    if (const auto v = parseFloat(n->style("max")))
        cfg.max = *v;
    if (const auto v = parseFloat(n->style("value")))
        cfg.value = *v;
    if (const auto v = parseFloat(n->style("step")))
        cfg.step = *v;

    // Geometry — also write back to the node so layout engine picks it up.
    if (const auto v = parseFloat(n->style("width"))) {
        cfg.w = *v;
        n->w  = *v;
    }
    if (const auto v = parseFloat(n->style("height"))) {
        cfg.h = *v;
        n->h  = *v;
    }
    if (const auto v = parseFloat(n->style("fontSize")))
        cfg.font_size = *v;
    if (const auto v = parseFloat(n->style("labelWidth")))
        cfg.label_w = *v;

    {
        const std::string_view sv = n->style("showValue");
        if (sv == "true" || sv == "1")
            cfg.show_value = true;
    }

    // Clamp + snap the initial value.
    cfg.value = snapToStep(std::clamp(cfg.value, cfg.min, cfg.max), cfg.min, cfg.max, cfg.step);

    // Allocate and install state.
    auto st   = std::make_shared<RangeSliderState>();
    st->value = cfg.value;
    st->cfg   = std::move(cfg);
    n->state  = st;  // replaces any previous std::any (e.g. from bindDrawCallbacks)

    // Override whatever draw/update callbacks bindDrawCallbacks may have set.
    installSliderCallbacks(tree, root, std::move(st));
}

}  // anonymous namespace

/**
 * @brief Binds input widgets
 *
 * @param tree  Red channel component [0, 1]
 * @param root  Red channel component [0, 1]
 */
void bindInputWidgets(RenderTree &tree, NodeHandle root) {
    walkBindInputs(tree, root);
}

}  // namespace pce::sdlos::widgets
