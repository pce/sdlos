#pragma once

#include "widget.hh"
#include "../render_tree.hh"

#include <SDL3/SDL.h>

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace pce::sdlos { class TextRenderer; }

namespace pce::sdlos::widgets {



/// Unified config for single-line (multiline=false) and multi-line (multiline=true) input.
struct TextFieldConfig {

    bool multiline = false;

    std::string_view      placeholder;
    Signal<std::string>*  value = nullptr; // non-owning; must outlive node

    float  w          = 320.f;
    float  h          = 44.f;

    // multiline sizing hints; ignored when 0
    int    rows        = 0;      // h = rows * line_height + padding
    int    cols        = 0;      // w = cols * char_width  + padding + scrollbar_w + 4
    float  line_height = 22.f;
    float  scrollbar_w = 6.f;

    float  font_size  = 17.f;
    Edges  padding    = Edges::horizontal(12.f);

    Color bg                = Color::hex(0x1c, 0x1c, 0x1e, 0xcc);
    Color bg_focused        = Color::hex(0x2c, 0x2c, 0x2e, 0xee);
    Color border            = Color::hex(0x48, 0x48, 0x4a, 0xff);
    Color border_focus      = Color::systemBlue();
    Color text_color        = Color::label();
    Color placeholder_color = Color::secondaryLabel();
    Color cursor_color      = Color::systemBlue();

    std::size_t max_length = 0;
    bool        secure     = false; // single-line: render '*' instead of chars
    bool        disabled   = false;

    std::function<void(std::string_view)> on_change;
    std::function<void(std::string_view)> on_submit; // single-line Enter; unused in multiline
};

// single-line alias — all existing TextBoxConfig call sites compile unchanged
using TextBoxConfig = TextFieldConfig;

/// Multi-line convenience subtype — pre-sets multiline defaults.
struct TextAreaConfig : TextFieldConfig {
    TextAreaConfig()
    {
        multiline   = true;
        rows        = 4;
        line_height = 22.f;
        scrollbar_w = 6.f;
        padding     = Edges::all(8.f);
    }
};


struct TextFieldState {
    std::string  text;
    std::size_t  cursor_pos = 0; // byte offset; UTF-8-safe
    bool         focused    = false;

    // ── Selection (byte offsets) ──────────────────────────────────────────
    // sel_start = anchor end (fixed);  sel_end = active end (tracks cursor).
    // normalizedSel() always returns [min, max] regardless of anchor direction.
    std::size_t  sel_start  = 0;
    std::size_t  sel_end    = 0;
    bool         sel_active = false;

    // ── IME preedit / composition ─────────────────────────────────────────
    std::string  composition;            // UTF-8 preedit text from TEXT_EDITING
    int          composition_cursor = 0; // byte offset of caret inside composition

    // ── Undo ─────────────────────────────────────────────────────────────
    std::vector<std::string>          undo_stack;
    static constexpr std::size_t      kUndoLimit = 64;

    // ── Mouse click / drag tracking ───────────────────────────────────────
    Uint64 last_click_ms    = 0;     // SDL_GetTicks() at last MOUSE_BUTTON_DOWN
    bool   text_drag_active = false; // true while LMB held for drag-select

    float scroll_offset_y = 0.f;
    float scroll_offset_x = 0.f;

    bool  scrollbar_drag = false; // multiline
    float drag_start_y   = 0.f;
    float drag_start_off = 0.f;

    TextFieldConfig cfg;

    // set on first draw; used by handleEvent for pixel-accurate cursor placement
    pce::sdlos::TextRenderer* text_renderer = nullptr;

    // recomputed in update(); only meaningful in multiline mode
    float content_w = 0.f;
    float content_h = 0.f;
    float total_h   = 0.f;
    float total_w   = 0.f;
};


struct TextField : WidgetView<TextFieldState> {

    [[nodiscard]] std::string_view getText() const noexcept;
    void setText(std::string text);
    void clear();

    /// Always 1 in single-line mode.
    [[nodiscard]] int lineCount() const noexcept;

    /// Scrolls so the cursor line is visible. No-op in single-line mode.
    void scrollToCursor();

    void focus();
    void unfocus();
    [[nodiscard]] bool isFocused() const noexcept;

    bool handleEvent(const SDL_Event& ev);
};

using TextBox  = TextField;
using TextArea = TextField;

[[nodiscard]] TextField makeTextField(RenderTree& tree, TextFieldConfig cfg);

[[nodiscard]] inline TextField makeTextBox(RenderTree& tree, TextFieldConfig cfg)
{
    cfg.multiline = false;
    return makeTextField(tree, std::move(cfg));
}

[[nodiscard]] inline TextField makeTextArea(RenderTree& tree, TextAreaConfig cfg)
{
    cfg.multiline = true;
    return makeTextField(tree, std::move(cfg));
}

} // namespace pce::sdlos::widgets
