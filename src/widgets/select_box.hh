#pragma once

// ---------------------------------------------------------------------------
// select_box.hh — SelectBox widget
//
// A dropdown option picker built on WidgetView<SelectBoxState>.
//
// Design notes (C++23):
//   • std::span<const SelectOption> in config — zero-alloc for static option
//     arrays; options are copied into state once at makeSelectBox() time.
//   • std::move_only_function for on_change — unique ownership of captures.
//   • Dropdown drawn inline by the draw() callback (no extra tree nodes).
//     The dropdown "overflows" the node bounds when open; it renders on top
//     of siblings that appear earlier in tree order.  Append the SelectBox
//     last among siblings when overlap matters.
//   • Self-closing in jade — value is the binding:
//       select_box(id="lang" value="en")
//   • options are registered in jade_app_init or passed as a span:
//       auto sel = makeSelectBox(tree, {
//           .options  = kLangOptions,
//           .value    = &lang_signal,
//           .selected = "en",
//       });
// ---------------------------------------------------------------------------

#include "widget.hh"
#include "../render_tree.hh"

#include <SDL3/SDL.h>

#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pce::sdlos::widgets {

// ============================================================================
// SelectOption — one item in the dropdown list
// ============================================================================

struct SelectOption {
    std::string value;  // key used in Signal / on_change callback
    std::string label;  // display text; if empty, value is shown instead

    [[nodiscard]] std::string_view displayLabel() const noexcept
    {
        return label.empty() ? std::string_view{ value }
                             : std::string_view{ label };
    }
};

// ============================================================================
// SelectBoxConfig
// ============================================================================

struct SelectBoxConfig {
    // Options — passed as a span; copied into state during makeSelectBox().
    // Use a static / constexpr array for zero-heap option lists:
    //   static constexpr SelectOption kOpts[] = {{"en","English"},{"de","Deutsch"}};
    //   cfg.options = kOpts;
    std::span<const SelectOption> options;

    // Initial selection — matched against SelectOption::value.
    // If no match, the first option (if any) is selected.
    std::string_view selected;

    // Optional two-way binding (non-owning; must outlive the node).
    Signal<std::string>* value = nullptr;

    // Sizing (position resolved by parent layout — no x / y).
    float w = 200.f;
    float h = 40.f;

    // Row height inside the open dropdown list.
    float item_h = 36.f;

    // Typography
    float font_size = 15.f;
    Edges padding   = Edges::horizontal(12.f);

    // Palette — control (closed state)
    Color bg            = Color::hex(0x2c, 0x2c, 0x2e, 0xee);
    Color bg_open       = Color::hex(0x3a, 0x3a, 0x3c, 0xff);
    Color border        = Color::hex(0x48, 0x48, 0x4a, 0xff);
    Color border_open   = Color::systemBlue();
    Color text_color    = Color::label();
    Color arrow_color   = Color::systemGray();

    // Palette — dropdown list
    Color dropdown_bg       = Color::hex(0x2c, 0x2c, 0x2e, 0xf5);
    Color item_hover_bg     = Color::hex(0x3a, 0x3a, 0x3c, 0xff);
    Color item_selected_bg  = Color::hex(0x0a, 0x84, 0xff, 0x33);
    Color item_text         = Color::label();
    Color item_selected_text = Color::systemBlue();

    // Callback — std::function, string_view arg (zero-copy on notify).
    std::function<void(std::string_view value)> on_change;
};

// ============================================================================
// Internal state — stored as std::any<shared_ptr<SelectBoxState>>
// ============================================================================

struct SelectBoxState {
    // Runtime state
    std::vector<SelectOption> options;   // owned copy of the span
    std::size_t selected_idx  = 0;       // index into options
    std::size_t hovered_idx   = SIZE_MAX; // index under mouse; SIZE_MAX = none
    bool        is_open       = false;

    // Config — owned here (moved in by makeSelectBox).
    // move_only_function makes this struct move-only; always heap-allocated.
    SelectBoxConfig cfg;

    // Convenience: resolved selected value / label.
    [[nodiscard]] std::string_view selectedValue() const noexcept
    {
        return options.empty() ? std::string_view{}
                               : std::string_view{ options[selected_idx].value };
    }

    [[nodiscard]] std::string_view selectedLabel() const noexcept
    {
        return options.empty() ? std::string_view{}
                               : options[selected_idx].displayLabel();
    }

    // Total height of the open dropdown panel.
    [[nodiscard]] float dropdownH() const noexcept
    {
        return static_cast<float>(options.size()) * cfg.item_h;
    }
};

// ============================================================================
// SelectBox — lightweight non-owning view
// ============================================================================

struct SelectBox : WidgetView<SelectBoxState> {

    // ---- Selection --------------------------------------------------------

    /// Currently selected option value; empty if no options.
    [[nodiscard]] std::string_view getSelected() const noexcept;

    /// Select by value string.  No-op if value not found.
    /// Fires on_change and updates the Signal binding.
    void select(std::string_view value);

    /// Select by zero-based index.  No-op if out of range.
    void selectAt(std::size_t index);

    // ---- Dropdown state ---------------------------------------------------

    void open();
    void close();
    void toggle();

    [[nodiscard]] bool isOpen() const noexcept;

    // ---- Event routing ----------------------------------------------------

    /// Feed a raw SDL_Event.  Returns true when the event was consumed.
    ///
    /// Handles:
    ///   MOUSE_BUTTON_DOWN — click control to open/close; click item to select
    ///   MOUSE_MOTION      — track hovered item for highlight
    ///   KEY_DOWN          — Up/Down navigate; Enter confirm; Escape close
    ///                       (only when isOpen() or isFocused by convention)
    bool handleEvent(const SDL_Event& ev);
};

/// Factory — allocates a node in `tree`, copies options, resolves initial
/// selection, and returns a view.  Caller must appendChild the result.
[[nodiscard]] SelectBox makeSelectBox(RenderTree& tree, SelectBoxConfig cfg);

} // namespace pce::sdlos::widgets
