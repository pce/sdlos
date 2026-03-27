#include "hit_test.h"

namespace pce::sdlos {

namespace {

// hitTestNode
// Recursive DFS worker.
//
// Parameters
//   h       — node to test (and recurse into its children)
//   ox, oy  — absolute pixel offset of h's *parent* in the root coordinate space
//   px, py  — pointer position to test, in root coordinate space
//
// Returns the deepest, topmost (last-sibling-wins) NodeHandle that contains
// the point, or k_null_handle when neither this node nor any descendant is hit.
//
// Algorithm:
//   1. Compute this node's absolute bounding box:
//        left  = ox + n->x,  top  = oy + n->y
//        right = left + n->w, bot = top  + n->h
//   2. Fast-reject: zero-area nodes, or point outside bounding box —
//      skip the entire subtree (children can't extend past a collapsed parent).
//      Exception: LayoutKind::None zero-area *containers* (e.g. scene3d nodes)
//      are transparent pass-throughs — we recurse into their children, which
//      may carry projected screen-space AABBs written by GltfScene::tick().
//   3. Recurse into children in LCRS order.  Keep overwriting `best` so that
//      the *last* child hit is remembered — it was painted last = visually on top.
//   4. If a child produced a hit, return it (more specific than self).
//      Otherwise return `h` itself — the point is inside this node but no
//      deeper match exists.

static NodeHandle hitTestNode(const RenderTree& tree,
                               NodeHandle h,
                               float ox, float oy,
                               float px, float py) noexcept
{
    const RenderNode* n = tree.node(h);
    if (!n) return k_null_handle;

    const bool has_area = (n->w > 0.f && n->h > 0.f);

    // Zero-area layout-participating node: invisible — skip self AND subtree.
    // Children of a collapsed Block/Flex parent cannot escape its clip rect.
    if (!has_area && n->layout_kind != LayoutKind::None)
        return k_null_handle;

    const float left  = ox + n->x;
    const float top   = oy + n->y;
    const float right = left + n->w;
    const float bot   = top  + n->h;

    // For nodes with area: fast-reject when pointer is outside bounding box.
    // Zero-area LayoutKind::None containers (e.g. scene3d) are pass-throughs —
    // skip the bounds check so their proxy children remain reachable.
    if (has_area && (px < left || px >= right || py < top || py >= bot))
        return k_null_handle;

    // Recurse into children — last sibling wins.
    NodeHandle best = k_null_handle;

    for (NodeHandle c = n->child; c.valid(); ) {
        const RenderNode* cn = tree.node(c);
        if (!cn) break;

        const NodeHandle r = hitTestNode(tree, c, left, top, px, py);
        if (r.valid()) best = r;   // overwrite — last child wins

        c = cn->sibling;
    }

    // A child hit is more specific than self; fall back to self (only when
    // this node has area — zero-area containers never absorb the hit).
    return best.valid() ? best : (has_area ? h : k_null_handle);
}

} // anonymous namespace


// Public entry point.
//
// The root node's own bounds are intentionally NOT checked.  This makes two
// common patterns work identically without special-casing:
//
//   - JadeLite virtual _root  (w=0, h=0 before layout has run)
//   - Desktop scene_root_     (w/h = swapchain physical pixels)
//
// Direct children of root are tested starting from root's own x/y offset,
// which is 0,0 for scene roots and also for freshly-parsed virtual roots.
// The last child that produces a hit wins (last = painted last = on top).

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
                   float px, float py) noexcept
{
    const RenderNode* r = tree.node(root);
    if (!r) return k_null_handle;

    // Use root's own position as the accumulated offset for its children.
    // For full-viewport scene roots (x=0, y=0) this is free.
    // For translated sub-roots this correctly maps into the right coordinate space.
    const float ox = r->x;
    const float oy = r->y;

    NodeHandle result = k_null_handle;

    for (NodeHandle c = r->child; c.valid(); ) {
        const RenderNode* cn = tree.node(c);
        if (!cn) break;

        const NodeHandle hit = hitTestNode(tree, c, ox, oy, px, py);
        if (hit.valid()) result = hit;  // last child wins

        c = cn->sibling;
    }

    return result;
}

} // namespace pce::sdlos
