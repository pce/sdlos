#pragma once

// hitTest geometric point-in-node test.
//
// Non-obvious rules (§ Hit Test for rationale):
//   - Last sibling wins — matches paint order (later = on top).
//   - Root own bounds are NOT checked — works for both virtual parse roots
//     (w=0,h=0) and full-viewport scene roots without special-casing.
//   - Zero-area node (w≤0 || h≤0) → node + entire subtree skipped.
//   - x/y are parent-relative; hitTest accumulates absolute offsets while descending.

#include "render_tree.h"

namespace pce::sdlos {

struct Point { float x = 0.f; float y = 0.f; };


/**
 * @brief Hit test
 *
 * @param tree  Red channel component [0, 1]
 * @param root  Red channel component [0, 1]

 * @param px    Horizontal coordinate in logical pixels
 * @param py    Vertical coordinate in logical pixels
 *
 * @return Handle to the node, or k_null_handle on failure
 */
NodeHandle hitTest(const RenderTree& tree, NodeHandle root,
                   float px, float py) noexcept;



/**
 * @brief Hit test
 *
 * @param tree  Red channel component [0, 1]
 * @param root  Red channel component [0, 1]
 * @param p     Signed 32-bit integer
 *
 * @return Handle to the node, or k_null_handle on failure
 */
inline NodeHandle hitTest(const RenderTree& tree, NodeHandle root, Point p) noexcept
{
    return hitTest(tree, root, p.x, p.y);
}

} // namespace pce::sdlos
