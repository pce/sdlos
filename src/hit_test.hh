#pragma once

// hitTest — parser-agnostic geometric point-in-node test.
// No SDL, no EventBus, no parser dependency.
//
// Non-obvious rules (see architecture.md § Hit Test for rationale):
//   • Last sibling wins — matches paint order (later = on top).
//   • Root own bounds are NOT checked — works for both virtual parse roots
//     (w=0,h=0) and full-viewport scene roots without special-casing.
//   • Zero-area node (w≤0 || h≤0) → node + entire subtree skipped.
//   • x/y are parent-relative; hitTest accumulates absolute offsets while descending.

#include "render_tree.hh"

namespace pce::sdlos {

struct Point { float x = 0.f; float y = 0.f; };

[[nodiscard]]
NodeHandle hitTest(const RenderTree& tree, NodeHandle root,
                   float px, float py) noexcept;

[[nodiscard]]
inline NodeHandle hitTest(const RenderTree& tree, NodeHandle root, Point p) noexcept
{
    return hitTest(tree, root, p.x, p.y);
}

} // namespace pce::sdlos
