#include "input_text_box.hh"
#include "../text_renderer.hh"

#include <SDL3/SDL.h>
#include <simdutf.h>
#include <utf8.h>

#include <algorithm>
#include <any>
#include <cassert>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pce::sdlos::widgets {

namespace {

// ============================================================================
// UTF-8 boundary navigation — utf8cpp unchecked (inputs are simdutf-validated)
// ============================================================================

[[nodiscard]] static std::size_t
utf8Next(std::string_view s, std::size_t pos) noexcept
{
    if (pos >= s.size()) return s.size();
    const char* it  = s.data() + pos;
    const char* end = s.data() + s.size();
    // utf8::next throws on invalid input; catch and byte-step as fallback.
    try {
        utf8::next(it, end);
    } catch (...) {
        return (pos + 1 <= s.size()) ? pos + 1 : s.size();
    }
    const auto result = static_cast<std::size_t>(it - s.data());
    return (result <= s.size()) ? result : s.size();
}

[[nodiscard]] static std::size_t
utf8Prev(std::string_view s, std::size_t pos) noexcept
{
    if (pos == 0) return 0;
    const char* it    = s.data() + pos;
    const char* begin = s.data();
    try {
        utf8::prior(it, begin);
    } catch (...) {
        return pos > 0 ? pos - 1 : 0;
    }
    return static_cast<std::size_t>(it - s.data());
}

// ============================================================================
// UTF-8 validation via simdutf
// ============================================================================

[[nodiscard]] static bool isValidUtf8(const char* data, std::size_t len) noexcept
{
    return simdutf::validate_utf8(data, len);
}

// ============================================================================
// String mutation primitives
// ============================================================================

/// Insert UTF-8 `chars` at `cursor_pos`. Returns false if max_length exceeded.
static bool insertText(std::string& text,
                       std::size_t& cursor_pos,
                       std::string_view chars,
                       std::size_t max_length) noexcept
{
    if (chars.empty()) return false;
    if (max_length > 0 && text.size() + chars.size() > max_length) return false;
    text.insert(cursor_pos, chars);
    cursor_pos += chars.size();
    return true;
}

/// Erase the codepoint immediately before `cursor_pos`.
static bool eraseLeft(std::string& text, std::size_t& cursor_pos) noexcept
{
    if (cursor_pos == 0 || text.empty()) return false;
    const std::size_t prev = utf8Prev(text, cursor_pos);
    text.erase(prev, cursor_pos - prev);
    cursor_pos = prev;
    return true;
}

/// Erase the codepoint at `cursor_pos` (cursor does not move).
static bool eraseRight(std::string& text, std::size_t cursor_pos) noexcept
{
    if (cursor_pos >= text.size()) return false;
    const std::size_t next = utf8Next(text, cursor_pos);
    text.erase(cursor_pos, next - cursor_pos);
    return true;
}

// ============================================================================
// Selection helpers
// ============================================================================

[[nodiscard]] static bool hasSelection(const TextFieldState& s) noexcept
{
    return s.sel_active && s.sel_start != s.sel_end;
}

/// Always returns [lo, hi] regardless of anchor direction.
[[nodiscard]] static std::pair<std::size_t, std::size_t>
normalizedSel(const TextFieldState& s) noexcept
{
    if (!s.sel_active || s.sel_start == s.sel_end)
        return { s.cursor_pos, s.cursor_pos };
    return { std::min(s.sel_start, s.sel_end),
             std::max(s.sel_start, s.sel_end) };
}

static void clearSel(TextFieldState& s) noexcept
{
    s.sel_active = false;
    s.sel_start  = s.cursor_pos;
    s.sel_end    = s.cursor_pos;
}

// ============================================================================
// Undo helpers
// ============================================================================

static void pushUndo(TextFieldState& s)
{
    if (!s.undo_stack.empty() && s.undo_stack.back() == s.text) return;
    s.undo_stack.push_back(s.text);                     // copy
    if (s.undo_stack.size() > TextFieldState::kUndoLimit)
        s.undo_stack.erase(s.undo_stack.begin());
}

[[nodiscard]] static bool performUndo(TextFieldState& s) noexcept
{
    if (s.undo_stack.empty()) return false;
    s.text       = std::move(s.undo_stack.back());
    s.undo_stack.pop_back();
    s.cursor_pos = s.text.size();
    clearSel(s);
    return true;
}

/// Replace selected range with `replacement` (or insert at cursor if none).
/// Caller must push undo before calling if a mutation history is wanted.
static void replaceSelection(TextFieldState& s, std::string_view replacement)
{
    if (hasSelection(s)) {
        auto [a, b] = normalizedSel(s);
        const std::size_t after_size = s.text.size() - (b - a) + replacement.size();
        if (s.cfg.max_length > 0 && after_size > s.cfg.max_length) return;
        s.text.erase(a, b - a);
        s.text.insert(a, replacement);
        s.cursor_pos = a + replacement.size();
        clearSel(s);
    } else {
        insertText(s.text, s.cursor_pos, replacement, s.cfg.max_length);
    }
}

// ============================================================================
// Drawing utilities
// ============================================================================

static void drawBorderRect(RenderContext& ctx,
                            float x, float y, float w, float h,
                            const Color& c) noexcept
{
    ctx.drawRect(x,           y,           w,        1.f,     c.r, c.g, c.b, c.a);
    ctx.drawRect(x,           y + h - 1.f, w,        1.f,     c.r, c.g, c.b, c.a);
    ctx.drawRect(x,           y + 1.f,     1.f,      h - 2.f, c.r, c.g, c.b, c.a);
    ctx.drawRect(x + w - 1.f, y + 1.f,    1.f,      h - 2.f, c.r, c.g, c.b, c.a);
}

[[nodiscard]] static constexpr float charWidth(float font_size) noexcept
{
    return font_size * 0.55f;
}

[[nodiscard]] static bool blinkOn() noexcept
{
    return (SDL_GetTicks() / 500u) % 2u == 0u;
}

// ============================================================================
// SDL text-input / IME helpers
// ============================================================================

/// Start SDL text input on the currently focused window (safe vs. null).
/// Must be called whenever the field becomes interactive — even if it was
/// already "focused" in our state, because the very first call during
/// jade_app_init may have run before the OS granted keyboard focus.
static void safeStartTextInput() noexcept
{
    if (SDL_Window* w = SDL_GetKeyboardFocus()) SDL_StartTextInput(w);
}

static void safeStopTextInput() noexcept
{
    if (SDL_Window* w = SDL_GetKeyboardFocus()) SDL_StopTextInput(w);
}

/// Hint the OS IME where its candidate pop-up should appear (SDL3).
static void hintImeRect(float screen_x, float screen_y, float font_h) noexcept
{
    SDL_Window* w = SDL_GetKeyboardFocus();
    if (!w) return;
    const SDL_Rect r{
        static_cast<int>(screen_x), static_cast<int>(screen_y),
        1, static_cast<int>(font_h)
    };
    SDL_SetTextInputArea(w, &r, 0);
}

[[nodiscard]] static float normWheel(float raw,
                                      SDL_MouseWheelDirection dir) noexcept
{
    return (dir == SDL_MOUSEWHEEL_FLIPPED) ? -raw : raw;
}

// ============================================================================
// Text layout utilities
// ============================================================================

static std::vector<std::string_view> splitLines(std::string_view text) noexcept
{
    std::vector<std::string_view> lines;
    lines.reserve(32);
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

[[nodiscard]] static LinePos
byteToLinePos(std::string_view text, std::size_t byte_off) noexcept
{
    int line = 0; std::size_t col = 0;
    for (std::size_t i = 0; i < byte_off && i < text.size(); ++i) {
        if (text[i] == '\n') { ++line; col = 0; }
        else                   ++col;
    }
    return { line, col };
}

[[nodiscard]] static std::size_t
linePosToByteOff(std::string_view text,
                  int target_line, std::size_t target_col) noexcept
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

struct ScrollbarMetrics { float track_x, track_y, track_h, thumb_y, thumb_h; };

[[nodiscard]] static ScrollbarMetrics
scrollbarMetrics(const TextFieldState& s) noexcept
{
    const float sb_w    = s.cfg.scrollbar_w;
    const float track_x = s.cfg.w - sb_w - 2.f;
    const float track_y = 2.f;
    const float track_h = s.cfg.h - 4.f;
    const float total   = std::max(s.total_h, s.cfg.h);
    const float ratio   = s.cfg.h / total;
    const float th      = std::max(20.f, track_h * ratio);
    const float range   = total - s.cfg.h;
    const float ty      = range > 0.f
        ? track_y + (s.scroll_offset_y / range) * (track_h - th)
        : track_y;
    return { track_x, track_y, track_h, ty, th };
}

struct RowRange { int first, last; };

[[nodiscard]] static RowRange
visibleRows(const TextFieldState& s, int total_lines) noexcept
{
    const float lh    = s.cfg.line_height;
    const float vis_h = s.cfg.h - s.cfg.padding.top - s.cfg.padding.bottom;
    const int   first = static_cast<int>(s.scroll_offset_y / lh);
    const int   last  = static_cast<int>((s.scroll_offset_y + vis_h) / lh) + 1;
    return { std::max(first, 0), std::min(last, total_lines) };
}

/// Walk the parent chain to accumulate absolute screen position.
[[nodiscard]] static std::pair<float, float>
absPos(const RenderTree& tree, NodeHandle h) noexcept
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

// ============================================================================
// Pixel ↔ byte-column helpers
// ============================================================================

/// Pixel width of the first `bytes` bytes of `line_text`.
[[nodiscard]] static float
lineColPx(const TextFieldState& s, std::string_view line_text,
          std::size_t bytes, float font_size) noexcept
{
    if (bytes == 0) return 0.f;
    const std::size_t safe = std::min(bytes, line_text.size());
    if (s.text_renderer && safe > 0)
        return static_cast<float>(
            s.text_renderer->measureText(line_text.substr(0, safe), font_size).first);
    return static_cast<float>(safe) * charWidth(font_size);
}

/// Find the nearest byte offset within `line` for a click at pixel `px`.
[[nodiscard]] static std::size_t
lineHitTest(const TextFieldState& s, std::string_view line,
            float px, float font_size) noexcept
{
    if (px <= 0.f || line.empty()) return 0;
    std::size_t best = 0, idx = 0;
    float prev_w = 0.f;
    while (idx < line.size()) {
        const std::size_t nxt   = utf8Next(line, idx);
        const float       cur_w = lineColPx(s, line, nxt, font_size);
        if (cur_w > px) {
            best = (px - prev_w < cur_w - px) ? idx : nxt;
            break;
        }
        prev_w = cur_w;
        best   = nxt;
        idx    = nxt;
    }
    return best;
}

/// Word boundaries (whitespace-delimited) around `pos`.
[[nodiscard]] static std::pair<std::size_t, std::size_t>
wordBoundaries(std::string_view text, std::size_t pos) noexcept
{
    std::size_t start = pos;
    while (start > 0) {
        const std::size_t p = utf8Prev(text, start);
        if (std::isspace(static_cast<unsigned char>(text[p]))) break;
        start = p;
    }
    std::size_t end = pos;
    while (end < text.size()) {
        if (std::isspace(static_cast<unsigned char>(text[end]))) break;
        end = utf8Next(text, end);
    }
    return { start, end };
}

} // anonymous namespace

// ============================================================================
// makeTextField
// ============================================================================

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

    // Selection background: translucent system-blue
    static constexpr Color kSelBg  = Color::hex(0x0a, 0x84, 0xff, 0x55);
    // IME composition underline: pale blue
    static constexpr Color kCompUl = Color::hex(0x88, 0xcc, 0xff, 0xff);

    // ── Draw callback ────────────────────────────────────────────────────────
    n->draw = [st, h, &tree](RenderContext& ctx) {
        st->text_renderer = ctx.text_renderer;
        const TextFieldState&  s = *st;
        const TextFieldConfig& c = s.cfg;

        const auto [ax, ay] = absPos(tree, h);
        const float w = c.w, wh = c.h;

        // Background
        const Color& bg = s.focused ? c.bg_focused : c.bg;
        ctx.drawRect(ax, ay, w, wh, bg.r, bg.g, bg.b, bg.a);

        // Border
        const Color& bord = s.focused ? c.border_focus : c.border;
        drawBorderRect(ctx, ax + 1.f, ay + 1.f, w - 2.f, wh - 2.f, bord);

        // Clip all content (text, cursor, selection, scrollbar) to the
        // widget's inner bounding box so long lines and partial rows never
        // bleed outside the textarea frame.
        const SDL_Rect clip{
            static_cast<int>(ax + 1.f),
            static_cast<int>(ay + 1.f),
            std::max(0, static_cast<int>(w  - 2.f)),
            std::max(0, static_cast<int>(wh - 2.f))
        };
        SDL_SetGPUScissor(ctx.pass, &clip);

        if (c.multiline) {
            // ── Multiline ────────────────────────────────────────────────
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

                // Cursor's line and column (in bytes from that line's start)
                const auto [cur_line, cur_col] =
                    byteToLinePos(s.text, s.cursor_pos);

                // Normalised selection range
                const auto [selA, selB] = normalizedSel(s);
                const bool any_sel      = hasSelection(s);

                for (int i = first; i < last; ++i) {
                    const std::size_t iu  = static_cast<std::size_t>(i);
                    const std::string_view line = lines[iu];
                    const float ry = ay + c.padding.top
                                   + static_cast<float>(i) * c.line_height
                                   - s.scroll_offset_y;
                    if (ry + c.line_height < ay) continue;
                    if (ry > ay + wh)             break;

                    // Byte range of this line within s.text
                    const std::size_t lstart =
                        static_cast<std::size_t>(line.data() - s.text.data());
                    const std::size_t lend = lstart + line.size();

                    // ── Selection background ──────────────────────────────
                    if (any_sel && selA < lend && selB > lstart) {
                        const std::size_t lA = std::max(selA, lstart) - lstart;
                        const std::size_t lB = std::min(selB, lend)   - lstart;
                        const float x0 = lineColPx(s, line, lA, c.font_size);
                        const float x1 = lineColPx(s, line, lB, c.font_size);
                        ctx.drawRect(ax + c.padding.left + x0 - s.scroll_offset_x,
                                     ry, x1 - x0, c.line_height,
                                     kSelBg.r, kSelBg.g, kSelBg.b, kSelBg.a);
                    }

                    // ── Text (with inline composition on the cursor line) ─
                    const float text_x = ax + c.padding.left - s.scroll_offset_x;
                    if (i == cur_line && !s.composition.empty()) {
                        // Split: prefix | composition | suffix
                        const std::string_view prefix =
                            line.substr(0, std::min(cur_col, line.size()));
                        const std::string_view suffix =
                            line.substr(std::min(cur_col, line.size()));

                        if (!prefix.empty())
                            ctx.drawText(prefix, text_x, ry, c.font_size,
                                         c.text_color.r, c.text_color.g,
                                         c.text_color.b, c.text_color.a);

                        const float pre_w  = lineColPx(s, line, cur_col, c.font_size);
                        const float comp_w = lineColPx(s, s.composition,
                            s.composition.size(), c.font_size);

                        ctx.drawText(s.composition,
                                     text_x + pre_w, ry, c.font_size,
                                     kCompUl.r, kCompUl.g, kCompUl.b, 1.f);
                        // Underline under composition
                        ctx.drawRect(text_x + pre_w, ry + c.font_size + 1.f,
                                     comp_w, 1.f,
                                     kCompUl.r, kCompUl.g, kCompUl.b, kCompUl.a);

                        if (!suffix.empty())
                            ctx.drawText(suffix, text_x + pre_w + comp_w, ry,
                                         c.font_size,
                                         c.text_color.r, c.text_color.g,
                                         c.text_color.b, c.text_color.a);
                    } else if (!line.empty()) {
                        ctx.drawText(line, text_x, ry, c.font_size,
                                     c.text_color.r, c.text_color.g,
                                     c.text_color.b, c.text_color.a);
                    }
                }

                // ── Cursor ────────────────────────────────────────────────
                if (s.focused && blinkOn() &&
                    cur_line >= first && cur_line < last)
                {
                    const std::string_view cl =
                        lines[static_cast<std::size_t>(cur_line)];
                    const float pre_w = lineColPx(s, cl, cur_col, c.font_size);

                    // Shift cursor inside composition by composition_cursor
                    float comp_cw = 0.f;
                    if (!s.composition.empty()) {
                        comp_cw = lineColPx(s, s.composition,
                            static_cast<std::size_t>(s.composition_cursor),
                            c.font_size);
                    }

                    const float cx = ax + c.padding.left - s.scroll_offset_x
                                   + pre_w + comp_cw;
                    const float cy = ay + c.padding.top
                                   + static_cast<float>(cur_line) * c.line_height
                                   - s.scroll_offset_y;

                    if (cy >= ay && cy + c.font_size <= ay + wh) {
                        ctx.drawRect(cx, cy, 2.f, c.font_size * 1.1f,
                                     c.cursor_color.r, c.cursor_color.g,
                                     c.cursor_color.b, c.cursor_color.a);
                        hintImeRect(cx, cy, c.font_size);
                    }
                }
            }

            // Scrollbar
            if (s.total_h > wh) {
                const auto sb = scrollbarMetrics(s);
                ctx.drawRect(ax + sb.track_x, ay + sb.track_y,
                             c.scrollbar_w, sb.track_h, 0.3f, 0.3f, 0.3f, 0.25f);
                const float ta = s.scrollbar_drag ? 0.7f : 0.45f;
                ctx.drawRect(ax + sb.track_x, ay + sb.thumb_y,
                             c.scrollbar_w, sb.thumb_h, 0.8f, 0.8f, 0.8f, ta);
            }

        } else {
            // ── Single-line ──────────────────────────────────────────────
            const float tx = ax + c.padding.left;
            const float ty = ay + (wh - c.font_size) * 0.5f;

            // For secure mode show '*' per byte (ASCII passwords only)
            const std::string display = c.secure
                ? std::string(s.text.size(), '*') : s.text;

            // Selection background
            if (hasSelection(s) && !display.empty()) {
                const auto [selA, selB] = normalizedSel(s);
                const float x0 = c.secure
                    ? static_cast<float>(selA) * charWidth(c.font_size)
                    : lineColPx(s, display, selA, c.font_size);
                const float x1 = c.secure
                    ? static_cast<float>(selB) * charWidth(c.font_size)
                    : lineColPx(s, display, selB, c.font_size);
                ctx.drawRect(tx + x0 - s.scroll_offset_x,
                             ay + 2.f, x1 - x0, wh - 4.f,
                             kSelBg.r, kSelBg.g, kSelBg.b, kSelBg.a);
            }

            // Text
            if (s.text.empty() && !c.placeholder.empty()) {
                ctx.drawText(c.placeholder, tx, ty, c.font_size,
                             c.placeholder_color.r, c.placeholder_color.g,
                             c.placeholder_color.b, c.placeholder_color.a);
            } else if (!display.empty()) {
                if (!s.composition.empty()) {
                    // Inline composition: prefix | comp | suffix
                    const std::size_t cpos = std::min(s.cursor_pos, display.size());
                    const std::string_view prefix = std::string_view(display).substr(0, cpos);
                    const std::string_view suffix  =
                        std::string_view(display).substr(cpos);
                    const float pre_w  = lineColPx(s, prefix, prefix.size(), c.font_size);
                    const float comp_w = lineColPx(s, s.composition,
                        s.composition.size(), c.font_size);

                    if (!prefix.empty())
                        ctx.drawText(prefix,
                                     tx - s.scroll_offset_x, ty, c.font_size,
                                     c.text_color.r, c.text_color.g,
                                     c.text_color.b, c.text_color.a);

                    ctx.drawText(s.composition,
                                 tx + pre_w - s.scroll_offset_x, ty, c.font_size,
                                 kCompUl.r, kCompUl.g, kCompUl.b, 1.f);
                    ctx.drawRect(tx + pre_w - s.scroll_offset_x,
                                 ty + c.font_size + 1.f, comp_w, 1.f,
                                 kCompUl.r, kCompUl.g, kCompUl.b, kCompUl.a);

                    if (!suffix.empty())
                        ctx.drawText(suffix,
                                     tx + pre_w + comp_w - s.scroll_offset_x, ty,
                                     c.font_size,
                                     c.text_color.r, c.text_color.g,
                                     c.text_color.b, c.text_color.a);
                } else {
                    ctx.drawText(display,
                                 tx - s.scroll_offset_x, ty, c.font_size,
                                 c.text_color.r, c.text_color.g,
                                 c.text_color.b, c.text_color.a);
                }
            }

            // Cursor
            if (s.focused && blinkOn()) {
                const float pre_w = c.secure
                    ? static_cast<float>(s.cursor_pos) * charWidth(c.font_size)
                    : lineColPx(s, display, s.cursor_pos, c.font_size);
                const float comp_cw = s.composition.empty() ? 0.f
                    : lineColPx(s, s.composition,
                        static_cast<std::size_t>(s.composition_cursor), c.font_size);

                const float cx = tx + pre_w + comp_cw - s.scroll_offset_x;
                const float cy = ay + (wh - c.font_size * 1.25f) * 0.5f;
                ctx.drawRect(cx, cy, 2.f, c.font_size * 1.25f,
                             c.cursor_color.r, c.cursor_color.g,
                             c.cursor_color.b, c.cursor_color.a);
                hintImeRect(cx, cy, c.font_size);
            }
        }
        // Restore full-viewport scissor so sibling nodes drawn after this
        // widget are not inadvertently clipped.
        const SDL_Rect full_vp{
            0, 0,
            static_cast<int>(ctx.viewport_w),
            static_cast<int>(ctx.viewport_h)
        };
        SDL_SetGPUScissor(ctx.pass, &full_vp);
    }; // end draw

    // ── Update callback ──────────────────────────────────────────────────────
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

// ============================================================================
// TextField public methods
// ============================================================================

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
    clearSel(*s);
    s->composition.clear();
    s->composition_cursor = 0;
    s->undo_stack.clear();  // programmatic reset clears history
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
    if (!s) return;

    if (!s->cfg.multiline) {
        // Horizontal scroll to keep cursor visible in single-line mode
        if (s->text_renderer) {
            const float cursor_px = static_cast<float>(
                s->text_renderer->measureText(
                    s->text.substr(0, s->cursor_pos), s->cfg.font_size).first);
            const float visible_w = s->cfg.w
                                  - s->cfg.padding.left - s->cfg.padding.right;
            if (cursor_px < s->scroll_offset_x)
                s->scroll_offset_x = cursor_px;
            else if (cursor_px > s->scroll_offset_x + visible_w)
                s->scroll_offset_x = cursor_px - visible_w;
        }
        tree.markDirty(handle);
        return;
    }

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
    if (!s || s->cfg.disabled) return;

    // Always attempt to re-arm text input — this is the critical fix:
    // focus() may have been called during jade_app_init before the OS
    // granted keyboard focus, so safeStartTextInput() silently did nothing.
    // Every subsequent call (e.g. on click) must retry.
    safeStartTextInput();

    if (s->focused) return;

    s->focused    = true;
    s->cursor_pos = s->text.size();
    clearSel(*s);
    tree.markDirty(handle);
}

void TextField::unfocus()
{
    TextFieldState* s = getState();
    if (!s || !s->focused) return;
    s->focused = false;
    s->composition.clear();
    s->composition_cursor = 0;
    clearSel(*s);
    safeStopTextInput();
    tree.markDirty(handle);
}

bool TextField::isFocused() const noexcept
{
    const TextFieldState* s = getState();
    return s && s->focused;
}

// ============================================================================
// handleEvent
// ============================================================================

bool TextField::handleEvent(const SDL_Event& ev)
{
    RenderNode*     n = tree.node(handle);
    TextFieldState* s = getState();
    if (!n || !s || s->cfg.disabled) return false;

    switch (ev.type) {

    // ── Mouse button down ────────────────────────────────────────────────────
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

        // ── KEY FIX: always re-arm SDL text input on every click ──────────
        // If focus() was called during jade_app_init the window may not yet
        // have had OS keyboard focus, so SDL_StartTextInput was a no-op.
        // Clicking guarantees the window is focused; call it unconditionally.
        if (!s->focused) {
            s->focused    = true;
            s->cursor_pos = s->text.size();
            clearSel(*s);
        }
        safeStartTextInput();

        // Scrollbar thumb drag
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

        // Compute clicked byte offset
        std::size_t clicked_byte = 0;
        if (s->cfg.multiline) {
            const float lx = mx - ax - s->cfg.padding.left + s->scroll_offset_x;
            const float ly = my - ay - s->cfg.padding.top  + s->scroll_offset_y;
            const int   cli = std::max(0,
                static_cast<int>(ly / s->cfg.line_height));
            const auto  lines = splitLines(s->text);
            const int   li    = std::min(cli,
                std::max(0, static_cast<int>(lines.size()) - 1));
            const auto& lsv   = lines[static_cast<std::size_t>(li)];
            const std::size_t lstart =
                static_cast<std::size_t>(lsv.data() - s->text.data());
            clicked_byte = lstart + lineHitTest(*s, lsv, lx, s->cfg.font_size);
        } else {
            const float lx = mx - ax - s->cfg.padding.left + s->scroll_offset_x;
            clicked_byte = lineHitTest(*s, s->text, lx, s->cfg.font_size);
        }

        const Uint64 now = SDL_GetTicks();
        const bool   dbl = (now - s->last_click_ms) < 400u;
        s->last_click_ms = now;

        const bool shift = (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;

        if (dbl && !shift) {
            // Double-click: select word around click position
            const auto [ws, we] = wordBoundaries(s->text, clicked_byte);
            s->sel_start  = ws;
            s->sel_end    = we;
            s->sel_active = (ws != we);
            s->cursor_pos = we;
        } else if (shift) {
            // Shift-click: extend / begin selection
            if (!s->sel_active) {
                s->sel_start  = s->cursor_pos;
                s->sel_active = true;
            }
            s->sel_end    = clicked_byte;
            s->cursor_pos = clicked_byte;
        } else {
            // Normal click: place cursor, start potential drag
            s->cursor_pos       = clicked_byte;
            clearSel(*s);
            s->text_drag_active = true;
        }

        tree.markDirty(handle);
        return true;
    }

    // ── Mouse button up ──────────────────────────────────────────────────────
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (s->scrollbar_drag) {
            s->scrollbar_drag = false;
            tree.markDirty(handle);
            return true;
        }
        s->text_drag_active = false;
        return false;

    // ── Mouse motion ─────────────────────────────────────────────────────────
    case SDL_EVENT_MOUSE_MOTION: {
        // Scrollbar drag
        if (s->scrollbar_drag) {
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

        // Drag-select
        if (s->text_drag_active && s->focused) {
            const float mx = ev.motion.x;
            const float my = ev.motion.y;
            const auto [ax, ay] = absPos(tree, handle);

            std::size_t drag_byte = 0;
            if (s->cfg.multiline) {
                const float lx = mx - ax - s->cfg.padding.left + s->scroll_offset_x;
                const float ly = my - ay - s->cfg.padding.top  + s->scroll_offset_y;
                const int   cli = std::max(0,
                    static_cast<int>(ly / s->cfg.line_height));
                const auto  lines = splitLines(s->text);
                const int   li    = std::min(cli,
                    std::max(0, static_cast<int>(lines.size()) - 1));
                const auto& lsv   = lines[static_cast<std::size_t>(li)];
                const std::size_t lstart =
                    static_cast<std::size_t>(lsv.data() - s->text.data());
                drag_byte = lstart + lineHitTest(*s, lsv, lx, s->cfg.font_size);
            } else {
                const float lx = mx - ax - s->cfg.padding.left + s->scroll_offset_x;
                drag_byte = lineHitTest(*s, s->text, lx, s->cfg.font_size);
            }

            if (!s->sel_active) {
                s->sel_start  = s->cursor_pos; // anchor = click-down position
                s->sel_active = true;
            }
            s->sel_end    = drag_byte;
            s->cursor_pos = drag_byte;
            tree.markDirty(handle);
            return true;
        }
        return false;
    }

    // ── Mouse wheel ──────────────────────────────────────────────────────────
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

    // ── IME composition ──────────────────────────────────────────────────────
    case SDL_EVENT_TEXT_EDITING: {
        if (!s->focused) return false;
        s->composition = ev.edit.text ? ev.edit.text : "";
        if (s->composition.empty()) {
            s->composition_cursor = 0;
        } else {
            // SDL gives ev.edit.start in Unicode codepoints; convert to bytes.
            std::size_t byte_off = 0;
            int cp = (ev.edit.start >= 0) ? ev.edit.start : 0;
            while (cp > 0 && byte_off < s->composition.size()) {
                byte_off = utf8Next(s->composition, byte_off);
                --cp;
            }
            s->composition_cursor = static_cast<int>(byte_off);
        }
        tree.markDirty(handle);
        return true;
    }

    // ── Text input (committed character(s)) ─────────────────────────────────
    case SDL_EVENT_TEXT_INPUT: {
        if (!s->focused) return false;

        // Lazy re-arm: if safeStartTextInput() already succeeded this is a
        // no-op; if it failed before (e.g. called before window had focus)
        // it will finally work now that the user is actively typing.
        safeStartTextInput();

        const char*       raw     = ev.text.text;
        const std::size_t raw_len = std::strlen(raw);
        if (raw_len == 0) return true;

        // Validate — SDL should always deliver valid UTF-8, but be defensive.
        if (!isValidUtf8(raw, raw_len)) return true;

        // Commit clears any in-progress preedit
        s->composition.clear();
        s->composition_cursor = 0;

        pushUndo(*s);
        replaceSelection(*s, { raw, raw_len });

        if (s->cfg.value)     s->cfg.value->set(s->text);
        if (s->cfg.on_change) s->cfg.on_change(s->text);
        if (s->cfg.multiline) scrollToCursor();
        else                  tree.markDirty(handle);
        return true;
    }

    // ── Key down ─────────────────────────────────────────────────────────────
    case SDL_EVENT_KEY_DOWN: {
        if (!s->focused) return false;
        const SDL_Keycode key = ev.key.key;
        const SDL_Keymod  mod = ev.key.mod;
        const bool primary    = (mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI)) != 0;
        const bool shift      = (mod & SDL_KMOD_SHIFT) != 0;

        // ── Clipboard, undo & select-all (Ctrl/Cmd) ───────────────────────
        if (primary) {
            if (key == SDLK_A) {
                // Select all
                s->sel_start  = 0;
                s->sel_end    = s->text.size();
                s->sel_active = !s->text.empty();
                s->cursor_pos = s->text.size();
                if (s->cfg.multiline) scrollToCursor();
                else                  tree.markDirty(handle);
                return true;
            }
            if (key == SDLK_C) {
                // Copy selection to clipboard
                if (hasSelection(*s)) {
                    auto [a, b] = normalizedSel(*s);
                    SDL_SetClipboardText(s->text.substr(a, b - a).c_str());
                }
                return true;
            }
            if (key == SDLK_X) {
                // Cut selection to clipboard
                if (hasSelection(*s)) {
                    auto [a, b] = normalizedSel(*s);
                    SDL_SetClipboardText(s->text.substr(a, b - a).c_str());
                    pushUndo(*s);
                    s->text.erase(a, b - a);
                    s->cursor_pos = a;
                    clearSel(*s);
                    if (s->cfg.value)     s->cfg.value->set(s->text);
                    if (s->cfg.on_change) s->cfg.on_change(s->text);
                    if (s->cfg.multiline) scrollToCursor();
                    else                  tree.markDirty(handle);
                }
                return true;
            }
            if (key == SDLK_V) {
                // Paste from clipboard
                char* clip = SDL_GetClipboardText();
                if (clip) {
                    const std::size_t clip_len = std::strlen(clip);
                    if (clip_len > 0 && isValidUtf8(clip, clip_len)) {
                        pushUndo(*s);
                        replaceSelection(*s, { clip, clip_len });
                        if (s->cfg.value)     s->cfg.value->set(s->text);
                        if (s->cfg.on_change) s->cfg.on_change(s->text);
                        if (s->cfg.multiline) scrollToCursor();
                        else                  tree.markDirty(handle);
                    }
                    SDL_free(clip);
                }
                return true;
            }
            if (key == SDLK_Z) {
                // Undo
                if (performUndo(*s)) {
                    if (s->cfg.value)     s->cfg.value->set(s->text);
                    if (s->cfg.on_change) s->cfg.on_change(s->text);
                    if (s->cfg.multiline) scrollToCursor();
                    else                  tree.markDirty(handle);
                }
                return true;
            }
            // Allow primary+navigation to fall through to the nav handlers below.
            // Any other Ctrl/Cmd combo is silently consumed to prevent host shortcuts
            // from firing while the field is focused.
            const bool is_nav_key =
                key == SDLK_LEFT  || key == SDLK_RIGHT ||
                key == SDLK_UP    || key == SDLK_DOWN  ||
                key == SDLK_HOME  || key == SDLK_END;
            if (!is_nav_key) return true;
        }

        // ── Helper: move cursor with optional Shift-based selection ────────
        // Captures s and shift by reference; call before any markDirty/scroll.
        auto moveCursor = [&](std::size_t new_pos) {
            if (shift) {
                if (!s->sel_active) {
                    s->sel_start  = s->cursor_pos;
                    s->sel_active = true;
                }
                s->sel_end = new_pos;
            } else {
                clearSel(*s);
            }
            s->cursor_pos = new_pos;
        };

        bool changed = false;

        // ── Backspace ─────────────────────────────────────────────────────
        if (key == SDLK_BACKSPACE) {
            if (hasSelection(*s)) {
                pushUndo(*s);
                auto [a, b] = normalizedSel(*s);
                s->text.erase(a, b - a);
                s->cursor_pos = a;
                clearSel(*s);
                changed = true;
            } else if (s->cursor_pos > 0 && !s->text.empty()) {
                // Push BEFORE mutation so undo restores the pre-erase state.
                pushUndo(*s);
                eraseLeft(s->text, s->cursor_pos);
                changed = true;
            }

        // ── Delete ────────────────────────────────────────────────────────
        } else if (key == SDLK_DELETE) {
            if (hasSelection(*s)) {
                pushUndo(*s);
                auto [a, b] = normalizedSel(*s);
                s->text.erase(a, b - a);
                s->cursor_pos = a;
                clearSel(*s);
                changed = true;
            } else if (s->cursor_pos < s->text.size()) {
                // Push BEFORE mutation so undo restores the pre-erase state.
                pushUndo(*s);
                eraseRight(s->text, s->cursor_pos);
                changed = true;
            }

        // ── Left arrow ────────────────────────────────────────────────────
        } else if (key == SDLK_LEFT) {
            if (!shift && hasSelection(*s)) {
                // Collapse selection to the near (left) edge
                s->cursor_pos = normalizedSel(*s).first;
                clearSel(*s);
            } else {
                const std::size_t new_pos = primary
                    ? (s->cfg.multiline
                        ? linePosToByteOff(s->text,
                            byteToLinePos(s->text, s->cursor_pos).line, 0)
                        : std::size_t{ 0 })
                    : utf8Prev(s->text, s->cursor_pos);
                moveCursor(new_pos);
            }
            if (s->cfg.multiline) scrollToCursor(); else tree.markDirty(handle);

        // ── Right arrow ───────────────────────────────────────────────────
        } else if (key == SDLK_RIGHT) {
            if (!shift && hasSelection(*s)) {
                // Collapse selection to the far (right) edge
                s->cursor_pos = normalizedSel(*s).second;
                clearSel(*s);
            } else {
                std::size_t new_pos;
                if (primary) {
                    if (s->cfg.multiline) {
                        const auto lines = splitLines(s->text);
                        const auto [li, col] = byteToLinePos(s->text, s->cursor_pos);
                        const std::size_t liu = static_cast<std::size_t>(li);
                        new_pos = linePosToByteOff(s->text, li,
                            liu < lines.size() ? lines[liu].size() : 0);
                    } else {
                        new_pos = s->text.size();
                    }
                } else {
                    new_pos = utf8Next(s->text, s->cursor_pos);
                }
                moveCursor(new_pos);
            }
            if (s->cfg.multiline) scrollToCursor(); else tree.markDirty(handle);

        // ── Up arrow (multiline) ──────────────────────────────────────────
        } else if (key == SDLK_UP) {
            if (s->cfg.multiline) {
                const auto [line, col] = byteToLinePos(s->text, s->cursor_pos);
                const std::size_t new_pos = primary
                    ? std::size_t{ 0 }
                    : (line > 0
                        ? linePosToByteOff(s->text, line - 1, col)
                        : s->cursor_pos);
                moveCursor(new_pos);
                scrollToCursor();
            }

        // ── Down arrow (multiline) ────────────────────────────────────────
        } else if (key == SDLK_DOWN) {
            if (s->cfg.multiline) {
                const auto [line, col] = byteToLinePos(s->text, s->cursor_pos);
                const std::size_t new_pos = primary
                    ? s->text.size()
                    : linePosToByteOff(s->text, line + 1, col);
                moveCursor(new_pos);
                scrollToCursor();
            }

        // ── Home ──────────────────────────────────────────────────────────
        } else if (key == SDLK_HOME) {
            const std::size_t new_pos = s->cfg.multiline
                ? linePosToByteOff(s->text,
                    byteToLinePos(s->text, s->cursor_pos).line, 0)
                : std::size_t{ 0 };
            moveCursor(new_pos);
            if (s->cfg.multiline) scrollToCursor(); else tree.markDirty(handle);

        // ── End ───────────────────────────────────────────────────────────
        } else if (key == SDLK_END) {
            std::size_t new_pos;
            if (s->cfg.multiline) {
                const auto lines = splitLines(s->text);
                const auto [li, col] = byteToLinePos(s->text, s->cursor_pos);
                const std::size_t liu = static_cast<std::size_t>(li);
                new_pos = linePosToByteOff(s->text, li,
                    liu < lines.size() ? lines[liu].size() : 0);
            } else {
                new_pos = s->text.size();
            }
            moveCursor(new_pos);
            if (s->cfg.multiline) scrollToCursor(); else tree.markDirty(handle);

        // ── Enter ─────────────────────────────────────────────────────────
        } else if (key == SDLK_RETURN  ||
                   key == SDLK_RETURN2 ||
                   key == SDLK_KP_ENTER) {
            if (s->cfg.multiline) {
                pushUndo(*s);
                replaceSelection(*s, "\n");
                changed = true;
            } else {
                if (s->cfg.on_submit) s->cfg.on_submit(s->text);
                unfocus();
            }

        // ── Escape ────────────────────────────────────────────────────────
        } else if (key == SDLK_ESCAPE) {
            unfocus();
        }

        if (changed) {
            if (s->cfg.value)     s->cfg.value->set(s->text);
            if (s->cfg.on_change) s->cfg.on_change(s->text);
            if (s->cfg.multiline) scrollToCursor();
            else                  tree.markDirty(handle);
        }
        return true;
    }

    default: return false;
    }
}

} // namespace pce::sdlos::widgets
