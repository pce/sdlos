// test_render_tree.cc — Unit tests for RenderTree dirty-flag and layout logic.
//
// Tests:
//   1. markLayoutDirty propagation  — upward walk stops at tree root.
//   2. markLayoutDirty early-exit   — break when ancestor already dirty.
//   3. flexLayout stable cascade    — subtree NOT re-dirtied when child geometry
//                                     is unchanged after a parent re-layout.
//
// ── Pointer safety note ──────────────────────────────────────────────────────
// slot_map uses std::vector<Slot> internally.  push_back() (called by alloc())
// may reallocate, invalidating any previously stored RenderNode* pointer.
// Rule: obtain ALL NodeHandles before caching any RenderNode* pointer.

#include <catch2/catch_test_macros.hpp>
#include "render_tree.h"

using namespace pce::sdlos;

// ─────────────────────────────────────────────────────────────────────────────
// Test 1 — markLayoutDirty propagation
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("RenderTree: markLayoutDirty propagation", "[render_tree]")
{
    RenderTree tree(1024);

    // Allocate all nodes before caching any pointers.
    const NodeHandle root       = tree.alloc();
    const NodeHandle child      = tree.alloc();
    const NodeHandle grandchild = tree.alloc();

    tree.appendChild(root, child);
    tree.appendChild(child, grandchild);

    // RenderNode defaults dirty_layout = true on construction.
    REQUIRE(tree.node(root)->dirty_layout       == true);
    REQUIRE(tree.node(child)->dirty_layout      == true);
    REQUIRE(tree.node(grandchild)->dirty_layout == true);

    // Initial settle: update() clears dirty_layout on every visited node.
    tree.update(root);
    REQUIRE(tree.node(root)->dirty_layout       == false);
    REQUIRE(tree.node(child)->dirty_layout      == false);
    REQUIRE(tree.node(grandchild)->dirty_layout == false);

    // Mark grandchild dirty — propagates upward to child and root.
    tree.markLayoutDirty(grandchild);

    REQUIRE(tree.node(grandchild)->dirty_layout == true);
    REQUIRE(tree.node(child)->dirty_layout      == true);
    REQUIRE(tree.node(root)->dirty_layout       == true);

    // Reset flags manually for the next sub-test.
    tree.node(root)->dirty_layout       = false;
    tree.node(child)->dirty_layout      = false;
    tree.node(grandchild)->dirty_layout = false;

    // Mark child dirty — propagates to root but NOT downward to grandchild.
    // markLayoutDirty walks the parent chain only.
    tree.markLayoutDirty(child);

    REQUIRE(tree.node(grandchild)->dirty_layout == false);  // untouched
    REQUIRE(tree.node(child)->dirty_layout      == true);
    REQUIRE(tree.node(root)->dirty_layout       == true);
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 2 — markLayoutDirty early-exit optimization
// ─────────────────────────────────────────────────────────────────────────────
//
// render_tree.cc: `if (n->dirty_layout) break;`
// Once a node in the parent chain is already dirty, propagation stops.
// Re-marking an already-dirty child must NOT re-dirty an ancestor that was
// manually cleared, because no new information is added.

TEST_CASE("RenderTree: markLayoutDirty early-exit when already dirty", "[render_tree]")
{
    RenderTree tree(1024);

    const NodeHandle root  = tree.alloc();
    const NodeHandle child = tree.alloc();
    tree.appendChild(root, child);

    // Settle.
    tree.update(root);

    // Mark child dirty — root picks it up.
    tree.markLayoutDirty(child);
    REQUIRE(tree.node(child)->dirty_layout == true);
    REQUIRE(tree.node(root)->dirty_layout  == true);

    // Manually clear root's flag (simulate external reset).
    tree.node(root)->dirty_layout = false;

    // Mark child again.  child is ALREADY dirty → break before reaching root.
    tree.markLayoutDirty(child);
    REQUIRE(tree.node(child)->dirty_layout == true);   // unchanged
    REQUIRE(tree.node(root)->dirty_layout  == false);  // NOT re-dirtied
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 3 — flexLayout geometry-stable cascade optimization
// ─────────────────────────────────────────────────────────────────────────────
//
// render_tree.cc (Pass 3 of flexLayout):
//   if (child->x != old_x || child->y != old_y ||
//       child->w != old_w || child->h != old_h)
//       child->dirty_layout = true;
//
// When a FlexColumn parent is re-laid out but a child's geometry is unchanged,
// the grandchild subtree must NOT be marked dirty.
//
// Tree: root (FlexColumn, 100×100) → child (lp.height=20) → grandchild

TEST_CASE("RenderTree: flexLayout does not cascade when child geometry is stable",
          "[render_tree]")
{
    RenderTree tree(1u << 20);

    // ── Alloc all nodes FIRST, then configure (pointer safety). ──────────────
    const NodeHandle root       = tree.alloc();
    const NodeHandle child      = tree.alloc();
    const NodeHandle grandchild = tree.alloc();

    tree.appendChild(root, child);
    tree.appendChild(child, grandchild);

    // Root: 100×100 FlexColumn, Justify::Start (default).
    {
        RenderNode* rn  = tree.node(root);
        rn->w           = 100.f;
        rn->h           = 100.f;
        rn->layout_kind = LayoutKind::FlexColumn;
    }

    // Child: fixed height via layout_props.height.
    // layout_props.width = 50 (>= 0) → Align::Stretch will not auto-expand
    // the cross-axis width, so child->w stays at 0 (default) after layout.
    {
        RenderNode* cn          = tree.node(child);
        cn->layout_props.height = 20.f;
        cn->layout_props.width  = 50.f;
    }

    // ── Initial settle ───────────────────────────────────────────────────────
    // root.dirty_layout is already true (default), markLayoutDirty is a no-op.
    tree.markLayoutDirty(root);
    tree.update(root);

    REQUIRE(tree.node(root)->dirty_layout       == false);
    REQUIRE(tree.node(child)->dirty_layout      == false);
    REQUIRE(tree.node(grandchild)->dirty_layout == false);
    REQUIRE(tree.node(child)->y                 == 0.f);

    // ── Case A: grow the parent — child stays at y=0, cascade blocked ────────
    // After re-layout: child->y=0, child->h=20 — identical to before.
    // Pass 3 detects no geometry change → grandchild NOT re-dirtied.
    tree.node(root)->h = 200.f;
    tree.markLayoutDirty(root);
    tree.update(root);

    REQUIRE(tree.node(grandchild)->dirty_layout == false);  // cascade blocked ✓

    // ── Case B: change justify → child moves → geometry changed ──────────────
    // Verify child->y is still 0 before this update fires.
    REQUIRE(tree.node(child)->y == 0.f);

    // Justify::Center with h=200, child h=20:
    //   free_space  = 200 - 20 = 180
    //   start_offset = 180 × 0.5 = 90
    //   child->y    = 90
    tree.node(root)->layout_props.justify = LayoutProps::Justify::Center;
    tree.markLayoutDirty(root);
    tree.update(root);

    REQUIRE(tree.node(child)->y == 90.f);
}
