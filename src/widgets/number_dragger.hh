#pragma once

#include "widget.hh"
#include "../render_tree.hh"
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

    [[nodiscard]] float effectiveDragSpeed() const noexcept {
        if (cfg.drag_speed > 0.f) return cfg.drag_speed;
        const float rs = (cfg.max > cfg.min) ? (cfg.max - cfg.min) / 200.f : 0.f;
        const float ss = cfg.step > 0.f ? cfg.step * 0.2f : 0.f;
        if (rs > 0.f && ss > 0.f) return std::min(rs, ss);
        return rs > 0.f ? rs : (ss > 0.f ? ss : 0.01f);
    }
};

struct NumberDragger : WidgetView<NumberDraggerState> {
    [[nodiscard]] float getValue() const noexcept;
    void setValue(float v);

    [[nodiscard]] bool isDragging() const noexcept;
    [[nodiscard]] bool isEditing()  const noexcept;

    [[nodiscard]] NumberDraggerState* getState() noexcept {
        return WidgetView<NumberDraggerState>::getState();
    }
    [[nodiscard]] const NumberDraggerState* getState() const noexcept {
        return WidgetView<NumberDraggerState>::getState();
    }

    bool handleEvent(const SDL_Event& ev);
};

[[nodiscard]] NumberDragger makeNumberDragger(RenderTree& tree, NumberDraggerConfig cfg);

// Upgrade every  input[type=dragnum]  node in the subtree into a live
// NumberDragger. Call after bindDrawCallbacks() + bindNodeEvents().
// Reads: min max value step width height fontSize dragSpeed
void bindDragNumWidgets(RenderTree& tree, NodeHandle root);

} // namespace pce::sdlos::widgets
