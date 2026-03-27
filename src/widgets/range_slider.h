#pragma once

#include "widget.h"
#include "../render_tree.h"

#include <SDL3/SDL.h>

#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace pce::sdlos::widgets {


struct RangeSliderConfig {

    // Range
    float min   = 0.f;    ///< Minimum value (inclusive).
    float max   = 100.f;  ///< Maximum value (inclusive).
    float value = 0.f;    ///< Initial value; clamped + snapped at make time.

    /// Snap increment. 0 = fully continuous (no quantisation).
    float step  = 0.f;

    // Geometry
    float w       = 200.f;  ///< Total widget width (including optional label).
    float h       = 32.f;   ///< Total widget height.
    float track_h = 4.f;    ///< Track bar height.

    /// Thumb half-size in pixels. The drawn square is (thumb_r*2 × thumb_r*2).
    float thumb_r = 8.f;

    // Value label
    bool  show_value = false;
    float label_w    = 44.f;   ///< Pixel width reserved for the label column.
    float font_size  = 13.f;

    // Two-way Signal binding
    /// Optional non-owning pointer to a Signal<float>.
    /// Updated (via Signal::set) on every value change. Must outlive the node.
    Signal<float>* value_signal = nullptr;

    // Palette
    Color track_bg     = Color::fillTertiary();           ///< Unfilled track.
    Color track_fill   = Color::systemBlue();             ///< Filled (left of thumb).
    Color thumb_color  = Color::label();                  ///< Normal thumb.
    Color thumb_hover  = Color::hex(0xcc, 0xcc, 0xcc);   ///< Thumb on hover.
    Color thumb_drag   = Color::systemBlue();             ///< Thumb while dragging.
    Color value_color  = Color::secondaryLabel();         ///< Value label text.

    std::function<void(float)> on_change;
};


struct RangeSliderState {
    float value         = 0.f;
    bool  dragging      = false;  ///< True while the left mouse button is held.
    bool  focused       = false;  ///< True when the widget has keyboard focus.
    bool  thumb_hovered = false;  ///< True when the pointer is over the thumb.

    RangeSliderConfig cfg;


    /// Normalised position in [0, 1] — used by draw and hit-test.
    [[nodiscard]] float fraction() const noexcept
    {
        if (cfg.max <= cfg.min) return 0.f;
        return (value - cfg.min) / (cfg.max - cfg.min);
    }

    /// Effective pixel width of the track (may reserve space for the label).
    [[nodiscard]] float trackW() const noexcept
    {
        return cfg.show_value
            ? cfg.w - cfg.label_w - 8.f   // 8 px gap between track and label
            : cfg.w;
    }
};


struct RangeSlider : WidgetView<RangeSliderState> {


    /// Returns the current value; 0 when the handle is invalid.
    [[nodiscard]] float getValue() const noexcept;

    /// Clamp + snap the given value, update any Signal binding, fire on_change,
    /// and mark the node dirty_render.  No-op when the clamped value is unchanged.
    void setValue(float v);

    /// Step down by cfg.step (or 1 % of range when step == 0).
    void stepDown(int multiplier = 1);

    /// Step up by cfg.step (or 1 % of range when step == 0).
    void stepUp(int multiplier = 1);

    /**
     * @brief Focus
     */
    void focus();
    /**
     * @brief Unfocus
     */
    void unfocus();
    /**
     * @brief Checks whether focused
     *
     * @return true on success, false on failure
     */
    [[nodiscard]] bool isFocused() const noexcept;


    /**
     * @brief Checks whether dragging
     *
     * @return true on success, false on failure
     */
    [[nodiscard]] bool isDragging() const noexcept;


    /**
     * @brief Returns state
     *
     * @return Pointer to the result, or nullptr on failure
     */
    [[nodiscard]] RangeSliderState* getState() noexcept
    {
        return WidgetView<RangeSliderState>::getState();
    }

    /**
     * @brief Returns state
     *
     * @return Pointer to the result, or nullptr on failure
     */
    [[nodiscard]] const RangeSliderState* getState() const noexcept
    {
        return WidgetView<RangeSliderState>::getState();
    }


    /**
     * @brief Handles event
     *
     * @param ev  SDL3 input or window event
     *
     * @return true on success, false on failure
     */
    bool handleEvent(const SDL_Event& ev);
};

// Factory returns a live RangeSlider view.
/**
 * @brief Constructs and returns range slider
 *
 * @param tree  Red channel component [0, 1]
 * @param cfg   Configuration options struct
 *
 * @return RangeSlider result
 */
[[nodiscard]] RangeSlider makeRangeSlider(RenderTree& tree, RangeSliderConfig cfg);

/**
 * @brief Binds input widgets
 *
 * @param tree  Red channel component [0, 1]
 * @param root  Red channel component [0, 1]
 */
void bindInputWidgets(RenderTree& tree, NodeHandle root);

} // namespace pce::sdlos::widgets
