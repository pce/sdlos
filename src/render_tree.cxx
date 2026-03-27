// render_tree.cxx — Scene graph implementation.
//
// Namespace : sdlos
// File      : render_tree.cxx  (lowercase snake_case)
//
// Responsibilities
// ================
//   loadShader()   — read compiled shader blob from disk;
//                    return std::expected<SDL_GPUShader*, std::string>
//   RenderContext  — pipeline lookup, drawRect / drawText geometry drawing
//   RenderTree     — slot_map-based node alloc/free, LCRS tree ops,
//                    dirty propagation, iterative preorder DFS,
//                    frame_arena reset, update / render lifecycle

#include "render_tree.hh"
#include "text_renderer.hh"

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <vector>

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

namespace pce::sdlos {

// ===========================================================================
// loadShader
// ===========================================================================

std::expected<SDL_GPUShader*, std::string>
loadShader(SDL_GPUDevice*     device,
           GPUBackend         backend,
           std::string_view   name,
           SDL_GPUShaderStage stage,
           uint32_t           num_samplers,
           uint32_t           num_uniform_buffers)
{
    namespace fs = std::filesystem;

    const std::string_view ext =
        (backend == GPUBackend::Metal) ? ".metallib" : ".spv";

    const std::string path =
        std::format("assets/shaders/{}{}", name, ext);

    if (!fs::exists(path)) {
        return std::unexpected(
            std::format("[loadShader] file not found: {}", path));
    }

    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return std::unexpected(
            std::format("[loadShader] cannot open: {}", path));
    }

    const auto file_size = static_cast<std::size_t>(file.tellg());
    file.seekg(0);

    std::vector<Uint8> code(file_size);
    if (!file.read(reinterpret_cast<char*>(code.data()),
                   static_cast<std::streamsize>(file_size))) {
        return std::unexpected(
            std::format("[loadShader] read error: {}", path));
    }

    const SDL_GPUShaderFormat fmt =
        (backend == GPUBackend::Metal)
            ? SDL_GPU_SHADERFORMAT_MSL
            : SDL_GPU_SHADERFORMAT_SPIRV;

    SDL_GPUShaderCreateInfo info{};
    info.code                = code.data();
    info.code_size           = code.size();
    info.entrypoint          = "main0";
    info.format              = fmt;
    info.stage               = stage;
    info.num_samplers        = num_samplers;
    info.num_uniform_buffers = num_uniform_buffers;
    info.props               = 0;

    SDL_GPUShader* shader = SDL_CreateGPUShader(device, &info);
    if (!shader) {
        return std::unexpected(
            std::format("[loadShader] SDL_CreateGPUShader failed for '{}': {}",
                        path, SDL_GetError()));
    }

    std::cout << std::format("[loadShader] loaded '{}'\n", path);
    return shader;
}

// ===========================================================================
// RenderContext
// ===========================================================================

// ---------------------------------------------------------------------------
// RenderContext::drawRect
//
// Draws a solid-colour rectangle using the "rect" pipeline registered in
// `pipelines`.  No vertex buffer is needed — the vertex shader (ui_rect.vert)
// generates a 6-vertex CCW quad entirely from push-uniform data.
//
// Push uniform layout (must match RectUniform in ui_rect.vert.metal):
//   slot 0, vertex stage  → {x, y, w, h, viewport_w, viewport_h, _pad, _pad}
//   slot 0, fragment stage → {r, g, b, a}
// ---------------------------------------------------------------------------

void RenderContext::drawRect(float x, float y, float w, float h,
                              float r, float g, float b, float a)
{
    if (!pass || !cmd) return;

    SDL_GPUGraphicsPipeline* pipe = pipeline("rect");
    if (!pipe) return;

    SDL_BindGPUGraphicsPipeline(pass, pipe);

    // Vertex-stage push uniform: rect bounds in pixel space + viewport size.
    // Must be 32 bytes to match `struct RectUniform` in the Metal shader.
    struct alignas(4) RectUniform {
        float x, y, w, h;
        float vw, vh;
        float _pad0, _pad1;
    };
    static_assert(sizeof(RectUniform) == 32, "RectUniform must be 32 bytes");

    const RectUniform vu{ x, y, w, h, viewport_w, viewport_h, 0.f, 0.f };
    SDL_PushGPUVertexUniformData(cmd, 0, &vu, sizeof(vu));

    // Fragment-stage push uniform: RGBA colour.
    // Must be 16 bytes to match `struct ColorUniform` in ui_rect.frag.metal.
    struct alignas(4) ColorUniform {
        float r, g, b, a;
    };
    static_assert(sizeof(ColorUniform) == 16, "ColorUniform must be 16 bytes");

    const ColorUniform fu{ r, g, b, a };
    SDL_PushGPUFragmentUniformData(cmd, 0, &fu, sizeof(fu));

    // Draw: 6 vertices = 2 triangles = 1 quad (no vertex buffer).
    SDL_DrawGPUPrimitives(pass, 6, 1, 0, 0);
}

// ---------------------------------------------------------------------------
// RenderContext::drawText
//
// Draws a pre-rendered text string using the "text" pipeline.
// Texture upload is handled by TextRenderer::flushUploads() BEFORE the render
// pass begins; by the time this is called the texture is already GPU-resident.
//
// Push uniform layout:
//   slot 0, vertex stage   → RectUniform  (same as drawRect)
//   slot 0, fragment stage → TintUniform  {r, g, b, a}
// Sampler binding:
//   fragment sampler slot 0 → text glyph texture
// ---------------------------------------------------------------------------

void RenderContext::drawText(std::string_view text,
                              float x, float y,
                              float size,
                              float r, float g, float b, float a)
{
    if (!pass || !cmd || !text_renderer) return;
    if (!text_renderer->isReady())       return;

    SDL_GPUGraphicsPipeline* pipe = pipeline("text");
    if (!pipe) return;

    // Fetch (or queue) the glyph texture for this string.
    const GlyphTexture gt = text_renderer->ensureTexture(text, size);
    if (!gt.valid()) return;

    SDL_BindGPUGraphicsPipeline(pass, pipe);

    // Vertex push uniform: position the quad at (x, y) with glyph dimensions.
    struct alignas(4) RectUniform {
        float x, y, w, h;
        float vw, vh;
        float _pad0, _pad1;
    };
    const RectUniform vu{
        x, y,
        static_cast<float>(gt.width),
        static_cast<float>(gt.height),
        viewport_w, viewport_h, 0.f, 0.f
    };
    SDL_PushGPUVertexUniformData(cmd, 0, &vu, sizeof(vu));

    // Fragment push uniform: tint colour.
    struct alignas(4) TintUniform { float r, g, b, a; };
    const TintUniform fu{ r, g, b, a };
    SDL_PushGPUFragmentUniformData(cmd, 0, &fu, sizeof(fu));

    // Bind glyph texture + sampler (slot 0).
    SDL_GPUTextureSamplerBinding sb{};
    sb.texture = gt.texture;
    sb.sampler = text_renderer->sampler();
    SDL_BindGPUFragmentSamplers(pass, 0, &sb, 1);

    SDL_DrawGPUPrimitives(pass, 6, 1, 0, 0);
}

SDL_GPUGraphicsPipeline* RenderContext::pipeline(std::string_view name)
{
    // Avoid operator[] — it inserts a default entry on miss.
    // Iterate directly; the map holds ≤ 8 entries on the stack.
    for (auto& [k, v] : pipelines) {
        if (k == name) return v;
    }
    return nullptr;
}

// ===========================================================================
// Layout — file-scope helpers (not part of the public RenderTree API)
//
// flexLayout  — FlexRow / FlexColumn container layout (CSS flex subset).
// blockLayout — vertical stack with gap (CSS block layout subset).
//
// Both functions:
//   • Read layout_props from the CONTAINER node (n) for gap / justify / align.
//   • Read layout_props from each CHILD node for sizing / grow hints.
//   • Write x, y, w, h on each direct child.
//   • Mark each child dirty_layout + dirty_render so the preorder traversal
//     in update() cascades layout to their own children on the same frame.
//
// Called from RenderTree::resolveLayout() when n.dirty_layout is true.
// The slot_map is structurally stable for the duration of the traversal
// (no alloc/free happens inside update callbacks), so RenderNode* pointers
// obtained via tree.node() remain valid for the entire layout pass.
// ===========================================================================

namespace {

// ---------------------------------------------------------------------------
// collectChildren — arena-backed flat array of a node's direct children.
//
// Two-pass: count first, then fill.  Uses the frame_arena so there is no
// heap allocation; the span is valid until the next arena_.reset().
// ---------------------------------------------------------------------------

std::span<RenderNode*> collectChildren(RenderNode& n, RenderTree& tree,
                                        core::frame_arena& arena)
{
    std::size_t count = 0;
    for (NodeHandle c = n.child; c.valid(); ) {
        RenderNode* cn = tree.node(c);
        if (!cn) break;
        ++count;
        c = cn->sibling;
    }

    auto children = arena.allocSpan<RenderNode*>(count);

    std::size_t i = 0;
    for (NodeHandle c = n.child; c.valid(); ) {
        RenderNode* cn = tree.node(c);
        if (!cn) break;
        children[i++] = cn;
        c = cn->sibling;
    }

    return children;
}

// ---------------------------------------------------------------------------
// flexLayout — FlexRow and FlexColumn layout.
//
//   is_column = false → FlexRow    main=X cross=Y
//   is_column = true  → FlexColumn main=Y cross=X
//
// Three-pass algorithm
// --------------------
//   Pass 1 — base sizes
//     For each child:
//       basis = flex_basis  (if ≥ 0)
//             → width / height  (if ≥ 0, axis-dependent)
//             → intrinsic (keep existing w / h)
//     Accumulate total_fixed and total_grow.
//
//   Pass 2 — flex-grow distribution
//     remaining = container_main − total_fixed − total_gap
//     Distribute remaining proportionally to flex_grow (grow only, no shrink).
//
//   Pass 3 — justify + align, then position
//     Recompute free_space after grow.
//     Apply justify-content to derive start_offset and extra_gap.
//     Apply align-items to set cross-axis position (and optionally stretch).
//     Mark each child dirty so its subtree re-layouts next.
// ---------------------------------------------------------------------------

void flexLayout(RenderNode& n, RenderTree& tree, core::frame_arena& arena,
                bool is_column)
{
    auto children = collectChildren(n, tree, arena);
    if (children.empty()) return;

    const float container_main  = is_column ? n.h : n.w;
    const float container_cross = is_column ? n.w : n.h;
    const float gap             = n.layout_props.gap;
    const std::size_t nc        = children.size();

    // ── Pass 1: base sizes ────────────────────────────────────────────────
    //
    // Priority chain: flex_basis → width/height → intrinsic (keep as-is).
    // Sentinel -1 means "not specified; try next fallback".

    float total_fixed = 0.f;
    float total_grow  = 0.f;

    for (RenderNode* child : children) {
        const LayoutProps& lp = child->layout_props;

        float basis = lp.flex_basis;
        if (basis < 0.f)
            basis = is_column ? lp.height : lp.width;

        if (basis >= 0.f) {
            if (is_column) child->h = basis;
            else           child->w = basis;
        }
        // If still auto (-1): keep the child's current w/h (intrinsic size
        // set by a previous draw callback or parent layout on an earlier frame).

        total_fixed += is_column ? child->h : child->w;
        total_grow  += lp.flex_grow;
    }

    // ── Pass 2: flex-grow distribution ────────────────────────────────────

    const float total_gap_px = (nc > 1) ? float(nc - 1) * gap : 0.f;
    const float remaining    = container_main - total_fixed - total_gap_px;

    if (remaining > 0.f && total_grow > 0.f) {
        for (RenderNode* child : children) {
            const float grow = child->layout_props.flex_grow;
            if (grow > 0.f) {
                const float extra = remaining * (grow / total_grow);
                if (is_column) child->h += extra;
                else           child->w += extra;
            }
        }
    }

    // ── Pass 3: justify-content ───────────────────────────────────────────
    //
    // Re-sum content after grow; derive start_offset and per-gap extra.
    // free_space is clamped to ≥ 0 so overflow containers don't produce
    // negative offsets (shrink behaviour is a future Phase 2 addition).

    float total_content = 0.f;
    for (RenderNode* child : children)
        total_content += is_column ? child->h : child->w;

    const float free_space =
        std::max(0.f, container_main - total_content - total_gap_px);

    float start_offset = 0.f;
    float extra_gap    = 0.f;   // additional spacing between items beyond `gap`

    using J = LayoutProps::Justify;
    switch (n.layout_props.justify) {
        case J::Start:
            start_offset = 0.f;
            break;
        case J::Center:
            start_offset = free_space * 0.5f;
            break;
        case J::End:
            start_offset = free_space;
            break;
        case J::SpaceBetween:
            start_offset = 0.f;
            extra_gap    = (nc > 1) ? free_space / float(nc - 1) : 0.f;
            break;
        case J::SpaceAround: {
            const float per = free_space / float(nc);
            start_offset    = per * 0.5f;
            extra_gap       = per;
            break;
        }
    }

    // ── Pass 3 cont.: position + cross-axis align ─────────────────────────

    float main_offset = start_offset;

    using A = LayoutProps::Align;
    for (RenderNode* child : children) {
        const float child_main = is_column ? child->h : child->w;

        // Main-axis position.
        if (is_column) child->y = main_offset;
        else           child->x = main_offset;
        main_offset += child_main + gap + extra_gap;

        // Cross-axis position (align-items).
        switch (n.layout_props.align) {
            case A::Start:
                if (is_column) child->x = 0.f;
                else           child->y = 0.f;
                break;
            case A::Center:
                if (is_column)
                    child->x = (container_cross - child->w) * 0.5f;
                else
                    child->y = (container_cross - child->h) * 0.5f;
                break;
            case A::End:
                if (is_column) child->x = container_cross - child->w;
                else           child->y = container_cross - child->h;
                break;
            case A::Stretch:
                // Only override the size when the child declared it as auto.
                if (is_column) {
                    child->x = 0.f;
                    if (child->layout_props.width < 0.f)
                        child->w = container_cross;
                } else {
                    child->y = 0.f;
                    if (child->layout_props.height < 0.f)
                        child->h = container_cross;
                }
                break;
        }

        // The child's geometry changed — cascade layout to its subtree.
        child->dirty_layout = true;
        child->dirty_render = true;
    }
}

// ---------------------------------------------------------------------------
// blockLayout — vertical stack with optional gap (CSS block layout).
//
// Children are placed top-to-bottom in document order.
// A child with layout_props.width == -1 (auto) inherits the container width.
// Child heights are taken as-is (intrinsic, set by draw callback or prior
// layout pass).
// ---------------------------------------------------------------------------

void blockLayout(RenderNode& n, RenderTree& tree)
{
    const float gap = n.layout_props.gap;
    float y_offset  = 0.f;

    for (NodeHandle c = n.child; c.valid(); ) {
        RenderNode* child = tree.node(c);
        if (!child) break;

        // Auto-width: fill the container.
        if (child->layout_props.width < 0.f)
            child->w = n.w;

        child->x = 0.f;
        child->y = y_offset;
        y_offset += child->h + gap;

        child->dirty_layout = true;
        child->dirty_render = true;

        c = child->sibling;
    }
}

} // anonymous namespace

// ===========================================================================
// RenderTree
// ===========================================================================

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

RenderTree::RenderTree(std::size_t arena_size)
    : arena_(arena_size)
{
}

// ---------------------------------------------------------------------------
// Node allocation  —  delegates entirely to slot_map
//
// slot_map::insert() handles the free-list internally:
//   • Pops a recycled slot when one is available (O(1)).
//   • Appends a new Slot to the backing vector otherwise (amortised O(1)).
//   • Returns a generational SlotID — the NodeHandle.
//
// Generational safety: a previously freed handle whose slot has been
// reused gets a new generation counter, so stale handles returned by
// slot_map::get() are nullptr rather than silently aliasing the new node.
// ---------------------------------------------------------------------------

NodeHandle RenderTree::alloc()
{
    return nodes_.insert(RenderNode{});
}

void RenderTree::free(NodeHandle handle)
{
    if (!handle.valid()) return;

    // Detach from the parent chain first so no dangling LCRS links remain.
    detach(handle);

    // Collect every node in the subtree (BFS via the growth of to_free).
    // Iterating by index avoids iterator invalidation if the vector grows.
    std::vector<NodeHandle> to_free;
    to_free.reserve(16);
    to_free.push_back(handle);

    for (std::size_t i = 0; i < to_free.size(); ++i) {
        const NodeHandle h = to_free[i];
        const RenderNode* n = nodes_.get(h);
        if (!n) continue;

        for (NodeHandle c = n->child; c.valid(); ) {
            const RenderNode* cn = nodes_.get(c);
            const NodeHandle  next = cn ? cn->sibling : k_null_handle;
            to_free.push_back(c);
            c = next;
        }
    }

    // Erase every collected slot — increments each slot's generation so
    // any surviving handles to these nodes become stale immediately.
    for (const NodeHandle h : to_free) {
        nodes_.erase(h);
    }
}

// ---------------------------------------------------------------------------
// Node access
// ---------------------------------------------------------------------------

RenderNode* RenderTree::node(NodeHandle handle)
{
    return nodes_.get(handle);
}

const RenderNode* RenderTree::node(NodeHandle handle) const
{
    return nodes_.get(handle);
}

// ---------------------------------------------------------------------------
// Tree structure
// ---------------------------------------------------------------------------

void RenderTree::appendChild(NodeHandle parent, NodeHandle child)
{
    RenderNode* p = nodes_.get(parent);
    RenderNode* c = nodes_.get(child);
    assert(p && "appendChild — invalid parent handle");
    assert(c && "appendChild — invalid child handle");

    // Detach child from its current parent if it has one.
    if (c->parent.valid()) {
        detach(child);
        c = nodes_.get(child);   // re-fetch: detach does not invalidate
    }

    c->parent  = parent;
    c->sibling = k_null_handle;

    if (!p->child.valid()) {
        // No children yet — child becomes the first.
        p->child = child;
        return;
    }

    // Walk to the last sibling and link there.
    NodeHandle last = p->child;
    for (;;) {
        RenderNode* ln = nodes_.get(last);
        assert(ln && "appendChild — corrupt sibling chain");
        if (!ln->sibling.valid()) {
            ln->sibling = child;
            break;
        }
        last = ln->sibling;
    }
}

void RenderTree::appendChildren(NodeHandle                  parent,
                                 std::span<const NodeHandle> children)
{
    for (const NodeHandle child : children) {
        appendChild(parent, child);
    }
}

void RenderTree::detach(NodeHandle child)
{
    RenderNode* c = nodes_.get(child);
    if (!c || !c->parent.valid()) return;

    RenderNode* p = nodes_.get(c->parent);
    if (!p) {
        // Parent is stale — just clear the child's link.
        c->parent  = k_null_handle;
        c->sibling = k_null_handle;
        return;
    }

    if (p->child == child) {
        // child is the first (leftmost) child.
        p->child = c->sibling;
    } else {
        // Walk the sibling chain to find the predecessor.
        NodeHandle prev = p->child;
        while (prev.valid()) {
            RenderNode* pn = nodes_.get(prev);
            if (!pn) break;
            if (pn->sibling == child) {
                pn->sibling = c->sibling;
                break;
            }
            prev = pn->sibling;
        }
    }

    c->parent  = k_null_handle;
    c->sibling = k_null_handle;
}

// ---------------------------------------------------------------------------
// Dirty propagation
// ---------------------------------------------------------------------------

void RenderTree::markDirty(NodeHandle handle)
{
    for (NodeHandle h = handle; h.valid(); ) {
        RenderNode* n = nodes_.get(h);
        if (!n) break;
        n->dirty_render = true;
        h = n->parent;
    }
}

void RenderTree::markLayoutDirty(NodeHandle handle)
{
    for (NodeHandle h = handle; h.valid(); ) {
        RenderNode* n = nodes_.get(h);
        if (!n) break;
        n->dirty_layout = true;
        n->dirty_render = true;
        h = n->parent;
    }
}

// ---------------------------------------------------------------------------
// traverse — iterative preorder DFS
//
// Visit order: parent → children left-to-right.
//
// Allocation-free inner loop trick
// ---------------------------------
// Children are pushed left→right onto the stack, then the newly pushed
// segment is reversed in-place so the leftmost child sits on top and is
// therefore popped (and visited) first.  No temporary container needed.
//
// Stale handles (slot was erased and generation advanced) are silently
// skipped via the nullptr check on nodes_.get().
//
// Mutation constraint
// -------------------
// fn must not call alloc() or free() during traversal. slot_map::insert()
// may reallocate its internal vector, which would invalidate the RenderNode*
// held by fn. Structural mutations should be queued and applied after render.
// ---------------------------------------------------------------------------

template<std::invocable<NodeHandle, RenderNode&> Fn>
void RenderTree::traverse(NodeHandle start, Fn&& fn)
{
    if (!start.valid()) return;

    std::vector<NodeHandle> stack;
    stack.reserve(64);
    stack.push_back(start);

    while (!stack.empty()) {
        const NodeHandle h = stack.back();
        stack.pop_back();

        RenderNode* n = nodes_.get(h);
        if (!n) continue;   // stale handle — skip gracefully

        // Pre-order visit.
        fn(h, *n);

        // Push children left→right then reverse so leftmost is on top.
        const auto base = static_cast<std::ptrdiff_t>(stack.size());

        for (NodeHandle c = n->child; c.valid(); ) {
            const RenderNode* cn = nodes_.get(c);
            const NodeHandle  next = cn ? cn->sibling : k_null_handle;
            stack.push_back(c);
            c = next;
        }

        std::reverse(stack.begin() + base, stack.end());
    }
}

// ---------------------------------------------------------------------------
// Frame lifecycle
// ---------------------------------------------------------------------------

void RenderTree::beginFrame()
{
    // Reset the bump allocator to offset 0 — all arena memory from the
    // previous frame is reclaimed in a single instruction.  No individual
    // frees, no fragmentation.
    arena_.reset();
}

void RenderTree::update(NodeHandle root)
{
    const NodeHandle start = root.valid() ? root : root_;
    if (!start.valid()) return;

    // Preorder traversal: parent is always visited before its children.
    //
    // For each node:
    //   1. resolveLayout — if dirty_layout, run the container's layout
    //      algorithm to position direct children and mark them dirty.
    //      Because traversal is preorder, those children will then run their
    //      own layouts when visited, cascading geometry down the tree in one
    //      pass without a separate fixpoint loop.
    //   2. update callback — widget-specific per-frame state update.
    //      May set dirty_render = true to trigger a redraw this frame.
    //      May call markLayoutDirty() to request a layout on the next frame.

    traverse(start, [this](NodeHandle h, RenderNode& n) {
        if (n.dirty_layout) {
            resolveLayout(h, n);
            n.dirty_layout = false;
        }
        if (n.update) n.update();
    });
}

// ---------------------------------------------------------------------------
// RenderTree::resolveLayout
//
// Dispatch to the correct layout algorithm based on n.layout_kind.
//
// LayoutKind::None  — no-op: absolute positioning, caller manages children.
// LayoutKind::Block — blockLayout: vertical stack, auto-width children.
// LayoutKind::FlexRow    — flexLayout (is_column=false): CSS flex row.
// LayoutKind::FlexColumn — flexLayout (is_column=true):  CSS flex column.
// LayoutKind::Grid  — reserved for Phase 3.
//
// Every layout function writes x/y/w/h on direct children and sets their
// dirty_layout + dirty_render flags.  The traversal then visits those
// children and cascades layout to their subtrees on the same frame.
// ---------------------------------------------------------------------------

void RenderTree::resolveLayout(NodeHandle /*h*/, RenderNode& n)
{
    switch (n.layout_kind) {
        case LayoutKind::FlexRow:
            flexLayout(n, *this, arena_, false);
            break;
        case LayoutKind::FlexColumn:
            flexLayout(n, *this, arena_, true);
            break;
        case LayoutKind::Block:
            blockLayout(n, *this);
            break;
        case LayoutKind::None:
        case LayoutKind::Grid:
            // Absolute / caller-managed.  Grid reserved for Phase 3.
            break;
    }
}

void RenderTree::render(NodeHandle root, RenderContext& ctx)
{
    if (!root.valid()) return;

    traverse(root, [&ctx](NodeHandle, RenderNode& n) {
        if (n.draw && n.dirty_render) {
            n.draw(ctx);
            n.dirty_render = false;
        }
    });
}

} // namespace pce::sdlos
