#pragma once

#include "widget.hh"
#include "../render_tree.hh"

#include <SDL3/SDL.h>

#include <functional>
#include <string>

namespace pce::sdlos::widgets {

struct InputBoxConfig {
    std::string placeholder = "";

    // Non-owning. Signal must outlive the node.
    Signal<std::string>* value = nullptr;

    float x = 0.f;
    float y = 0.f;
    float w = 320.f;
    float h = 44.f;

    Color bg         = Color::hex(0x1c, 0x1c, 0x1e, 0xcc);
    Color bg_focused = Color::hex(0x2c, 0x2c, 0x2e, 0xee);

    Color border       = Color::hex(0x48, 0x48, 0x4a, 0xff);
    Color border_focus = Color::systemBlue();

    Color placeholder_color = Color::secondaryLabel();
    Color text_color        = Color::label();
    Color cursor_color      = Color::systemBlue();

    float font_size = 17.f;
    Edges padding   = Edges::horizontal(12.f);  // vertical is auto-centred

    std::size_t max_length = 0;   // 0 = unlimited
    bool secure   = false;        // glyphs replaced with '●' in draw; Signal gets real text
    bool disabled = false;

    std::function<void(const std::string&)> on_submit;
    std::function<void(const std::string&)> on_change;
};

// Stored as std::any inside the node; accessed via std::any_cast<InputBoxState>.
struct InputBoxState {
    std::string text;
    bool        focused        = false;
    bool        cursor_visible = true;
    std::size_t cursor_pos     = 0;   // byte offset into text

    InputBoxConfig cfg;
};

[[nodiscard]]
Widget inputBox(RenderTree& tree, InputBoxConfig cfg);

// Route a raw SDL_Event to the InputBox identified by handle.
// Handled: MOUSE_BUTTON_DOWN (focus), KEY_DOWN (edit/nav/submit), TEXT_INPUT.
void inputBoxHandleEvent(RenderTree& tree, NodeHandle handle,
                         const SDL_Event& event);

void inputBoxFocus  (RenderTree& tree, NodeHandle handle);
void inputBoxUnfocus(RenderTree& tree, NodeHandle handle);

[[nodiscard]]
std::string inputBoxGetText(const RenderTree& tree, NodeHandle handle);

void inputBoxSetText(RenderTree& tree, NodeHandle handle, std::string text);

} // namespace pce::sdlos::widgets
