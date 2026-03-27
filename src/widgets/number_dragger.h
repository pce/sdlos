#pragma once

#include "widget.h"
#include "../render_tree.h"
#include <SDL3/SDL.h>
#include <functional>
#include <memory>
#include <string>

namespace pce::sdlos::widgets {

struct NumberDraggerConfig {
    float min        = -1e9f;
    float max        =  1e9f;
    float value      =  0.f;
    float step       =  1.f;  // snap + key increment; 0 = continuous
    float drag_speed =  0.f;  // px→value; 0 = auto from range/step

    float w         = 80.f;
    float h         = 28.f;
    float font_size = 13.f;

    Color bg          = Color::clear();
    Color bg_hover    = Color::hex(0xff, 0xff, 0xff, 0x18);
    Color bg_drag     = Color::hex(0x63, 0x66, 0xf1, 0x22);
    Color bg_edit     = Color::hex(0x1c, 0x1c, 0x1e, 0xcc);
    Color text_color  = Color::label();
    Color arrow_color = Color::hex(0xff, 0xff, 0xff, 0x50);
    Color cursor_color = Color::systemBlue();
    Color border_edit  = Color::systemBlue();

    Signal<float>* value_signal = nullptr;
    std::function<void(float)> on_change;
    std::function<void(float)> on_commit;  // fired only on edit-mode Enter/commit
};

enum class DraggerMode : uint8_t { Idle, PendingDrag, Dragging, Editing };

struct NumberDraggerState {
    float       value         = 0.f;
    DraggerMode mode          = DraggerMode::Idle;
    bool        hovered       = false;
    float       drag_start_x  = 0.f;
    float       drag_start_val = 0.f;
    std::string edit_text;
    std::size_t cursor_pos   = 0;
    float       pre_edit_val = 0.f;

    NumberDraggerConfig cfg;
    pce::sdlos::TextRenderer* text_renderer = nullptr;

    /**
     * @brief Effective drag speed
     *
     * @return Computed floating-point value
     */
    [[nodiscard]] float effectiveDragSpeed() const noexcept {
        if (cfg.drag_speed > 0.f) return cfg.drag_speed;
        const float rs = (cfg.max > cfg.min) ? (cfg.max - cfg.min) / 200.f : 0.f;
        const float ss = cfg.step > 0.f ? cfg.step * 0.2f : 0.f;
        if (rs > 0.f && ss > 0.f) return std::min(rs, ss);
        return rs > 0.f ? rs : (ss > 0.f ? ss : 0.01f);
    }
};

struct NumberDragger : WidgetView<NumberDraggerState> {

    /**
     * @brief Returns value
     *
     * @return Computed floating-point value
     */
    float getValue() const noexcept;

    /**
     * @brief Sets value
     *
     * @param v  32-bit floating-point scalar
     */
    void setValue(float v);


    /**
     * @brief Checks whether dragging
     *
     * @return true on success, false on failure
     */
    bool isDragging() const noexcept;

    /**
     * @brief Checks whether editing
     *
     * @return true on success, false on failure
     */
    bool isEditing()  const noexcept;

    /**
     * @brief Returns state
     *
     * @return Pointer to the result, or nullptr on failure
     */
    NumberDraggerState* getState() noexcept {
        return WidgetView<NumberDraggerState>::getState();
    }
    /**
     * @brief Returns state
     *
     * @return Pointer to the result, or nullptr on failure
     */
    const NumberDraggerState* getState() const noexcept {
        return WidgetView<NumberDraggerState>::getState();
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

/**
 * @brief Constructs and returns number dragger
 *
 * @param tree  Red channel component [0, 1]
 * @param cfg   Configuration options struct
 *
 * @return NumberDragger result
 */
[[nodiscard]] NumberDragger makeNumberDragger(RenderTree& tree, NumberDraggerConfig cfg);

// Upgrade every  input[type=dragnum]  node in the subtree into a live
// NumberDragger. Call after bindDrawCallbacks() + bindNodeEvents().
// Reads: min max value step width height fontSize dragSpeed
/**
 * @brief Binds drag num widgets
 *
 * @param tree  Red channel component [0, 1]
 * @param root  Red channel component [0, 1]
 */
void bindDragNumWidgets(RenderTree& tree, NodeHandle root);

} // namespace pce::sdlos::widgets
