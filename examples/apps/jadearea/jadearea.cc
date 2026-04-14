// ============================================================================
// jadearea.cc — Jade Live Editor behaviour
// ============================================================================
//
// This file is #include-d directly into jade_host.cc via:
//   -DSDLOS_APP_BEHAVIOR="<abs-path>/jadearea.cc"
// so every declaration in jade_host.cc is visible here:
//   sdlos_log(), SDLOS_WIN_W / SDLOS_WIN_H, etc.
//
// Behaviour overview
// ──────────────────
//  - A multi-line TextArea widget is injected into the #editor-wrap node.
//  - On every keystroke the jade source is re-parsed and the result replaces
//    the children of #preview-wrap (live preview).
//  - "▶ Render" button → bus event "jadearea:render"  — explicit re-render.
//  - "Clear"    button → bus event "jadearea:clear"   — wipe editor + preview.
//  - out_handler is set so the host event loop can forward raw
//    SDL_Events (mouse / keyboard / text) to the TextArea widget.
// ============================================================================

#include "jade/jade_parser.h"       // jade::parse()
#include "style_draw.h"             // bindDrawCallbacks()
#include "widgets/input_text_box.h" // TextArea / makeTextArea

#include <memory>
#include <string>
#include <string_view>

// ============================================================================
// Anonymous helpers
// ============================================================================

namespace {

// ─── Starter source ──────────────────────────────────────────────────────────
// Shown in the editor on launch so there is always something visible in the
// preview pane from the very first frame.

static constexpr const char kDefaultJade[] =
R"jade(col(gap="16" padding="20" backgroundColor="#0f172a")
  div(color="#e2e8f0" fontSize="22") Hello, Jade!
  div(backgroundColor="#1e293b" height="1")
  row(gap="10")
    div(backgroundColor="#6366f1" color="#ffffff" height="36" width="90" borderRadius="8" fontSize="13") Button
    div(backgroundColor="#1e293b" color="#94a3b8" height="36" width="90" borderRadius="8" fontSize="13") Cancel
  div(color="#64748b" fontSize="13") Live jade → preview editor.
  div(color="#475569" fontSize="12") Try: col, row, div(color=... fontSize=...)
  div(backgroundColor="#1e293b" height="1")
  row(gap="8")
    div(backgroundColor="#0f766e" color="#ccfbf1" height="28" width="70" borderRadius="6" fontSize="12") green
    div(backgroundColor="#7c3aed" color="#ede9fe" height="28" width="70" borderRadius="6" fontSize="12") purple
    div(backgroundColor="#be185d" color="#fce7f3" height="28" width="70" borderRadius="6" fontSize="12") pink)jade";


// clearChildren
// Frees every child of `parent` without freeing `parent` itself.
// Uses tree.free() which internally calls detach() then recursively frees
// the entire subtree — generation-bumps every freed handle so stale
// NodeHandle copies become safely invalid on the next tree.node() lookup.

static void clearChildren(pce::sdlos::RenderTree& tree,
                           pce::sdlos::NodeHandle  parent)
{
    for (;;) {
        const pce::sdlos::RenderNode* pn = tree.node(parent);
        if (!pn || !pn->child.valid()) break;
        tree.free(pn->child);
    }
}



static void save(std::string_view jade_text)
{
    using std::filesystem::path;

}

static std::string_view load()
{

}

// renderToPreview
// Parses `jade_text` into a new subtree, binds draw callbacks, and attaches it
// as the sole child of `preview_h`.  Any previous preview subtree is freed first.
// Also updates the status label if `status_h` is valid.

static void renderToPreview(pce::sdlos::RenderTree& tree,
                             pce::sdlos::NodeHandle  preview_h,
                             pce::sdlos::NodeHandle  status_h,
                             std::string_view        jade_text)
{
    // 1. Remove old preview content.
    clearChildren(tree, preview_h);

    // 2. Parse and attach new content.
    if (!jade_text.empty()) {
        const pce::sdlos::NodeHandle nr =
            pce::sdlos::jade::parse(jade_text, tree);

        if (nr.valid()) {
            // Give the parsed root flex-grow=1 so it fills the entire
            // preview-wrap.  The layout pass will then size it correctly.
            if (pce::sdlos::RenderNode* rn = tree.node(nr)) {
                rn->layout_props.flex_grow = 1.f;
                rn->dirty_render           = true;
            }
            pce::sdlos::bindDrawCallbacks(tree, nr);
            tree.appendChild(preview_h, nr);
        }
    }

    // 3. Force a full re-layout + re-render of the whole tree.
    tree.forceAllDirty(tree.root());

    // 4. Update the status chip in the toolbar.
    if (status_h.valid()) {
        if (pce::sdlos::RenderNode* sn = tree.node(status_h)) {
            sn->setStyle("text", jade_text.empty() ? "" : "● rendered");
            sn->dirty_render = true;
        }
    }
}

} // anonymous namespace



void jade_app_init(pce::sdlos::RenderTree&               tree,
                   pce::sdlos::NodeHandle                 root,
                   pce::sdlos::IEventBus&                 bus,
                   pce::sdlos::SDLRenderer&               renderer,
                   std::function<bool(const SDL_Event&)>& out_handler)
{
    using namespace pce::sdlos::widgets;

    // 1. Locate jade-declared host nodes
    const pce::sdlos::NodeHandle editor_wrap_h =
        tree.findById(root, "editor-wrap");
    const pce::sdlos::NodeHandle preview_h =
        tree.findById(root, "preview-wrap");
    const pce::sdlos::NodeHandle status_h =
        tree.findById(root, "preview-status");

    if (!editor_wrap_h.valid() || !preview_h.valid()) {
        sdlos_log("[jadearea] ERROR: #editor-wrap or #preview-wrap not found — "
                  "check jadearea.jade");
        return;
    }

    sdlos_log("[jadearea] nodes ok — editor-wrap + preview-wrap found");

    // 2. Compute TextArea pixel dimensions from the actual window size at init.
    // SDL_GetWindowSize() returns the live logical-pixel dimensions, so the
    // editor fills the real window even when it differs from the CMake defaults.
    //
    // Layout breakdown (tallies with jadearea.jade):
    //   toolbar      : 44 px
    //   pane header  : 28 px
    //   content area : win_h − 44 − 28 = win_h − 72  px
    //   half width   : win_w / 2 − 1   (1 px centre divider)

    int win_w = SDLOS_WIN_W, win_h = SDLOS_WIN_H;   // compile-time fallback
    if (SDL_Window *sdl_win = renderer.GetWindow())
        SDL_GetWindowSize(sdl_win, &win_w, &win_h);

    const float kPaneW = static_cast<float>(win_w) * 0.5f - 1.f;
    const float kPaneH = static_cast<float>(win_h) - 44.f - 28.f;

    // 3. Build the TextArea config
    TextAreaConfig edcfg;
    edcfg.rows        = 0;          // explicit w / h below
    edcfg.cols        = 0;
    edcfg.w           = kPaneW;
    edcfg.h           = kPaneH;
    edcfg.line_height = 18.f;
    edcfg.font_size   = 13.f;
    edcfg.scrollbar_w = 6.f;
    edcfg.padding     = Edges::all(8.f);

    // Dark GitHub-style palette
    edcfg.bg                = Color::hex(0x0d, 0x11, 0x17);   // #0d1117
    edcfg.bg_focused        = Color::hex(0x0d, 0x11, 0x17);
    edcfg.border            = Color::hex(0x30, 0x36, 0x3d);   // #30363d
    edcfg.border_focus      = Color::hex(0x58, 0xa6, 0xff);   // #58a6ff accent
    edcfg.text_color        = Color::hex(0xc9, 0xd1, 0xd9);   // #c9d1d9
    edcfg.placeholder_color = Color::hex(0x48, 0x4f, 0x58);   // #484f58
    edcfg.cursor_color      = Color::hex(0x58, 0xa6, 0xff);

    // TODO debounce
    // Live preview: re-parse and re-render on every keystroke.
    // The lambda captures by reference — safe because jade_host clears
    // out_handler (and all bus subscriptions) before the tree
    // is destroyed on the next scene load.
    edcfg.on_change =
        [&tree, preview_h, status_h](std::string_view text) {
            renderToPreview(tree, preview_h, status_h, text);
        };

    // Create widget, attach, and focus
    auto editor = makeTextArea(tree, std::move(edcfg));
    tree.appendChild(editor_wrap_h, editor);
    editor.focus();     // starts OS IME; cursor visible from first frame

    // Seed editor with starter source
    // setText() fires on_change → renderToPreview() automatically, so no
    // explicit renderToPreview() call is needed here.
    editor.setText(kDefaultJade);


    // Bus subscriptions

    bus.subscribe("jadearea:load",
    [editor, &tree, preview_h, status_h](const std::string&) mutable {
    	// TODO load by filenames of jade/?.jade and set text to Editor

    });

    bus.subscribe("jadearea:list",
    [editor, &tree, preview_h, status_h](const std::string&) mutable {
    	// TODO load all filenames of jade/?.jade and return as list to host for UI (e.g. dropdown in toolbar)

    });

    bus.subscribe("jadearea:save",
    [editor, &tree, preview_h, status_h](const std::string&) mutable {
        // renderToPreview(tree, preview_h, status_h, editor.getText());
        // todo save data/jade/
    });

    // "▶ Render" button — explicit re-render (useful after pasting large text
    // when live-preview might have produced a partial result).
    bus.subscribe("jadearea:render",
        [editor, &tree, preview_h, status_h](const std::string&) mutable {
            renderToPreview(tree, preview_h, status_h, editor.getText());
        });

    // "Clear" button — wipe editor content and clear the preview pane.
    bus.subscribe("jadearea:clear",
        [editor, &tree, preview_h, status_h](const std::string&) mutable {
            editor.clear();
            clearChildren(tree, preview_h);
            tree.forceAllDirty(tree.root());
            if (status_h.valid()) {
                if (pce::sdlos::RenderNode* sn = tree.node(status_h)) {
                    sn->setStyle("text", "");
                    sn->dirty_render = true;
                }
            }
        });

    // Raw-event hook
    // out_handler is the SceneState-owned slot passed in by jade_host.cc.
    // Assigning it here routes raw SDL events to the TextArea for the lifetime
    // of this scene; it is cleared automatically on the next loadScene() call.
    //
    // Routing rules:
    //   MOUSE_BUTTON_DOWN / UP  → scale to physical pixels, forward to TextArea,
    //                             return FALSE so dispatchClick still runs.
    //   MOUSE_MOTION            → scale, forward (scrollbar drag); consume when
    //                             a drag is active.
    //   MOUSE_WHEEL             → scale mouse position, forward; consume.
    //   TEXT_INPUT / KEY_DOWN   → forward as-is; consume when TextArea is
    //                             focused (prevents host ` / F1 shortcuts while
    //                             typing, which is the expected behaviour for an
    //                             editor).
    //
    // editor is captured by value — TextArea is a lightweight WidgetView that
    // carries RenderTree& (a reference) and NodeHandle (a trivial value), both
    // of which remain valid until loadScene() resets this handler.

    out_handler =
        [editor, &renderer](const SDL_Event& ev) mutable -> bool
    {
        const float sx = renderer.pixelScaleX();
        const float sy = renderer.pixelScaleY();

        switch (ev.type) {

        // Focus / unfocus the TextArea on click; do NOT consume so that
        // jade_host's dispatchClick can also fire onclick events on buttons.
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP: {
            SDL_Event s = ev;
            s.button.x  = ev.button.x * sx;
            s.button.y  = ev.button.y * sy;
            editor.handleEvent(s);
            return false;
        }

        // Scrollbar drag — consume when dragging to suppress other handlers.
        case SDL_EVENT_MOUSE_MOTION: {
            SDL_Event s = ev;
            s.motion.x  = ev.motion.x * sx;
            s.motion.y  = ev.motion.y * sy;
            return editor.handleEvent(s);
        }

        // Scroll wheel — forward only when the TextArea is focused.
        case SDL_EVENT_MOUSE_WHEEL: {
            SDL_Event s     = ev;
            s.wheel.mouse_x = ev.wheel.mouse_x * sx;
            s.wheel.mouse_y = ev.wheel.mouse_y * sy;
            return editor.handleEvent(s);
        }

        // Text / key events — consumed only when the editor is focused.
        case SDL_EVENT_TEXT_INPUT:
        case SDL_EVENT_KEY_DOWN:
            return editor.handleEvent(ev);

        default:
            return false;
        }
    };
}
