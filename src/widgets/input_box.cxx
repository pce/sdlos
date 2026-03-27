// input_box.cxx — InputBox widget implementation.
//
// Namespace : pce::sdlos::widgets
// File      : src/widgets/input_box.cxx
//
// State model
// ===========
// All mutable widget state lives in a heap-allocated InputBoxState managed
// by a shared_ptr.  The shared_ptr is stored in RenderNode::state (as
// std::any<shared_ptr<InputBoxState>>) and is also captured by value in the
// draw lambda.  This gives the lambda a stable pointer to the state without
// ever storing a raw pointer to a slot_map entry (which can move on insert).
//
// Draw order (back-to-front)
// ==========================
//   1. Background rect
//   2. Border  (four 1-px rects)
//   3. Placeholder text  OR  active text
//   4. Blinking cursor rect (when focused)
//
// Cursor blink
// ============
// Computed entirely from SDL_GetTicks() inside draw() — no update() callback
// needed.  Desktop::tick() calls markSubtreeDirty() on the search overlay
// every frame while it is visible, so draw() is already called every frame.
//
// No exception handling.  A bad_any_cast is a programmer bug — assert in
// debug, crash-fast in release.

#include "input_box.hh"

#include <SDL3/SDL.h>

#include <any>
#include <cassert>
#include <memory>
#include <string>
#include <string_view>

namespace pce::sdlos::widgets {

// ===========================================================================
// Internal helpers
// ===========================================================================

namespace {

// Retrieve a non-owning pointer to the shared state stored in node->state.
// Returns nullptr on type mismatch (programmer error; assert fires in debug).
std::shared_ptr<InputBoxState>* sharedStateOf(RenderNode* node) noexcept
{
    if (!node) return nullptr;
    return std::any_cast<std::shared_ptr<InputBoxState>>(&node->state);
}

// Insert UTF-8 bytes at the cursor position, honouring max_length (bytes).
// Returns true if the text changed.
bool insertText(InputBoxState& s, std::string_view chars)
{
    std::string next = s.text;
    next.insert(s.cursor_pos, chars);

    if (s.cfg.max_length > 0 && next.size() > s.cfg.max_length)
        return false;

    s.text        = std::move(next);
    s.cursor_pos += chars.size();
    return true;
}

// Erase the UTF-8 code-point immediately to the left of the cursor.
// Returns true if anything was erased.
bool eraseLeft(InputBoxState& s)
{
    if (s.cursor_pos == 0 || s.text.empty()) return false;

    // Walk back past UTF-8 continuation bytes (10xxxxxx).
    std::size_t pos = s.cursor_pos - 1;
    while (pos > 0 &&
           (static_cast<unsigned char>(s.text[pos]) & 0xC0) == 0x80)
        --pos;

    const std::size_t erased = s.cursor_pos - pos;
    s.text.erase(pos, erased);
    s.cursor_pos = pos;
    return true;
}

// Draw a 1-px border as four axis-aligned filled rects.
void drawBorder(RenderContext& ctx,
                float x, float y, float w, float h,
                const Color& c)
{
    ctx.drawRect(x,           y,           w,      1.f,     c.r, c.g, c.b, c.a); // top
    ctx.drawRect(x,           y + h - 1.f, w,      1.f,     c.r, c.g, c.b, c.a); // bottom
    ctx.drawRect(x,           y + 1.f,     1.f,    h - 2.f, c.r, c.g, c.b, c.a); // left
    ctx.drawRect(x + w - 1.f, y + 1.f,    1.f,    h - 2.f, c.r, c.g, c.b, c.a); // right
}

} // anonymous namespace

// ===========================================================================
// inputBox — factory
// ===========================================================================

Widget inputBox(RenderTree& tree, InputBoxConfig cfg)
{
    const NodeHandle h = tree.alloc();
    RenderNode*      n = tree.node(h);
    assert(n && "inputBox: alloc returned invalid handle");

    n->x = cfg.x;
    n->y = cfg.y;
    n->w = cfg.w;
    n->h = cfg.h;
    n->dirty_render = true;
    n->dirty_layout = false;

    // Heap-allocate state so the draw lambda can capture a stable pointer.
    auto state = std::make_shared<InputBoxState>();
    state->cfg  = std::move(cfg);

    // Store the shared_ptr in the node so external helpers can reach it.
    n->state = state;   // std::any holds shared_ptr<InputBoxState>

    // -----------------------------------------------------------------------
    // draw — void(RenderContext&)
    //
    // Captures the shared_ptr<InputBoxState> by value.  The RenderTree owns
    // the node; the node owns the shared_ptr via std::any; the lambda owns a
    // second reference.  The state is freed when both the node and the lambda
    // are destroyed (i.e. when tree.free(h) is called).
    // -----------------------------------------------------------------------
    n->draw = [state](RenderContext& ctx) {
        const InputBoxState& s   = *state;
        const InputBoxConfig& cfg = s.cfg;

        const float x = cfg.x;
        const float y = cfg.y;
        const float w = cfg.w;
        const float h = cfg.h;

        // 1. Background.
        const Color& bg = s.focused ? cfg.bg_focused : cfg.bg;
        ctx.drawRect(x, y, w, h, bg.r, bg.g, bg.b, bg.a);

        // 2. Border.
        const Color& bord = s.focused ? cfg.border_focus : cfg.border;
        drawBorder(ctx, x + 1.f, y + 1.f, w - 2.f, h - 2.f, bord);

        // 3. Placeholder or active text.
        const float tx = x + cfg.padding.left;
        const float ty = y + (h - cfg.font_size) * 0.5f;

        if (s.text.empty() && !cfg.placeholder.empty()) {
            const Color& pc = cfg.placeholder_color;
            ctx.drawText(cfg.placeholder, tx, ty,
                         cfg.font_size, pc.r, pc.g, pc.b, pc.a);
        } else if (!s.text.empty()) {
            const Color& tc = cfg.text_color;
            // Secure fields replace every glyph with '*'.
            const std::string& display = cfg.secure
                ? std::string(s.text.size(), '*')
                : s.text;
            ctx.drawText(display, tx, ty, cfg.font_size,
                         tc.r, tc.g, tc.b, tc.a);
        }

        // 4. Cursor — blinking at 0.5 Hz, derived from wall-clock time.
        //    Desktop::tick() re-dirties the overlay every frame while visible,
        //    so this draw callback is invoked on every frame; no update()
        //    callback or explicit markDirty() is needed for the blink.
        if (s.focused) {
            const Uint64 ms    = SDL_GetTicks();
            const bool   blink = (ms / 500u) % 2u == 0u;

            if (blink) {
                // Approximate cursor x with a fixed char-width factor.
                // Phase 2 will replace this with TextRenderer::dimensions().
                const float char_w   = cfg.font_size * 0.55f;
                const float cursor_x = tx + static_cast<float>(s.cursor_pos) * char_w;
                const float cursor_y = y + (h - cfg.font_size * 1.25f) * 0.5f;
                const Color& cc = cfg.cursor_color;
                ctx.drawRect(cursor_x, cursor_y, 2.f, cfg.font_size * 1.25f,
                             cc.r, cc.g, cc.b, cc.a);
            }
        }
    };

    // No update() callback — blink state is computed from wall-clock time in draw().

    return h;
}

// ===========================================================================
// Event forwarding
// ===========================================================================

void inputBoxHandleEvent(RenderTree& tree, NodeHandle handle,
                         const SDL_Event& event)
{
    RenderNode* node = tree.node(handle);
    if (!node) return;

    auto* sp = sharedStateOf(node);
    assert(sp && "inputBoxHandleEvent: state type mismatch");
    if (!sp || !*sp) return;

    InputBoxState& s = **sp;
    if (s.cfg.disabled) return;

    switch (event.type) {

    // Focus / unfocus by hit-test on mouse click.
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        const float mx  = event.button.x;
        const float my  = event.button.y;
        const float bx  = s.cfg.x;
        const float by  = s.cfg.y;
        const bool  hit = (mx >= bx && mx < bx + s.cfg.w &&
                           my >= by && my < by + s.cfg.h);
        if (hit && !s.focused)
            inputBoxFocus(tree, handle);
        else if (!hit && s.focused)
            inputBoxUnfocus(tree, handle);
        break;
    }

    // Printable characters from the OS input method.
    case SDL_EVENT_TEXT_INPUT: {
        if (!s.focused) break;
        if (insertText(s, event.text.text)) {
            if (s.cfg.value)     s.cfg.value->set(s.text);
            if (s.cfg.on_change) s.cfg.on_change(s.text);
            tree.markDirty(handle);
        }
        break;
    }

    // Navigation, editing, submit.
    case SDL_EVENT_KEY_DOWN: {
        if (!s.focused) break;
        const SDL_Keycode key = event.key.key;
        const SDL_Keymod  mod = event.key.mod;

        if (key == SDLK_BACKSPACE) {
            if (eraseLeft(s)) {
                if (s.cfg.value)     s.cfg.value->set(s.text);
                if (s.cfg.on_change) s.cfg.on_change(s.text);
                tree.markDirty(handle);
            }
        } else if (key == SDLK_DELETE) {
            if (s.cursor_pos < s.text.size()) {
                s.text.erase(s.cursor_pos, 1);
                if (s.cfg.value)     s.cfg.value->set(s.text);
                if (s.cfg.on_change) s.cfg.on_change(s.text);
                tree.markDirty(handle);
            }
        } else if (key == SDLK_LEFT) {
            if (s.cursor_pos > 0) {
                --s.cursor_pos;
                while (s.cursor_pos > 0 &&
                       (static_cast<unsigned char>(s.text[s.cursor_pos]) & 0xC0) == 0x80)
                    --s.cursor_pos;
                tree.markDirty(handle);
            }
        } else if (key == SDLK_RIGHT) {
            if (s.cursor_pos < s.text.size()) {
                ++s.cursor_pos;
                while (s.cursor_pos < s.text.size() &&
                       (static_cast<unsigned char>(s.text[s.cursor_pos]) & 0xC0) == 0x80)
                    ++s.cursor_pos;
                tree.markDirty(handle);
            }
        } else if (key == SDLK_HOME ||
                   (key == SDLK_A && (mod & SDL_KMOD_CTRL))) {
            if (s.cursor_pos != 0) {
                s.cursor_pos = 0;
                tree.markDirty(handle);
            }
        } else if (key == SDLK_END ||
                   (key == SDLK_E && (mod & SDL_KMOD_CTRL))) {
            const std::size_t end = s.text.size();
            if (s.cursor_pos != end) {
                s.cursor_pos = end;
                tree.markDirty(handle);
            }
        } else if (key == SDLK_RETURN  ||
                   key == SDLK_RETURN2 ||
                   key == SDLK_KP_ENTER) {
            if (s.cfg.on_submit) s.cfg.on_submit(s.text);
            inputBoxUnfocus(tree, handle);
        } else if (key == SDLK_ESCAPE) {
            inputBoxUnfocus(tree, handle);
        }
        break;
    }

    default: break;
    }
}

// ===========================================================================
// Focus helpers
// ===========================================================================

void inputBoxFocus(RenderTree& tree, NodeHandle handle)
{
    RenderNode* node = tree.node(handle);
    if (!node) return;
    auto* sp = sharedStateOf(node);
    assert(sp && "inputBoxFocus: state type mismatch");
    if (!sp || !*sp) return;

    InputBoxState& s = **sp;
    if (s.focused) return;

    s.focused        = true;
    s.cursor_pos     = s.text.size();   // place cursor at end on focus

    SDL_StartTextInput(nullptr);        // enable IME / text input events
    tree.markDirty(handle);
}

void inputBoxUnfocus(RenderTree& tree, NodeHandle handle)
{
    RenderNode* node = tree.node(handle);
    if (!node) return;
    auto* sp = sharedStateOf(node);
    assert(sp && "inputBoxUnfocus: state type mismatch");
    if (!sp || !*sp) return;

    InputBoxState& s = **sp;
    if (!s.focused) return;

    s.focused = false;

    SDL_StopTextInput(nullptr);
    tree.markDirty(handle);
}

// ===========================================================================
// Text accessors
// ===========================================================================

std::string inputBoxGetText(const RenderTree& tree, NodeHandle handle)
{
    const RenderNode* node = tree.node(handle);
    if (!node) return {};
    const auto* sp = std::any_cast<std::shared_ptr<InputBoxState>>(&node->state);
    return (sp && *sp) ? (*sp)->text : std::string{};
}

void inputBoxSetText(RenderTree& tree, NodeHandle handle, std::string text)
{
    RenderNode* node = tree.node(handle);
    if (!node) return;
    auto* sp = sharedStateOf(node);
    if (!sp || !*sp) return;

    InputBoxState& s = **sp;
    s.text       = std::move(text);
    s.cursor_pos = s.text.size();

    if (s.cfg.value)     s.cfg.value->set(s.text);
    if (s.cfg.on_change) s.cfg.on_change(s.text);
    tree.markDirty(handle);
}

} // namespace pce::sdlos::widgets
