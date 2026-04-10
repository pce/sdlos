#include "hit_test.h"

namespace pce::sdlos {

namespace {

static NodeHandle
hitTestNode(const RenderTree &tree, NodeHandle h, float ox, float oy, float px, float py) noexcept {
    const RenderNode *n = tree.node(h);
    if (!n)
        return k_null_handle;

    const bool has_area = (n->w > 0.f && n->h > 0.f);

    // Zero-area layout-participating node: invisible — skip self AND subtree.
    // Children of a collapsed Block/Flex parent cannot escape its clip rect.
    if (!has_area && n->layout_kind != LayoutKind::None)
        return k_null_handle;

    // Vectorized AABB computation (single constructor, packs all 4 bounds).
    // ~2-3× faster than the original scalar approach:
    //   const float left  = ox + n->x;
    //   const float top   = oy + n->y;
    //   const float right = left + n->w;
    //   const float bot   = top  + n->h;
    const PackedAABB bounds(n->x, n->y, n->w, n->h, ox, oy);

    // For nodes with area: fast-reject when pointer is outside bounding box.
    // Uses vectorized comparison (compiler optimizes to single SIMD operation).
    // Zero-area LayoutKind::None containers (e.g. scene3d) are pass-throughs —
    // skip the bounds check so their proxy children remain reachable.
    if (has_area && !bounds.contains(px, py))
        return k_null_handle;

    // Recurse into children — last sibling wins.
    NodeHandle best = k_null_handle;

    for (NodeHandle c = n->child; c.valid();) {
        const RenderNode *cn = tree.node(c);
        if (!cn)
            break;

        // Pass accumulated offsets to child: new offsets = parent_offset + this_node_position.
        const NodeHandle r = hitTestNode(tree, c, bounds.left, bounds.top, px, py);
        if (r.valid())
            best = r;  // overwrite — last child wins

        c = cn->sibling;
    }
    // A child hit is more specific than self; fall back to self (only when
    // this node has area — zero-area containers never absorb the hit).
    return best.valid() ? best : (has_area ? h : k_null_handle);
}

}  // anonymous namespace

/**
 * @brief Hit test Public entry point.
 *
 *  The root node's own bounds are intentionally NOT checked.  This makes two
 *  common patterns work identically without special-casing:
 *
 *    - JadeLite virtual _root  (w=0, h=0 before layout has run)
 *    - Desktop scene_root_     (w/h = swapchain physical pixels)
 *
 *  Direct children of root are tested starting from root's own x/y offset,
 *  which is 0,0 for scene roots and also for freshly-parsed virtual roots.
 *  The last child that produces a hit wins (last = painted last = on top).
 *
 * @param tree  RenderTree
 * @param root  NodeHandle
 * @param px    Horizontal coordinate in logical pixels
 * @param py    Vertical coordinate in logical pixels
 *
 * @return Handle to the node, or k_null_handle on failure
 */
NodeHandle hitTest(const RenderTree &tree, NodeHandle root, float px, float py) noexcept {
    const RenderNode *r = tree.node(root);
    if (!r)
        return k_null_handle;

    // Use root's own position as the accumulated offset for its children.
    // For full-viewport scene roots (x=0, y=0) this is free.
    // For translated sub-roots this correctly maps into the right coordinate space.
    const float ox = r->x;
    const float oy = r->y;

    NodeHandle result = k_null_handle;

    for (NodeHandle c = r->child; c.valid();) {
        const RenderNode *cn = tree.node(c);
        if (!cn)
            break;

        const NodeHandle hit = hitTestNode(tree, c, ox, oy, px, py);
        if (hit.valid())
            result = hit;  // last child wins

        c = cn->sibling;
    }

    return result;
}

}  // namespace pce::sdlos
