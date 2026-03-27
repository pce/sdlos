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

// ---- loadShader ----

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

// ---- RenderContext ----

// drawRect — no vertex buffer; ui_rect.vert generates a 6-vertex CCW quad
// entirely from push-uniform data.
//
// Push uniform layout (must match shader structs):
//   slot 0, vertex stage   → {x, y, w, h, viewport_w, viewport_h, _pad, _pad}
//   slot 0, fragment stage → {r, g, b, a}

void RenderContext::drawRect(float x, float y, float w, float h,
                              float r, float g, float b, float a)
{
    if (!pass || !cmd) return;

    SDL_GPUGraphicsPipeline* pipe = pipeline("rect");
    if (!pipe) return;

    SDL_BindGPUGraphicsPipeline(pass, pipe);

    // Must be 32 bytes — matches struct RectUniform in ui_rect.vert.metal.
    struct alignas(4) RectUniform {
        float x, y, w, h;
        float vw, vh;
        float _pad0, _pad1;
    };
    static_assert(sizeof(RectUniform) == 32, "RectUniform must be 32 bytes");

    const RectUniform vu{ x, y, w, h, viewport_w, viewport_h, 0.f, 0.f };
    SDL_PushGPUVertexUniformData(cmd, 0, &vu, sizeof(vu));

    // Must be 16 bytes — matches struct ColorUniform in ui_rect.frag.metal.
    struct alignas(4) ColorUniform {
        float r, g, b, a;
    };
    static_assert(sizeof(ColorUniform) == 16, "ColorUniform must be 16 bytes");

    const ColorUniform fu{ r, g, b, a };
    SDL_PushGPUFragmentUniformData(cmd, 0, &fu, sizeof(fu));

    SDL_DrawGPUPrimitives(pass, 6, 1, 0, 0);  // 6 verts = 2 tris = 1 quad
}

// drawText — the glyph texture must already be GPU-resident; call
// TextRenderer::flushUploads() BEFORE SDL_BeginGPURenderPass.
//
// Push uniform layout:
//   slot 0, vertex stage   → RectUniform  (same as drawRect)
//   slot 0, fragment stage → TintUniform  {r, g, b, a}
// Sampler binding:
//   fragment sampler slot 0 → glyph texture

void RenderContext::drawText(std::string_view text,
                              float x, float y,
                              float size,
                              float r, float g, float b, float a)
{
    if (!pass || !cmd || !text_renderer) return;
    if (!text_renderer->isReady())       return;

    SDL_GPUGraphicsPipeline* pipe = pipeline("text");
    if (!pipe) return;

    const GlyphTexture gt = text_renderer->ensureTexture(text, size);
    if (!gt.valid()) return;

    SDL_BindGPUGraphicsPipeline(pass, pipe);

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

    struct alignas(4) TintUniform { float r, g, b, a; };
    const TintUniform fu{ r, g, b, a };
    SDL_PushGPUFragmentUniformData(cmd, 0, &fu, sizeof(fu));

    SDL_GPUTextureSamplerBinding sb{};
    sb.texture = gt.texture;
    sb.sampler = text_renderer->sampler();
    SDL_BindGPUFragmentSamplers(pass, 0, &sb, 1);

    SDL_DrawGPUPrimitives(pass, 6, 1, 0, 0);
}

SDL_GPUGraphicsPipeline* RenderContext::pipeline(std::string_view name)
{
    // Avoid operator[] — it inserts a default entry on miss.
    // Iterate directly; the map holds ≤ 8 entries.
    for (auto& [k, v] : pipelines) {
        if (k == name) return v;
    }
    return nullptr;
}

// drawRectOutline — four axis-aligned filled rects:
//
//   ┌─────────────────────┐   ← top    (x, y,        w,     t)
//   │                     │   ← left   (x, y+t,      t, h−2t)
//   │                     │   ← right  (x+w−t, y+t,  t, h−2t)
//   └─────────────────────┘   ← bottom (x, y+h−t,    w,     t)
//
// Top/bottom claim the corners; left/right span only h−2t for clean mitre joints.
// `thickness` is clamped to ≤ min(w,h)/2 so rects never have negative extents.

void RenderContext::drawRectOutline(float x, float y, float w, float h,
                                     float thickness,
                                     float r, float g, float b, float a)
{
    const float t = std::min(thickness, std::min(w, h) * 0.5f);
    if (t <= 0.f || w <= 0.f || h <= 0.f) return;

    drawRect(x,         y,         w,   t,       r, g, b, a);  // top
    drawRect(x,         y + h - t, w,   t,       r, g, b, a);  // bottom
    drawRect(x,         y + t,     t,   h - 2*t, r, g, b, a);  // left
    drawRect(x + w - t, y + t,     t,   h - 2*t, r, g, b, a);  // right
}

// ---- Layout helpers (file-scope) ----
//
// Called from RenderTree::resolveLayout() when n.dirty_layout is true.
// The slot_map is structurally stable for the entire layout pass (no
// alloc/free inside update callbacks), so RenderNode* pointers remain valid.

namespace {

// collectChildren — arena-backed flat array of a node's direct children.
// Two-pass (count then fill). Span is valid until the next arena_.reset().

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

// flexLayout — FlexRow (is_column=false, main=X cross=Y) and
//              FlexColumn (is_column=true,  main=Y cross=X).
//
// Three-pass algorithm:
//   Pass 1 — base sizes.
//     Priority: flex_basis → width/height → intrinsic (keep existing w/h).
//     Sentinel -1 means "not specified; use next fallback".
//   Pass 2 — flex-grow distribution.
//     remaining = container_main − total_fixed − total_gap
//     Distributed proportionally to flex_grow (grow only, no shrink).
//   Pass 3 — justify-content + align-items, then position.

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
        // Still -1 (auto): keep existing w/h — intrinsic size set by a draw
        // callback or parent layout on an earlier frame.

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
    // free_space clamped ≥ 0: overflow containers don't produce negative
    // offsets (shrink is a future addition).

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

        if (is_column) child->y = main_offset;
        else           child->x = main_offset;
        main_offset += child_main + gap + extra_gap;

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
                // Only override the size when the child declared it as auto (-1).
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

        // Cascade layout to this child's subtree.
        child->dirty_layout = true;
        child->dirty_render = true;
    }
}

// blockLayout — vertical stack with optional gap.
// A child with layout_props.width == -1 (auto) inherits the container width.
// Child heights are taken as-is (intrinsic).

void blockLayout(RenderNode& n, RenderTree& tree)
{
    const float gap = n.layout_props.gap;
    float y_offset  = 0.f;

    for (NodeHandle c = n.child; c.valid(); ) {
        RenderNode* child = tree.node(c);
        if (!child) break;

        if (child->layout_props.width < 0.f)  // -1 = auto: fill container
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

// ---- RenderTree ----

RenderTree::RenderTree(std::size_t arena_size)
    : arena_(arena_size)
{
}

// alloc — O(1) amortised via slot_map free-list.
// Generational handles: a stale handle whose slot was reused returns nullptr
// from node(), never silently aliasing the new occupant.

NodeHandle RenderTree::alloc()
{
    return nodes_.insert(RenderNode{});
}

void RenderTree::free(NodeHandle handle)
{
    if (!handle.valid()) return;

    // Detach first so no dangling LCRS links remain.
    detach(handle);

    // BFS subtree collection via vector growth — index-based loop avoids
    // iterator invalidation when the vector reallocates.
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

    // Erasing increments each slot's generation — surviving handles to these
    // nodes become stale immediately.
    for (const NodeHandle h : to_free) {
        nodes_.erase(h);
    }
}

// ---- Node access ----

RenderNode* RenderTree::node(NodeHandle handle)
{
    return nodes_.get(handle);
}

const RenderNode* RenderTree::node(NodeHandle handle) const
{
    return nodes_.get(handle);
}

// ---- Tree structure ----

void RenderTree::appendChild(NodeHandle parent, NodeHandle child)
{
    RenderNode* p = nodes_.get(parent);
    RenderNode* c = nodes_.get(child);
    assert(p && "appendChild — invalid parent handle");
    assert(c && "appendChild — invalid child handle");

    if (c->parent.valid()) {
        detach(child);
        c = nodes_.get(child);   // re-fetch: detach does not invalidate
    }

    c->parent  = parent;
    c->sibling = k_null_handle;

    if (!p->child.valid()) {
        p->child = child;
        return;
    }

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
        // Parent is stale — clear the child's link.
        c->parent  = k_null_handle;
        c->sibling = k_null_handle;
        return;
    }

    if (p->child == child) {
        p->child = c->sibling;
    } else {
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

// ---- Dirty propagation ----

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

// traverse — iterative preorder DFS (parent before children, left-to-right).
//
// Children are pushed left→right onto the stack then the newly pushed segment
// is reversed in-place so the leftmost child sits on top — allocation-free
// ordering trick, no temporary container needed.
// Stale handles are silently skipped via the nullptr check on nodes_.get().
//
// Mutation constraint: fn must not call alloc() or free() during traversal.
// slot_map::insert() may reallocate its backing vector, invalidating any
// RenderNode* held by fn. Queue structural mutations and apply after render.

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

        fn(h, *n);

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

// ---- Frame lifecycle ----

void RenderTree::beginFrame()
{
    // O(1) bump-allocator reset: reclaims all arena memory from the previous
    // frame in a single instruction — no individual frees, no fragmentation.
    arena_.reset();
}

void RenderTree::update(NodeHandle root)
{
    const NodeHandle start = root.valid() ? root : root_;
    if (!start.valid()) return;

    // Because traversal is preorder, resolveLayout on a dirty_layout node
    // writes geometry onto direct children and marks them dirty_layout too —
    // their own layouts then run when they are visited, cascading down the
    // tree in one pass without a separate fixpoint loop.
    traverse(start, [this](NodeHandle h, RenderNode& n) {
        if (n.dirty_layout) {
            resolveLayout(h, n);
            n.dirty_layout = false;
        }
        if (n.update) n.update();
    });
}

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
            // Absolute / caller-managed. Grid reserved for Phase 3.
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
