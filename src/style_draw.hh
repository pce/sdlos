#pragma once

// Walks a parsed RenderTree subtree and installs draw + update callbacks on
// every node that carries visual style attributes (backgroundColor, color,
// fontSize, text).
//
// Call this once after jade::parse() and before the render loop:
//
//   NodeHandle root = jade::parse(source, tree);
//   bindDrawCallbacks(tree, root);
//
// Responsibilities
// ────────────────
// draw()   — reads backgroundColor → drawRect, text+color+fontSize → drawText
//            (text is approximately centered in the node bounds).
//            Coordinates are converted from layout-relative to absolute screen
//            space by walking the parent chain.
//
// update() — marks dirty_render = true every frame.  Required because
//            SDL_GPU_LOADOP_CLEAR wipes the framebuffer on every render pass;
//            nodes that don't re-emit draw commands each frame disappear.
//            Composed on top of any existing update() (e.g. from bindNodeEvents).

#include "render_tree.hh"

namespace pce::sdlos {

void bindDrawCallbacks(RenderTree& tree, NodeHandle root);

} // namespace pce::sdlos
