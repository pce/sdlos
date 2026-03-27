#pragma once

// input_box.hh — Declarative InputBox widget for sdlos::widgets.
//
// Namespace : sdlos::widgets
// File      : src/widgets/input_box.hh
//
// Design
// ======
// An InputBox is a text-entry field that follows the iOS/macOS dark-mode
// visual language: rounded pill shape, translucent dark background, blue
// focus ring, greyed-out placeholder label, and a blinking cursor.
//
// Because sdlos has no retained-mode widget tree of its own, an InputBox is
// simply a *subtree* of RenderNodes inside a RenderTree.  The factory
// function `inputBox()` allocates one root node (and any child nodes it
// needs), attaches draw/update lambdas, wires Signals, and returns the root
// NodeHandle.  The caller attaches it to the scene with:
//
//   tree.appendChild(parent, inputBox(tree, cfg));
//
// Declarative, functional style
// ==============================
//   Signal<std::string> query;
//
//   Widget searchInput = widgets::inputBox(tree, {
//       .placeholder = "Search…",
//       .x = 200.f, .y = 300.f,
//       .w = 440.f, .h = 44.f,
//       .value      = &query,
//   });
//
// State machine
// =============
// The InputBox manages three internal states:
//
//   Normal   — default; renders with bg colour and placeholder text.
//   Focused  — after SDL_EVENT_MOUSE_BUTTON_DOWN inside the bounds or an
//              explicit focus(handle) call; shows blue border + cursor.
//   Disabled — draw callback renders with reduced alpha; ignores input.
//
// The *current text* is stored in the node's std::any state so it lives
// exactly as long as the node does.  If `value` is non-null, every keystroke
// calls Signal<std::string>::set() so observers are notified reactively.
//
// Keyboard handling
// =================
// The InputBox does *not* install its own SDL event listener.  Instead the
// owner (typically Desktop) forwards raw SDL_Events to
// `InputBox::handleEvent(tree, handle, event)`.  This keeps the widget
// stateless with respect to the event system and makes it easy to test.
//
// Draw order (back → front)
// =========================
//   1. Background rect           (bg / bg_focused)
//   2. Border rect               (1 px inset, border / border_focus colour)
//   3. Placeholder text          (if text is empty; secondary-label colour)
//      OR active text            (label colour)
//   4. Cursor rect               (blinking at 0.5 Hz, visible when focused)
//
// Text rendering
// ==============
// Text is drawn via RenderContext::drawText(), which delegates to
// TextRenderer.  If no font is loaded, the text layers are silently skipped;
// the rect layers still render so the box remains visually present.

#include "widget.hh"          // Color, Edges, FontStyle, Widget alias
#include "../render_tree.hh"  // RenderTree, NodeHandle, Signal<T>

#include <SDL3/SDL.h>

#include <functional>
#include <string>

namespace pce::sdlos::widgets {

// ---------------------------------------------------------------------------
// InputBoxConfig — declarative configuration for an InputBox.
//
// Every field has a sensible default so callers only need to specify what
// differs.  The struct is cheap to copy (no heap members beyond std::string)
// so it is passed by value to the factory function.
// ---------------------------------------------------------------------------

struct InputBoxConfig {

    // ---- Content ---------------------------------------------------------

    /// Displayed when `value` is empty or null.
    std::string placeholder = "";

    /// Reactive value backing.  If non-null, every keystroke propagates to
    /// the Signal so bound observers (e.g. a search results list) update.
    /// The InputBox does *not* take ownership; the Signal must outlive the node.
    Signal<std::string>* value = nullptr;

    // ---- Layout (pixel space, top-left origin) ---------------------------

    float x = 0.f;
    float y = 0.f;
    float w = 320.f;
    float h = 44.f;

    // ---- Appearance — background -----------------------------------------

    /// Fill colour in Normal state.
    Color bg         = Color::hex(0x1c, 0x1c, 0x1e, 0xcc);

    /// Fill colour in Focused state (slightly brighter).
    Color bg_focused = Color::hex(0x2c, 0x2c, 0x2e, 0xee);

    // ---- Appearance — border ---------------------------------------------

    /// Border colour in Normal state (1 px rendered as a 1-px-inset rect).
    Color border       = Color::hex(0x48, 0x48, 0x4a, 0xff);

    /// Border colour in Focused state (system blue).
    Color border_focus = Color::systemBlue();

    // ---- Appearance — text -----------------------------------------------

    Color placeholder_color = Color::secondaryLabel();
    Color text_color        = Color::label();

    /// Cursor / caret colour.
    Color cursor_color      = Color::systemBlue();

    // ---- Typography ------------------------------------------------------

    float font_size = 17.f;

    /// Horizontal padding between the border and the text / placeholder.
    /// Vertical alignment is centred automatically.
    Edges padding = Edges::horizontal(12.f);

    // ---- Behaviour -------------------------------------------------------

    /// Maximum number of Unicode codepoints the user can enter.
    /// 0 = unlimited.
    std::size_t max_length = 0;

    /// If true, characters are replaced with '●' in the draw layer
    /// (password / PIN input).  The Signal still receives the real text.
    bool secure = false;

    /// If true, the widget rejects keyboard input but still renders.
    bool disabled = false;

    /// Called when the user presses Return / Enter.
    std::function<void(const std::string&)> on_submit;

    /// Called whenever the text changes (alternative to watching `value`).
    std::function<void(const std::string&)> on_change;
};

// ---------------------------------------------------------------------------
// InputBoxState — mutable per-node state stored in RenderNode::state.
//
// Stored as std::any inside the node so it lives exactly as long as the node.
// Accessed inside draw/update lambdas via std::any_cast<InputBoxState>.
// ---------------------------------------------------------------------------

struct InputBoxState {
    std::string text;           // current input text
    bool        focused = false;
    double      last_blink_toggle = 0.0;  // SDL_GetTicks() / 1000.0
    bool        cursor_visible    = true;
    std::size_t cursor_pos        = 0;    // byte offset in text

    // Copy of the config so the draw lambda is self-contained.
    InputBoxConfig cfg;
};

// ---------------------------------------------------------------------------
// inputBox — allocate a RenderNode subtree for an InputBox widget.
//
// Returns the root NodeHandle.  Attach it to your scene:
//
//   tree.appendChild(parent, inputBox(tree, cfg));
//
// The returned handle is stable (generational slot_map) and safe to store.
// Call tree.free(handle) to destroy the widget and all its children.
// ---------------------------------------------------------------------------

[[nodiscard]]
Widget inputBox(RenderTree& tree, InputBoxConfig cfg);

// ---------------------------------------------------------------------------
// InputBox event forwarding
//
// The Desktop (or any other owner) calls handleEvent() to route raw SDL
// events to a specific InputBox node.  The function updates the node's
// InputBoxState and marks the node render-dirty when anything changes.
//
// Handled event types:
//   SDL_EVENT_MOUSE_BUTTON_DOWN  → focus / unfocus based on hit-test.
//   SDL_EVENT_KEY_DOWN           → character input, backspace, cursor nav,
//                                  Return (calls on_submit).
//   SDL_EVENT_TEXT_INPUT         → printable character(s) from the OS IME.
// ---------------------------------------------------------------------------

/// Route a raw SDL_Event to the InputBox identified by `handle`.
/// `tree` is needed to re-fetch the node pointer and call markDirty().
void inputBoxHandleEvent(RenderTree& tree, NodeHandle handle,
                         const SDL_Event& event);

/// Programmatically give keyboard focus to the InputBox.
void inputBoxFocus(RenderTree& tree, NodeHandle handle);

/// Programmatically remove keyboard focus from the InputBox.
void inputBoxUnfocus(RenderTree& tree, NodeHandle handle);

/// Return the current text stored in the InputBox node, or "" if invalid.
[[nodiscard]]
std::string inputBoxGetText(const RenderTree& tree, NodeHandle handle);

/// Replace the current text programmatically and notify the value Signal.
void inputBoxSetText(RenderTree& tree, NodeHandle handle,
                     std::string text);

} // namespace pce::sdlos::widgets
