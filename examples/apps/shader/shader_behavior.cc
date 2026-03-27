// =============================================================================
// shader_behavior.cc  —  [minimal] behaviour for the shader app
// =============================================================================
//
// Architecture overview
// ---------------------
//   This file is #include-d directly into jade_host.cc at compile time.
//   All declarations in jade_host.cc are visible here without any includes.
//   The entry point is jade_app_init() — called once after the scene is fully
//   parsed, styled, and layout-bound.
//
// Lifecycle
// ---------
//   1. jade_host.cc: SDL_Init, create window, SDLRenderer::Initialize
//   2. jade_host.cc: load + parse shader.jade  →  RenderTree
//   3. jade_host.cc: apply shader.css           →  StyleSheet
//   4. jade_host.cc: resolve asset paths (src=, _font=, …)
//   5. jade_host.cc: bindDrawCallbacks + bindNodeEvents
//   6. jade_host.cc: ► jade_app_init() ◄  ← you are here
//   7. jade_host.cc: event loop → render loop
//
// EventBus topics (published by this behavior)
// --------------------------------------------
//   None defined by default.  Subscribe with:
//     bus.subscribe("shader:<topic>", [&tree, state](const std::string& v) { … });
//   Fire events from jade with  onclick="shader:<topic>"  on any node.
//
// Customisation guide
// -------------------
//   1. Locate scene nodes with tree.findById(root, "my-id") and cache their
//      handles in ShaderState.
//   2. Subscribe to bus topics inside the "enter the forrest" user region.
//   3. Use tree.node(h)->setStyle(key, val) + dirty_render = true to push
//      reactive updates from callbacks.
//   4. Uncomment and complete out_handler if any jade widget needs raw
//      SDL events (TextField, NumberDragger, …).
//
// Regeneration
// ------------
//   sdlos create shader --overwrite
//   Code between "enter the forrest" / "back to the sea" markers is preserved.
//
// Performance notes
// -----------------
//   • Bus callbacks run on the render thread — keep them O(1).
//   • tree.node(h)->dirty_render = true marks only the subtree that changed;
//     jade_host will forceAllDirty only if at least one node is dirty.
//   • Never call SDL_PollEvent(), acquire a GPU command buffer, or block on
//     I/O from inside jade_app_init() or any bus callback.
// =============================================================================

#include <memory>
#include <string>

// ── State ─────────────────────────────────────────────────────────────────────

namespace {

struct ShaderState {
    // Add per-scene state here.
    // Capture a std::shared_ptr<ShaderState> in lambdas so bus
    // callbacks hold a safe reference even after jade_app_init returns.

    // --- enter the forrest ---

    // --- back to the sea ---
};

} // namespace

// ── jade_app_init ─────────────────────────────────────────────────────────────

void jade_app_init(pce::sdlos::RenderTree&               tree,
                   pce::sdlos::NodeHandle                 root,
                   pce::sdlos::IEventBus&                 bus,
                   pce::sdlos::SDLRenderer&               renderer,
                   std::function<bool(const SDL_Event&)>& out_handler)
{
    auto state = std::make_shared<ShaderState>();

    // ── Locate nodes ──────────────────────────────────────────────────────────
    // const pce::sdlos::NodeHandle title_h = tree.findById(root, "my-title");

    // ── Bus subscriptions ─────────────────────────────────────────────────────
    // --- enter the forrest ---

    // bus.subscribe("shader:action", [&tree, state](const std::string& data) {
    //     sdlos_log("[shader] action: " + data);
    // });

    // --- back to the sea ---

    // ── Raw SDL event hook (optional) ─────────────────────────────────────────
    // Assign out_handler only when you need to forward raw SDL events to
    // widgets (TextField, NumberDragger, …).  Leave unset otherwise.
    //
    // out_handler = [&renderer](const SDL_Event& ev) mutable -> bool {
    //     const float sx = renderer.pixelScaleX();
    //     const float sy = renderer.pixelScaleY();
    //     // Scale logical → physical coords, forward to widgets.
    //     // Return true to consume (suppresses host keyboard shortcuts).
    //     return false;
    // };
}
