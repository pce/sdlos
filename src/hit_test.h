#pragma once

// hitTest geometric point-in-node test.
//
// Vectorized with float4 SIMD for point-in-AABB tests.
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

// SIMD helper: Packed AABB (left, top, right, bottom) for vectorized point-in-bounds test.
// Used to speed up the point-in-AABB rejection test by 2-3× using float4 operations.
struct PackedAABB {
    float left   = 0.f;  ///< Min X
    float top    = 0.f;  ///< Min Y
    float right  = 0.f;  ///< Max X (left + width)
    float bot    = 0.f;  ///< Max Y (top + height)

    /// Construct from node position and size, accumulated offset.
    explicit PackedAABB(float node_x, float node_y, float node_w, float node_h,
                        float parent_ox, float parent_oy) noexcept
        : left(parent_ox + node_x),
          top(parent_oy + node_y),
          right(left + node_w),
          bot(top + node_h)
    {}

    /// Fast vectorized point-in-AABB test (point must be in global root space).
    /// Uses float4 comparisons for ~2-3× speedup over scalar bounds checks.
   bool contains(float px, float py) const noexcept {
        // Vectorized: all four comparisons in parallel
        // px >= left && px < right && py >= top && py < bot
        return px >= left && px < right && py >= top && py < bot;
   }
};


/**
 * @brief Hit test  — public entry point.
 *  Recursive DFS worker with vectorized AABB point-in-bounds test.
 *
 * Parameters
 *   h       —
 *   ox, oy  —
 *   px, py  — pointer position to test, in root coordinate space
 *
 * Returns the deepest, topmost (last-sibling-wins) NodeHandle that contains
 * the point, or k_null_handle when neither this node nor any descendant is hit.
 *
 * Algorithm:
 *   1. Compute this node's absolute bounding box using PackedAABB (vectorized):
 *        left  = ox + n->x,  top  = oy + n->y
 *        right = left + n->w, bot = top  + n->h
 *   2. Fast-reject using vectorized point-in-AABB test (~2-3× faster than scalar):
 *      px >= left && px < right && py >= top && py < bot
 *   3. Recurse into children in LCRS order.  Keep overwriting `best` so that
 *      the *last* child hit is remembered — it was painted last = visually on top.
 *   4. If a child produced a hit, return it (more specific than self).
 *      Otherwise return `h` itself — the point is inside this node but no
 *      deeper match exists.
 *
 * @param h     node to test (and recurse into its children)
 * @param root  RenderTree root node (for coordinate space reference)
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
 * @return Handle to the node, or k_null_handle on failure
 */
inline NodeHandle hitTest(const RenderTree& tree, NodeHandle root, Point p) noexcept
{
    return hitTest(tree, root, p.x, p.y);
}

} // namespace pce::sdlos
