// render_tree.cxx — Scene graph implementation.
//
// Namespace : sdlos
// File      : render_tree.cxx  (lowercase snake_case)
//
// Responsibilities
// ================
//   loadShader()   — read compiled shader blob from disk;
//                    return std::expected<SDL_GPUShader*, std::string>
//   RenderContext  — pipeline lookup, drawRect / drawText geometry stubs
//   RenderTree     — slot_map-based node alloc/free, LCRS tree ops,
//                    dirty propagation, iterative preorder DFS,
//                    frame_arena reset, update / render lifecycle

#include "render_tree.hh"

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <vector>

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

namespace sdlos {

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

SDL_GPUGraphicsPipeline* RenderContext::pipeline(std::string_view name)
{
    // Avoid operator[] — it inserts a default entry on miss.
    // Iterate directly; the map holds ≤ 8 entries on the stack.
    for (auto& [k, v] : pipelines) {
        if (k == name) return v;
    }
    return nullptr;
}

void RenderContext::drawRect(float x, float y, float w, float h,
                              float r, float g, float b, float a) const
{
    // TODO: allocate 6 vertices from arena, write NDC quad, bind rect
    //       pipeline and issue SDL_DrawGPUPrimitives(pass, 6, 1, 0, 0).
    //
    //   struct Vertex { float pos[2]; float color[4]; };
    //   auto* v = static_cast<Vertex*>(
    //       arena->alloc(6 * sizeof(Vertex), alignof(Vertex)));
    //   v[0] = {{x,   y  }, {r,g,b,a}};
    //   v[1] = {{x+w, y  }, {r,g,b,a}};
    //   v[2] = {{x+w, y+h}, {r,g,b,a}};
    //   v[3] = {{x,   y  }, {r,g,b,a}};
    //   v[4] = {{x+w, y+h}, {r,g,b,a}};
    //   v[5] = {{x,   y+h}, {r,g,b,a}};
    //   SDL_BindGPUGraphicsPipeline(pass, pipeline("rect"));
    //   SDL_DrawGPUPrimitives(pass, 6, 1, 0, 0);
    (void)x; (void)y; (void)w; (void)h;
    (void)r; (void)g; (void)b; (void)a;
}

void RenderContext::drawText(std::string_view text,
                              float x, float y, float size) const
{
    // TODO: walk the GlyphAtlas, build one TextVertex quad per glyph from
    //       the arena, bind the "text" pipeline + font atlas sampler, and
    //       issue SDL_DrawGPUPrimitives(pass, glyph_count * 6, 1, 0, 0).
    //
    //   struct TextVertex { float pos[2]; float uv[2]; float color[4]; };
    //   SDL_BindGPUGraphicsPipeline(pass, pipeline("text"));
    //   SDL_BindGPUFragmentSamplers(pass, 0, &font_sampler, 1);
    //   SDL_DrawGPUPrimitives(pass, glyph_count * 6, 1, 0, 0);
    (void)text; (void)x; (void)y; (void)size;
}

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

    traverse(start, [](NodeHandle, RenderNode& n) {
        if (n.update) n.update();
    });
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

} // namespace sdlos
