#include "input_text_box.hh"
#include "../text_renderer.hh"

#include <SDL3/SDL.h>

#include <algorithm>
#include <any>
#include <cassert>
#include <cmath>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace pce::sdlos::widgets {

namespace {

[[nodiscard]] std::size_t utf8Prev(std::string_view s, std::size_t pos) noexcept
{
    if (pos == 0) return 0;
    --pos;
    while (pos > 0 && (static_cast<unsigned char>(s[pos]) & 0xC0) == 0x80)
        --pos;
    return pos;
}

[[nodiscard]] std::size_t utf8Next(std::string_view s, std::size_t pos) noexcept
{
    if (pos >= s.size()) return s.size();
    ++pos;
    while (pos < s.size() && (static_cast<unsigned char>(s[pos]) & 0xC0) == 0x80)
        ++pos;
    return pos;
}

bool insertText(std::string& text, std::size_t& cursor_pos,
                std::string_view chars, std::size_t max_length) noexcept
{
    if (chars.empty()) return false;
    std::string next = text;
    next.insert(cursor_pos, chars);
    if (max_length > 0 && next.size() > max_length) return false;
    text        = std::move(next);
    cursor_pos += chars.size();
    return true;
}

bool eraseLeft(std::string& text, std::size_t& cursor_pos) noexcept
{
    if (cursor_pos == 0 || text.empty()) return false;
    const std::size_t prev = utf8Prev(text, cursor_pos);
    text.erase(prev, cursor_pos - prev);
    cursor_pos = prev;
    return true;
}

bool eraseRight(std::string& text, std::size_t cursor_pos) noexcept
{
    if (cursor_pos >= text.size()) return false;
    const std::size_t next = utf8Next(text, cursor_pos);
    text.erase(cursor_pos, next - cursor_pos);
    return true;
}

void drawBorderRect(RenderContext& ctx,
                    float x, float y, float w, float h,
                    const Color& c) noexcept
{
    ctx.drawRect(x,           y,           w,        1.f,     c.r, c.g, c.b, c.a);
    ctx.drawRect(x,           y + h - 1.f, w,        1.f,     c.r, c.g, c.b, c.a);
    ctx.drawRect(x,           y + 1.f,     1.f,      h - 2.f, c.r, c.g, c.b, c.a);
    ctx.drawRect(x + w - 1.f, y + 1.f,    1.f,      h - 2.f, c.r, c.g, c.b, c.a);
}

[[nodiscard]] constexpr float charWidth(float font_size) noexcept
{
    return font_size * 0.55f;
}

[[nodiscard]] bool blinkOn() noexcept
{
    return (SDL_GetTicks() / 500u) % 2u == 0u;
}

// SDL_GetKeyboardFocus() can return null on Wayland before the compositor
// grants focus; guard both helpers so callers never pass null to SDL.
static void safeStartTextInput() noexcept
{
    if (SDL_Window* w = SDL_GetKeyboardFocus()) SDL_StartTextInput(w);
}
static void safeStopTextInput() noexcept
{
    if (SDL_Window* w = SDL_GetKeyboardFocus()) SDL_StopTextInput(w);
}

// SDL3 sets FLIPPED when the OS scroll direction is reversed (macOS Natural
// Scrolling); negate so callers always get conventional up=negative sense.
[[nodiscard]] static float normWheel(float raw, SDL_MouseWheelDirection dir) noexcept
{
    return (dir == SDL_MOUSEWHEEL_FLIPPED) ? -raw : raw;
}

std::vector<std::string_view> splitLines(std::string_view text) noexcept
{
    std::vector<std::string_view> lines;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == '\n') {
            lines.push_back(text.substr(start, i - start));
            start = i + 1;
        }
    }
    return lines;
}

struct LinePos { int line; std::size_t col; };

[[nodiscard]] LinePos byteToLinePos(std::string_view text, std::size_t byte_off) noexcept
{
    int line = 0; std::size_t col = 0;
    for (std::size_t i = 0; i < byte_off && i < text.size(); ++i) {
        if (text[i] == '\n') { ++line; col = 0; }
        else                   ++col;
    }
    return { line, col };
}

[[nodiscard]] std::size_t linePosToByteOff(std::string_view text,
                                            int target_line,
                                            std::size_t target_col) noexcept
{
    int line = 0; std::size_t i = 0;
    while (i < text.size() && line < target_line) {
        if (text[i] == '\n') ++line;
        ++i;
    }
    std::size_t col = 0;
    while (i < text.size() && text[i] != '\n' && col < target_col)
        { ++i; ++col; }
    return i;
}

struct ScrollbarMetrics {
    float track_x, track_y, track_h;
    float thumb_y, thumb_h;
};

[[nodiscard]] static ScrollbarMetrics scrollbarMetrics(const TextFieldState& s) noexcept
{
    const float sb_w     = s.cfg.scrollbar_w;
    const float track_x  = s.cfg.w - sb_w - 2.f;
    const float track_y  = 2.f;
    const float track_h  = s.cfg.h - 4.f;
    const float total    = std::max(s.total_h, s.cfg.h);
    const float ratio    = s.cfg.h / total;
    const float th       = std::max(20.f, track_h * ratio);
    const float range    = total - s.cfg.h;
    const float ty       = range > 0.f
        ? track_y + (s.scroll_offset_y / range) * (track_h - th)
        : track_y;
    return { track_x, track_y, track_h, ty, th };
}

struct RowRange { int first; int last; };

[[nodiscard]] static RowRange visibleRows(const TextFieldState& s, int total_lines) noexcept
{
    const float lh    = s.cfg.line_height;
    const float vis_h = s.cfg.h - s.cfg.padding.top - s.cfg.padding.bottom;
    const int   first = static_cast<int>(s.scroll_offset_y / lh);
    const int   last  = static_cast<int>((s.scroll_offset_y + vis_h) / lh) + 1;
    return { std::max(first, 0), std::min(last, total_lines) };
}

// Walk the parent chain to accumulate the absolute screen position.
// Layout stores parent-relative x/y; this mirrors style_draw::absolutePos.
[[nodiscard]] static std::pair<float,float> absPos(const RenderTree& tree,
                                                    NodeHandle h) noexcept
{
    float ax = 0.f, ay = 0.f;
    for (NodeHandle cur = h; cur.valid(); ) {
        const RenderNode* n = tree.node(cur);
        if (!n) break;
        ax += n->x; ay += n->y;
        cur = n->parent;
    }
    return { ax, ay };
}

} // anonymous namespace


TextField makeTextField(RenderTree& tree, TextFieldConfig cfg)
{
    if (cfg.multiline) {
        if (cfg.rows > 0)
            cfg.h = static_cast<float>(cfg.rows) * cfg.line_height
                    + cfg.padding.top + cfg.padding.bottom;
        if (cfg.cols > 0)
            cfg.w = static_cast<float>(cfg.cols) * charWidth(cfg.font_size)
                    + cfg.padding.left + cfg.padding.right + cfg.scrollbar_w + 4.f;
    }

    const NodeHandle h = tree.alloc();
    RenderNode*      n = tree.node(h);
    assert(n && "makeTextField: alloc returned invalid handle");
    n->w            = cfg.w;
    n->h            = cfg.h;
    n->dirty_render = true;

    auto st   = std::make_shared<TextFieldState>();
    st->cfg   = std::move(cfg);
    if (st->cfg.multiline) {
        st->content_w = st->cfg.w - st->cfg.scrollbar_w - 4.f
                      - st->cfg.padding.left - st->cfg.padding.right;
        st->content_h = st->cfg.h - st->cfg.padding.top - st->cfg.padding.bottom;
    }
    n->state = st;

    n->draw = [st, h, &tree](RenderContext& ctx) {
        st->text_renderer = ctx.text_renderer;
        const TextFieldState&  s = *st;
        const TextFieldConfig& c = s.cfg;

        const auto [ax, ay] = absPos(tree, h);
        const float w = c.w, wh = c.h;

        const Color& bg = s.focused ? c.bg_focused : c.bg;
        ctx.drawRect(ax, ay, w, wh, bg.r, bg.g, bg.b, bg.a);

        const Color& bord = s.focused ? c.border_focus : c.border;
        drawBorderRect(ctx, ax + 1.f, ay + 1.f, w - 2.f, wh - 2.f, bord);

        if (c.multiline) {
            const auto lines = splitLines(s.text);
            const int  nl    = static_cast<int>(lines.size());

            if (nl == 1 && lines[0].empty() && !c.placeholder.empty()) {
                ctx.drawText(c.placeholder,
                             ax + c.padding.left, ay + c.padding.top,
                             c.font_size,
                             c.placeholder_color.r, c.placeholder_color.g,
                             c.placeholder_color.b, c.placeholder_color.a);
            } else {
                const auto [first, last] = visibleRows(s, nl);
                for (int i = first; i < last; ++i) {
                    const float ry = ay + c.padding.top
                                   + static_cast<float>(i) * c.line_height
                                   - s.scroll_offset_y;
                    if (ry + c.line_height < ay) continue;
                    if (ry > ay + wh)             break;
                    if (!lines[static_cast<std::size_t>(i)].empty())
                        ctx.drawText(lines[static_cast<std::size_t>(i)],
                                     ax + c.padding.left - s.scroll_offset_x,
                                     ry, c.font_size,
                                     c.text_color.r, c.text_color.g,
                                     c.text_color.b, c.text_color.a);
                }
            }

            if (s.focused && blinkOn()) {
                const auto [line, col] = byteToLinePos(s.text, s.cursor_pos);
                // substr(0, col) gives the prefix whose rendered width = cursor x
                const float cx = ax + c.padding.left - s.scroll_offset_x
                    + (ctx.text_renderer && col > 0
                        ? static_cast<float>(ctx.text_renderer->measureText(
                            lines[static_cast<std::size_t>(line)].substr(0, col),
                            c.font_size).first)
                        : static_cast<float>(col) * charWidth(c.font_size));
                const float cy = ay + c.padding.top
                               + static_cast<float>(line) * c.line_height
                               - s.scroll_offset_y;
                if (cy >= ay && cy + c.font_size <= ay + wh)
                    ctx.drawRect(cx, cy, 2.f, c.font_size * 1.1f,
                                 c.cursor_color.r, c.cursor_color.g,
                                 c.cursor_color.b, c.cursor_color.a);
            }

            if (s.total_h > wh) {
                const auto sb = scrollbarMetrics(s);
                ctx.drawRect(ax + sb.track_x, ay + sb.track_y,
                             c.scrollbar_w, sb.track_h,  0.3f, 0.3f, 0.3f, 0.25f);
                const float ta = s.scrollbar_drag ? 0.7f : 0.45f;
                ctx.drawRect(ax + sb.track_x, ay + sb.thumb_y,
                             c.scrollbar_w, sb.thumb_h,  0.8f, 0.8f, 0.8f, ta);
            }
        } else {
            const float tx = ax + c.padding.left;
            const float ty = ay + (wh - c.font_size) * 0.5f;

            if (s.text.empty() && !c.placeholder.empty()) {
                ctx.drawText(c.placeholder, tx, ty, c.font_size,
                             c.placeholder_color.r, c.placeholder_color.g,
                             c.placeholder_color.b, c.placeholder_color.a);
            } else if (!s.text.empty()) {
                const std::string display = c.secure
                    ? std::string(s.text.size(), '*') : s.text;
                ctx.drawText(display, tx, ty, c.font_size,
                             c.text_color.r, c.text_color.g,
                             c.text_color.b, c.text_color.a);
            }

            if (s.focused && blinkOn()) {
                const std::string prefix = c.secure
                    ? std::string(s.cursor_pos, '*')
                    : s.text.substr(0, s.cursor_pos);
                const float cx = tx + (ctx.text_renderer && !prefix.empty()
                    ? static_cast<float>(
                        ctx.text_renderer->measureText(prefix, c.font_size).first)
                    : static_cast<float>(s.cursor_pos) * charWidth(c.font_size));
                const float cy = ay + (wh - c.font_size * 1.25f) * 0.5f;
                ctx.drawRect(cx, cy, 2.f, c.font_size * 1.25f,
                             c.cursor_color.r, c.cursor_color.g,
                             c.cursor_color.b, c.cursor_color.a);
            }
        }
    };

    RenderNode* np = n;
    n->update = [st, np]() {
        TextFieldState& s = *st;
        if (s.cfg.multiline) {
            const auto lines = splitLines(s.text);
            s.total_h = static_cast<float>(std::max(1, static_cast<int>(lines.size())))
                      * s.cfg.line_height
                      + s.cfg.padding.top + s.cfg.padding.bottom;

            s.total_w = 0.f;
            for (const auto& line : lines) {
                const float lw = s.text_renderer
                    ? static_cast<float>(
                        s.text_renderer->measureText(line, s.cfg.font_size).first)
                    : static_cast<float>(line.size()) * charWidth(s.cfg.font_size);
                if (lw > s.total_w) s.total_w = lw;
            }
            s.total_w += s.cfg.padding.left + s.cfg.padding.right;

            s.scroll_offset_y = std::clamp(s.scroll_offset_y, 0.f,
                                            std::max(0.f, s.total_h - s.cfg.h));
            s.scroll_offset_x = std::clamp(s.scroll_offset_x, 0.f,
                                            std::max(0.f, s.total_w - s.content_w));
        }
        if (s.focused) np->dirty_render = true;
    };

    return TextField{ tree, h };
}


std::string_view TextField::getText() const noexcept
{
    const TextFieldState* s = getState();
    return s ? std::string_view{ s->text } : std::string_view{};
}

void TextField::setText(std::string text)
{
    TextFieldState* s = getState();
    if (!s || s->cfg.disabled) return;
    s->text       = std::move(text);
    s->cursor_pos = s->text.size();
    if (s->cfg.value)     s->cfg.value->set(s->text);
    if (s->cfg.on_change) s->cfg.on_change(s->text);
    tree.markDirty(handle);
}

void TextField::clear() { setText({}); }

int TextField::lineCount() const noexcept
{
    const TextFieldState* s = getState();
    if (!s || !s->cfg.multiline) return 1;
    return static_cast<int>(splitLines(s->text).size());
}

void TextField::scrollToCursor()
{
    TextFieldState* s = getState();
    if (!s || !s->cfg.multiline) { tree.markDirty(handle); return; }

    const auto  [line, col] = byteToLinePos(s->text, s->cursor_pos);
    const float cy          = static_cast<float>(line) * s->cfg.line_height;

    if (cy < s->scroll_offset_y)
        s->scroll_offset_y = cy;

    const float bottom = s->scroll_offset_y + s->content_h - s->cfg.line_height;
    if (cy > bottom)
        s->scroll_offset_y = cy - s->content_h + s->cfg.line_height;

    tree.markDirty(handle);
}

void TextField::focus()
{
    TextFieldState* s = getState();
    if (!s || s->focused || s->cfg.disabled) return;
    s->focused    = true;
    s->cursor_pos = s->text.size();
    safeStartTextInput();
    tree.markDirty(handle);
}

void TextField::unfocus()
{
    TextFieldState* s = getState();
    if (!s || !s->focused) return;
    s->focused = false;
    safeStopTextInput();
    tree.markDirty(handle);
}

bool TextField::isFocused() const noexcept
{
    const TextFieldState* s = getState();
    return s && s->focused;
}

bool TextField::handleEvent(const SDL_Event& ev)
{
    RenderNode*     n = tree.node(handle);
    TextFieldState* s = getState();
    if (!n || !s || s->cfg.disabled) return false;

    switch (ev.type) {

    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        const float mx = ev.button.x;
        const float my = ev.button.y;
        const auto [ax, ay] = absPos(tree, handle);
        const bool hit = (mx >= ax && mx < ax + n->w &&
                          my >= ay && my < ay + n->h);

        if (!hit) {
            if (s->focused) unfocus();
            return false;
        }

        if (s->cfg.multiline && s->total_h > s->cfg.h) {
            const auto  sb = scrollbarMetrics(*s);
            const float lx = mx - ax;
            const float ly = my - ay;
            if (lx >= sb.track_x && lx <= sb.track_x + s->cfg.scrollbar_w &&
                ly >= sb.thumb_y  && ly <= sb.thumb_y  + sb.thumb_h) {
                s->scrollbar_drag = true;
                s->drag_start_y   = my;
                s->drag_start_off = s->scroll_offset_y;
                tree.markDirty(handle);
                return true;
            }
        }

        if (!s->focused) focus();

        if (s->cfg.multiline) {
            const float lx = mx - ax - s->cfg.padding.left + s->scroll_offset_x;
            const float ly = my - ay - s->cfg.padding.top  + s->scroll_offset_y;
            const int clicked_line = std::max(0,
                static_cast<int>(ly / s->cfg.line_height));

            if (s->text_renderer && lx > 0.f) {
                const auto lines = splitLines(s->text);
                const int  li    = std::min(clicked_line,
                    std::max(0, static_cast<int>(lines.size()) - 1));
                const auto& line_sv    = lines[static_cast<std::size_t>(li)];
                const std::size_t lstart = linePosToByteOff(s->text, clicked_line, 0);

                std::size_t best = 0, idx = 0;
                float prev_w = 0.f;
                while (idx < line_sv.size()) {
                    const std::size_t nxt   = utf8Next(line_sv, idx);
                    const float       cur_w = static_cast<float>(
                        s->text_renderer->measureText(
                            line_sv.substr(0, nxt), s->cfg.font_size).first);
                    if (cur_w > lx) {
                        best = (lx - prev_w < cur_w - lx) ? idx : nxt;
                        break;
                    }
                    prev_w = cur_w; best = nxt; idx = nxt;
                }
                s->cursor_pos = lstart + best;
            } else {
                s->cursor_pos = linePosToByteOff(s->text, clicked_line,
                    static_cast<std::size_t>(std::max(0,
                        static_cast<int>(lx / charWidth(s->cfg.font_size)))));
            }
        }
        tree.markDirty(handle);
        return true;
    }

    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (s->scrollbar_drag) {
            s->scrollbar_drag = false;
            tree.markDirty(handle);
            return true;
        }
        return false;

    case SDL_EVENT_MOUSE_MOTION: {
        if (!s->scrollbar_drag) return false;
        const auto  sb    = scrollbarMetrics(*s);
        const float dy    = ev.motion.y - s->drag_start_y;
        const float range = sb.track_h - sb.thumb_h;
        if (range > 0.f) {
            const float sr = std::max(0.f, s->total_h - s->cfg.h);
            s->scroll_offset_y = std::clamp(
                s->drag_start_off + dy * (sr / range), 0.f, sr);
        }
        tree.markDirty(handle);
        return true;
    }

    case SDL_EVENT_MOUSE_WHEEL: {
        if (!s->focused || !s->cfg.multiline) return false;
        const float wy = normWheel(ev.wheel.y, ev.wheel.direction);
        const float wx = normWheel(ev.wheel.x, ev.wheel.direction);
        if (SDL_GetModState() & SDL_KMOD_SHIFT) {
            const float delta = (wx != 0.f) ? wx : wy;
            s->scroll_offset_x = std::clamp(
                s->scroll_offset_x - delta * s->cfg.line_height * 3.f,
                0.f, std::max(0.f, s->total_w - s->content_w));
        } else {
            s->scroll_offset_y = std::clamp(
                s->scroll_offset_y - wy * s->cfg.line_height * 3.f,
                0.f, std::max(0.f, s->total_h - s->cfg.h));
            if (wx != 0.f)
                s->scroll_offset_x = std::clamp(
                    s->scroll_offset_x - wx * s->cfg.line_height * 3.f,
                    0.f, std::max(0.f, s->total_w - s->content_w));
        }
        tree.markDirty(handle);
        return true;
    }

    case SDL_EVENT_TEXT_EDITING:
        return s->focused; // consume IME composition; don't render pre-edit yet

    case SDL_EVENT_TEXT_INPUT: {
        if (!s->focused) return false;
        safeStartTextInput(); // lazy re-arm if focus() ran before window had focus
        if (insertText(s->text, s->cursor_pos, ev.text.text, s->cfg.max_length)) {
            if (s->cfg.value)     s->cfg.value->set(s->text);
            if (s->cfg.on_change) s->cfg.on_change(s->text);
            if (s->cfg.multiline) scrollToCursor();
            else                  tree.markDirty(handle);
        }
        return true;
    }

    case SDL_EVENT_KEY_DOWN: {
        if (!s->focused) return false;
        const SDL_Keycode key = ev.key.key;
        const SDL_Keymod  mod = ev.key.mod;
        const bool        gui = (mod & SDL_KMOD_GUI) != 0; // Cmd on macOS
        bool changed = false;

        if (key == SDLK_BACKSPACE) {
            changed = eraseLeft(s->text, s->cursor_pos);
        } else if (key == SDLK_DELETE) {
            changed = eraseRight(s->text, s->cursor_pos);

        } else if (key == SDLK_LEFT) {
            if (gui) {
                if (s->cfg.multiline) {
                    auto [line, col]      = byteToLinePos(s->text, s->cursor_pos);
                    const std::size_t bol = linePosToByteOff(s->text, line, 0);
                    if (s->cursor_pos != bol) { s->cursor_pos = bol; scrollToCursor(); }
                } else {
                    if (s->cursor_pos) { s->cursor_pos = 0; tree.markDirty(handle); }
                }
            } else {
                const auto prev = utf8Prev(s->text, s->cursor_pos);
                if (prev != s->cursor_pos) {
                    s->cursor_pos = prev;
                    if (s->cfg.multiline) scrollToCursor(); else tree.markDirty(handle);
                }
            }

        } else if (key == SDLK_RIGHT) {
            if (gui) {
                if (s->cfg.multiline) {
                    const auto lines     = splitLines(s->text);
                    auto [line, col]     = byteToLinePos(s->text, s->cursor_pos);
                    const std::size_t li = static_cast<std::size_t>(line);
                    const std::size_t eol = linePosToByteOff(s->text, line,
                        li < lines.size() ? lines[li].size() : 0);
                    if (s->cursor_pos != eol) { s->cursor_pos = eol; scrollToCursor(); }
                } else {
                    if (s->cursor_pos != s->text.size()) {
                        s->cursor_pos = s->text.size(); tree.markDirty(handle);
                    }
                }
            } else {
                const auto next = utf8Next(s->text, s->cursor_pos);
                if (next != s->cursor_pos) {
                    s->cursor_pos = next;
                    if (s->cfg.multiline) scrollToCursor(); else tree.markDirty(handle);
                }
            }

        } else if (key == SDLK_UP && s->cfg.multiline) {
            if (gui) {
                if (s->cursor_pos) { s->cursor_pos = 0; scrollToCursor(); }
            } else {
                auto [line, col] = byteToLinePos(s->text, s->cursor_pos);
                if (line > 0) {
                    s->cursor_pos = linePosToByteOff(s->text, line - 1, col);
                    scrollToCursor();
                }
            }

        } else if (key == SDLK_DOWN && s->cfg.multiline) {
            if (gui) {
                if (s->cursor_pos != s->text.size()) {
                    s->cursor_pos = s->text.size(); scrollToCursor();
                }
            } else {
                auto [line, col] = byteToLinePos(s->text, s->cursor_pos);
                s->cursor_pos    = linePosToByteOff(s->text, line + 1, col);
                scrollToCursor();
            }

        } else if (key == SDLK_HOME || (key == SDLK_A && (mod & SDL_KMOD_CTRL))) {
            if (s->cursor_pos) {
                s->cursor_pos = 0;
                if (s->cfg.multiline) scrollToCursor(); else tree.markDirty(handle);
            }
        } else if (key == SDLK_END  || (key == SDLK_E && (mod & SDL_KMOD_CTRL))) {
            if (s->cursor_pos != s->text.size()) {
                s->cursor_pos = s->text.size();
                if (s->cfg.multiline) scrollToCursor(); else tree.markDirty(handle);
            }

        } else if (key == SDLK_RETURN || key == SDLK_RETURN2 || key == SDLK_KP_ENTER) {
            if (s->cfg.multiline) {
                changed = insertText(s->text, s->cursor_pos, "\n", s->cfg.max_length);
                if (changed) scrollToCursor();
            } else {
                if (s->cfg.on_submit) s->cfg.on_submit(s->text);
                unfocus();
            }
        } else if (key == SDLK_ESCAPE) {
            unfocus();
        }

        if (changed) {
            if (s->cfg.value)     s->cfg.value->set(s->text);
            if (s->cfg.on_change) s->cfg.on_change(s->text);
            if (s->cfg.multiline) scrollToCursor(); else tree.markDirty(handle);
        }
        return true;
    }

    default: return false;
    }
}

} // namespace pce::sdlos::widgets
