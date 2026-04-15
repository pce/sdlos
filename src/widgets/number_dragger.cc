#include "number_dragger.h"

#include "../core/parse.h"
#include "../text_renderer.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <any>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <optional>

namespace pce::sdlos {

struct AbsPos { float x, y; };

inline AbsPos absolutePos(const RenderTree &tree, NodeHandle h) {
    float ax = 0.f, ay = 0.f;
    for (; h.valid();) {
        const RenderNode *n = tree.node(h);
        if (!n)
            break;
        ax += n->x;
        ay += n->y;
        h = n->parent;
    }
    return {ax, ay};
}

} // namespace pce::sdlos

namespace pce::sdlos::widgets {
namespace {

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

static float snapClamp(float v, float lo, float hi, float step) noexcept {
    const float c = std::clamp(v, lo, hi);
    if (step <= 0.f)
        return c;
    return std::clamp(lo + std::round((c - lo) / step) * step, lo, hi);
}

static std::string fmtVal(float v, float step) noexcept {
    int dp = 0;
    if (step > 0.f && step < 1.f) {
        float s = step;
        while (s < 1.f - 1e-5f && dp < 6) {
            s *= 10.f;
            ++dp;
        }
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.*f", dp, static_cast<double>(v));
    return buf;
}

static std::optional<float> parseFloat(std::string_view sv) noexcept {
    if (sv.empty())
        return {};
    float v{};
    auto [p, ok] = ::pce::sdlos::parse_float(sv.data(), sv.data() + sv.size(), v);
    return ok ? std::optional<float>{v} : std::nullopt;
}

static bool applyValue(RenderTree &tree, NodeHandle h, NumberDraggerState &s, float raw) noexcept {
    const float next = snapClamp(raw, s.cfg.min, s.cfg.max, s.cfg.step);
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

static bool blinkOn() noexcept {
    return (SDL_GetTicks() / 530u) % 2u == 0u;
}
static void startText() noexcept {
    if (SDL_Window *w = SDL_GetKeyboardFocus())
        SDL_StartTextInput(w);
}
static void stopText() noexcept {
    if (SDL_Window *w = SDL_GetKeyboardFocus())
        SDL_StopTextInput(w);
}

static void
installCallbacks(RenderTree &tree, NodeHandle h, std::shared_ptr<NumberDraggerState> st) {
    RenderNode *n = tree.node(h);
    if (!n)
        return;

    n->draw = [st, h, &tree](RenderContext &ctx) {
        NumberDraggerState &s        = *st;
        const NumberDraggerConfig &c = s.cfg;
        const RenderNode *self       = tree.node(h);
        if (!self || self->w <= 0.f || self->h <= 0.f)
            return;

        s.text_renderer     = ctx.text_renderer;
        const auto [ax, ay] = absPos(tree, h);
        const float w       = self->w;
        const float wh      = self->h;
        const float cy      = ay + wh * 0.5f;

        const bool dragging = s.mode == DraggerMode::Dragging || s.mode == DraggerMode::PendingDrag;
        const bool editing  = s.mode == DraggerMode::Editing;
        const bool arrows   = (s.hovered || dragging) && !editing;

        // background
        const Color &bg = editing   ? c.bg_edit
                        : dragging  ? c.bg_drag
                        : s.hovered ? c.bg_hover
                                    : c.bg;
        if (bg.a > 0.001f)
            ctx.drawRect(ax, ay, w, wh, bg.r, bg.g, bg.b, bg.a);

        // drag-hint arrows: dim "< ... >"
        if (arrows) {
            const float asz = c.font_size * 0.85f;
            const float aty = cy - asz * 0.5f;
            ctx.drawText(
                "<",
                ax + 3.f,
                aty,
                asz,
                c.arrow_color.r,
                c.arrow_color.g,
                c.arrow_color.b,
                c.arrow_color.a);
            ctx.drawText(
                ">",
                ax + w - asz - 3.f,
                aty,
                asz,
                c.arrow_color.r,
                c.arrow_color.g,
                c.arrow_color.b,
                c.arrow_color.a);
        }

        // value / edit text
        const std::string disp = editing ? s.edit_text : fmtVal(s.value, c.step);
        const float tw =
            ctx.text_renderer
                ? static_cast<float>(ctx.text_renderer->measureText(disp, c.font_size).first)
                : static_cast<float>(disp.size()) * c.font_size * 0.55f;
        const float tx = ax + (w - tw) * 0.5f;
        const float ty = cy - c.font_size * 0.5f;
        ctx.drawText(
            disp,
            tx,
            ty,
            c.font_size,
            c.text_color.r,
            c.text_color.g,
            c.text_color.b,
            c.text_color.a);

        // cursor blink in edit mode
        if (editing && blinkOn()) {
            const std::string_view pre = std::string_view{s.edit_text}.substr(0, s.cursor_pos);
            const float pw =
                ctx.text_renderer
                    ? static_cast<float>(ctx.text_renderer->measureText(pre, c.font_size).first)
                    : static_cast<float>(s.cursor_pos) * c.font_size * 0.55f;
            ctx.drawRect(
                tx + pw,
                ty,
                1.5f,
                c.font_size * 1.1f,
                c.cursor_color.r,
                c.cursor_color.g,
                c.cursor_color.b,
                c.cursor_color.a);
        }

        // edit-mode border
        if (editing)
            ctx.drawRectOutline(
                ax,
                ay,
                w,
                wh,
                1.f,
                c.border_edit.r,
                c.border_edit.g,
                c.border_edit.b,
                0.8f);

        // hover underline
        if (s.hovered && !editing)
            ctx.drawRect(
                ax + 2.f,
                ay + wh - 1.f,
                w - 4.f,
                1.f,
                c.arrow_color.r,
                c.arrow_color.g,
                c.arrow_color.b,
                0.7f);
    };

    n->update = [st, h, &tree]() {
        const bool needs = st->mode != DraggerMode::Idle || st->hovered;
        if (needs)
            if (RenderNode *self = tree.node(h))
                self->dirty_render = true;
    };

    n->dirty_render = true;
}

}  // namespace

/**
 * @brief Constructs and returns number dragger
 *
 * @param tree  Red channel component [0, 1]
 * @param cfg   Configuration options struct
 *
 * @return NumberDragger result
 */
NumberDragger makeNumberDragger(RenderTree &tree, NumberDraggerConfig cfg) {
    cfg.initial_value  = snapClamp(cfg.initial_value, cfg.min, cfg.max, cfg.step);
    const NodeHandle h = tree.alloc();
    RenderNode *n      = tree.node(h);
    assert(n);
    n->w           = cfg.w;
    n->h           = cfg.h;
    n->layout_kind = LayoutKind::None;

    auto st   = std::make_shared<NumberDraggerState>();
    st->value = cfg.initial_value;
    st->cfg   = std::move(cfg);
    n->state  = st;

    if (st->cfg.value_signal)
        st->cfg.value_signal->set(st->value);
    installCallbacks(tree, h, st);
    return NumberDragger{tree, h};
}

/**
 * @brief Returns value
 *
 * @return Computed floating-point value
 */
float NumberDragger::getValue() const noexcept {
    const auto *s = getState();
    return s ? s->value : 0.f;
}

/**
 * @brief Sets value
 *
 * @param v  32-bit floating-point scalar
 */
void NumberDragger::setValue(float v) {
    auto *s = getState();
    if (!s)
        return;
    applyValue(tree, handle, *s, v);
    if (s->mode == DraggerMode::Editing) {
        s->edit_text  = fmtVal(s->value, s->cfg.step);
        s->cursor_pos = s->edit_text.size();
    }
}

/**
 * @brief Checks whether dragging
 *
 * @return true on success, false on failure
 */
bool NumberDragger::isDragging() const noexcept {
    const auto *s = getState();
    return s && (s->mode == DraggerMode::Dragging || s->mode == DraggerMode::PendingDrag);
}
/**
 * @brief Checks whether editing
 *
 * @return true on success, false on failure
 */
bool NumberDragger::isEditing() const noexcept {
    const auto *s = getState();
    return s && s->mode == DraggerMode::Editing;
}

/**
 * @brief Handles event
 *
 * @param ev  SDL3 input or window event
 *
 * @return true on success, false on failure
 */
bool NumberDragger::handleEvent(const SDL_Event &ev) {
    RenderNode *n         = tree.node(handle);
    NumberDraggerState *s = getState();
    if (!n || !s)
        return false;

    const auto [ax, ay] = absPos(tree, handle);

    auto commitEdit = [&] {
        if (s->mode != DraggerMode::Editing)
            return;
        if (auto p = parseFloat(s->edit_text))
            applyValue(tree, handle, *s, *p);
        if (s->cfg.on_commit)
            s->cfg.on_commit(s->value);
        s->mode       = DraggerMode::Idle;
        s->edit_text  = {};
        s->cursor_pos = 0;
        stopText();
        n->dirty_render = true;
    };
    auto cancelEdit = [&] {
        if (s->mode != DraggerMode::Editing)
            return;
        applyValue(tree, handle, *s, s->pre_edit_val);
        s->mode       = DraggerMode::Idle;
        s->edit_text  = {};
        s->cursor_pos = 0;
        stopText();
        n->dirty_render = true;
    };

    switch (ev.type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        if (ev.button.button != SDL_BUTTON_LEFT)
            return false;
        const float mx = ev.button.x, my = ev.button.y;
        const bool hit = mx >= ax && mx < ax + n->w && my >= ay && my < ay + n->h;
        if (!hit) {
            commitEdit();
            return false;
        }
        if (s->mode == DraggerMode::Editing)
            return true;
        s->mode           = DraggerMode::PendingDrag;
        s->drag_start_x   = mx;
        s->drag_start_val = s->value;
        n->dirty_render   = true;
        return true;
    }

    case SDL_EVENT_MOUSE_MOTION: {
        const float mx = ev.motion.x, my = ev.motion.y;
        const bool in = mx >= ax && mx < ax + n->w && my >= ay && my < ay + n->h;
        if (in != s->hovered) {
            s->hovered      = in;
            n->dirty_render = true;
        }

        if (s->mode == DraggerMode::PendingDrag) {
            if (std::abs(mx - s->drag_start_x) > 3.f)
                s->mode = DraggerMode::Dragging;
        }
        if (s->mode == DraggerMode::Dragging) {
            applyValue(
                tree,
                handle,
                *s,
                s->drag_start_val + (mx - s->drag_start_x) * s->effectiveDragSpeed());
            return true;
        }
        return false;
    }

    case SDL_EVENT_MOUSE_BUTTON_UP: {
        if (ev.button.button != SDL_BUTTON_LEFT)
            return false;
        if (s->mode == DraggerMode::PendingDrag) {
            // click (no drag) → enter edit mode
            s->pre_edit_val = s->value;
            s->edit_text    = fmtVal(s->value, s->cfg.step);
            s->cursor_pos   = s->edit_text.size();
            s->mode         = DraggerMode::Editing;
            startText();
            n->dirty_render = true;
            return true;
        }
        if (s->mode == DraggerMode::Dragging) {
            s->mode         = DraggerMode::Idle;
            n->dirty_render = true;
            return true;
        }
        return false;
    }

    case SDL_EVENT_TEXT_INPUT: {
        if (s->mode != DraggerMode::Editing)
            return false;
        for (char raw : std::string_view{ev.text.text}) {
            const auto c = static_cast<unsigned char>(raw);
            if (std::isdigit(c) || c == '.' || (raw == '-' && s->cursor_pos == 0)) {
                s->edit_text.insert(s->cursor_pos, 1, raw);
                ++s->cursor_pos;
            }
        }
        n->dirty_render = true;
        return true;
    }

    case SDL_EVENT_KEY_DOWN: {
        if (s->mode == DraggerMode::Editing) {
            switch (ev.key.key) {
            case SDLK_RETURN:
            case SDLK_KP_ENTER:
                commitEdit();
                return true;
            case SDLK_ESCAPE:
                cancelEdit();
                return true;
            case SDLK_BACKSPACE:
                if (s->cursor_pos > 0) {
                    s->edit_text.erase(--s->cursor_pos, 1);
                    n->dirty_render = true;
                }
                return true;
            case SDLK_LEFT:
                if (s->cursor_pos > 0) {
                    --s->cursor_pos;
                    n->dirty_render = true;
                }
                return true;
            case SDLK_RIGHT:
                if (s->cursor_pos < s->edit_text.size()) {
                    ++s->cursor_pos;
                    n->dirty_render = true;
                }
                return true;
            default:
                break;
            }
            return false;
        }
        // Idle: arrow keys step (not consumed so parent can also respond)
        const float eff = s->cfg.step > 0.f ? s->cfg.step : (s->cfg.max - s->cfg.min) * 0.01f;
        const float inc = (ev.key.mod & SDL_KMOD_SHIFT) ? eff * 10.f : eff;
        if (ev.key.key == SDLK_LEFT || ev.key.key == SDLK_DOWN) {
            applyValue(tree, handle, *s, s->value - inc);
            return false;
        }
        if (ev.key.key == SDLK_RIGHT || ev.key.key == SDLK_UP) {
            applyValue(tree, handle, *s, s->value + inc);
            return false;
        }
        return false;
    }

    default:
        break;
    }
    return false;
}

namespace {
static void walkBindDragNum(RenderTree &tree, NodeHandle root) {
    if (!root.valid())
        return;
    RenderNode *n = tree.node(root);
    if (!n)
        return;
    for (NodeHandle c = n->child; c.valid();) {
        const RenderNode *cn = tree.node(c);
        if (!cn)
            break;
        NodeHandle next = cn->sibling;
        walkBindDragNum(tree, c);
        c = next;
    }
    if (n->style("tag") != "input" || n->style("type") != "dragnum")
        return;

    auto pf = [](std::string_view sv) -> std::optional<float> {
        if (sv.empty())
            return {};
        float v{};
        auto [p, ok] = ::pce::sdlos::parse_float(sv.data(), sv.data() + sv.size(), v);
        return ok ? std::optional<float>{v} : std::nullopt;
    };

    NumberDraggerConfig cfg;
    cfg.min        = pf(n->style("min")).value_or(cfg.min);
    cfg.max        = pf(n->style("max")).value_or(cfg.max);
    cfg.initial_value      = pf(n->style("value")).value_or(cfg.initial_value);
    cfg.step       = pf(n->style("step")).value_or(cfg.step);
    cfg.drag_speed = pf(n->style("dragSpeed")).value_or(0.f);
    if (auto v = pf(n->style("width"))) {
        cfg.w = *v;
        n->w  = *v;
    }
    if (auto v = pf(n->style("height"))) {
        cfg.h = *v;
        n->h  = *v;
    }
    if (auto v = pf(n->style("fontSize")))
        cfg.font_size = *v;

    cfg.initial_value = snapClamp(cfg.initial_value, cfg.min, cfg.max, cfg.step);

    auto st   = std::make_shared<NumberDraggerState>();
    st->value = cfg.initial_value;
    st->cfg   = std::move(cfg);
    n->state  = st;
    installCallbacks(tree, root, std::move(st));
}
}  // namespace

/**
 * @brief Binds drag num widgets
 *
 * @param tree  Red channel component [0, 1]
 * @param root  Red channel component [0, 1]
 */
void bindDragNumWidgets(RenderTree &tree, NodeHandle root) {
    walkBindDragNum(tree, root);
}

}  // namespace pce::sdlos::widgets
