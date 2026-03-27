#pragma once

// render_tree.h — Scene graph for sdlos widget rendering.
//
// Namespace : sdlos
// File      : render_tree.h  (lowercase snake_case)
//
// Container layer (sdlos::core)
// ==============================
//   slot_map<RenderNode>        — stable generational node IDs; freed handles
//                                 become invalid immediately (generation check),
//                                 no dangling pointer hazard.
//
//   small_flat_map<K,V,8>       — pipeline cache: up to 8 entries on the stack,
//                                 sorted-vector heap fallback beyond. A typical
//                                 UI needs only rect / text / image / blur — all
//                                 fit on the stack with zero heap traffic.
//
//   frame_arena                 — bump allocator reset once per frame. Use for
//                                 transient vertex data, text quads, layout
//                                 scratch — no individual frees ever needed.
//
// Tree topology: Left-Child / Right-Sibling (LCRS)
// =================================================
//   Each RenderNode stores three NodeHandles:
//     parent  — one level up  (k_null_handle for the root)
//     child   — first child   (left  in the binary-tree reading)
//     sibling — next sibling  (right in the binary-tree reading)
//
//   To iterate all children of node N:
//     for (NodeHandle c = n->child; c.valid(); c = tree.node(c)->sibling) { … }
//
//   This keeps the node struct small and makes insert / remove / traverse O(depth).
//
// Dirty flags
// ===========
//   dirty_layout and dirty_render are propagated lazily upward on write and
//   consumed depth-first on the next update() / render() call. Only nodes
//   with dirty_render set execute their draw callback.
//
// Signal<T>
// =========
//   Reactive value cell. Calling set() notifies observers synchronously.
//   RenderTree::bind() connects a Signal to a node so it auto-marks dirty.
//   Uses C++23 "deducing this" (P0847) on observe() — one template covers
//   lvalue and rvalue Signals without a const / non-const overload pair.
//
// Frame lifecycle
// ===============
//   tree.beginFrame();             // resets frame_arena
//   tree.update(root);             // calls each node's update() preorder
//   // … SDL_BeginGPURenderPass …
//   tree.render(root, ctx);        // calls dirty nodes' draw() preorder
//   // … SDL_EndGPURenderPass …

#include "core/slot_map.hh"
#include "core/small_flat_map.hh"
#include "core/frame_arena.hh"

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include <algorithm>
#include <any>
#include <cassert>
#include <concepts>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pce::sdlos {

// Forward declaration — avoids pulling all of SDL_ttf into every translation
// unit that includes render_tree.hh.  The full definition is in text_renderer.hh.
class TextRenderer;


// ---------------------------------------------------------------------------
// NodeHandle — alias for sdlos::core::SlotID.
//
// Generational safety: a freed node's handle becomes stale immediately.
// slot_map::get(stale_handle) returns nullptr — caught in debug asserts
// and handled gracefully in release (node skipped during traversal).
// ---------------------------------------------------------------------------

using NodeHandle = core::SlotID;

/// The canonical null / invalid handle.
inline constexpr NodeHandle k_null_handle = core::k_null_slot;

// ---------------------------------------------------------------------------
// GPUBackend
// ---------------------------------------------------------------------------

enum class GPUBackend : uint8_t {
    Metal,  // MSL compiled to .metallib  — macOS / iOS
    SpirV,  // GLSL compiled to .spv      — Vulkan (Linux, Windows)
};

// ---------------------------------------------------------------------------
// LayoutKind — selects the built-in layout algorithm for a container node.
//
// Set on the CONTAINER whose children need to be arranged.
// LayoutKind::None means the node does not participate in automatic layout;
// its children's x/y/w/h must be set manually (absolute positioning) or by a
// parent that has its own non-None layout kind.
//
// LayoutKind determines which path resolveLayout() dispatches to.  Custom
// layout can always be implemented in a node's update() callback instead.
// ---------------------------------------------------------------------------

enum class LayoutKind : uint8_t {
    None,       // absolute / caller-managed positions (default)
    Block,      // vertical stack with optional gap between children
    FlexRow,    // flex container, main axis = horizontal (X)
    FlexColumn, // flex container, main axis = vertical   (Y)
    Grid,       // reserved — Phase 3
};

// ---------------------------------------------------------------------------
// LayoutProps — per-node layout hints consumed by resolveLayout().
//
// Container properties (gap, justify, align) are read from the PARENT node
// when laying out its children.  Item properties (width, height, flex_*)
// are read from the CHILD node to determine its size within the parent.
//
// Sentinel value -1.f means "auto":
//   width / height   -1 → use intrinsic size (child->w / child->h as-is)
//                        or stretch to fill the cross axis when align=Stretch
//   flex_basis       -1 → fall back to width / height as the flex base size
//
// All sizes are in pixel space (matching x, y, w, h on RenderNode).
// ---------------------------------------------------------------------------

struct LayoutProps {

    // ---- Sizing (item, read by parent) -----------------------------------

    float width   = -1.f;   // -1 = auto
    float height  = -1.f;   // -1 = auto

    // ---- Flex item properties (read by a FlexRow/FlexColumn parent) ------

    float flex_grow   = 0.f;   // 0 = do not grow beyond basis
    float flex_shrink = 1.f;   // 1 = may shrink when container is too small
    float flex_basis  = -1.f;  // -1 = use width / height as the base size

    // ---- Container properties (read when THIS node is the container) -----

    float gap = 0.f;   // pixel spacing between children on the main axis

    // Justify: how children are packed along the main axis.
    enum class Justify : uint8_t {
        Start,        // pack at main-axis start  (default)
        Center,       // centre all children as a group
        End,          // pack at main-axis end
        SpaceBetween, // first at start, last at end, equal gaps between
        SpaceAround,  // equal space around each child (half-gaps at edges)
    } justify = Justify::Start;

    // Align: how children are aligned on the cross axis.
    enum class Align : uint8_t {
        Start,    // cross-axis start
        Center,   // cross-axis centre
        End,      // cross-axis end
        Stretch,  // fill available cross-axis space when size is auto (default)
    } align = Align::Stretch;
};

// ---------------------------------------------------------------------------
// RenderContext — frame-scoped GPU state threaded through every draw callback.
//
// Constructed once per frame just before SDL_BeginGPURenderPass and passed
// by (non-const) reference to every node's draw() lambda. Must not outlive
// the render pass it was created for.
// ---------------------------------------------------------------------------

struct RenderContext {

    // ---- Frame-level GPU handles (non-owning) ----------------------------

    GPUBackend            backend = GPUBackend::Metal;
    SDL_GPUDevice*        device  = nullptr;
    SDL_GPUCommandBuffer* cmd     = nullptr;
    SDL_GPURenderPass*    pass    = nullptr;

    // ---- Viewport (pixels) -----------------------------------------------

    float viewport_w = 0.f;
    float viewport_h = 0.f;

    // ---- Pipeline cache --------------------------------------------------
    //
    // small_flat_map<K, V, N=8>: stack-storage for ≤8 entries, sorted-vector
    // heap fallback beyond. Typical UI needs: rect, text, image, rounded-rect,
    // blur — all fit on the stack. Lookup is O(log N) binary search.

    core::small_flat_map<std::string, SDL_GPUGraphicsPipeline*, 8> pipelines;

    // ---- Per-frame arena (non-owning) ------------------------------------
    //
    // Points to the RenderTree's owned frame_arena. Reset once at beginFrame().
    // Use for transient vertex buffers, text quad arrays, layout scratch.
    // Do NOT call delete / free on anything allocated from the arena.

    core::frame_arena* arena = nullptr;

    // ---- Text renderer (non-owning) -------------------------------------
    //
    // Set by SDLRenderer::Render() before calling RenderTree::render().
    // Widget draw callbacks call drawText() which delegates here to obtain
    // a cached GPU texture and bind it via the "text" pipeline.
    // Null when text rendering is unavailable; drawText() no-ops gracefully.

    TextRenderer* text_renderer = nullptr;

    // ---- Draw helpers ----------------------------------------------------
    //
    // Submit geometry into `pass`.  Non-const: both functions bind GPU
    // pipelines and push uniform data, which mutates the command buffer state.

    void drawRect(float x, float y, float w, float h,
                  float r, float g, float b, float a = 1.f);

    void drawText(std::string_view text,
                  float x, float y,
                  float size = 16.f,
                  float r = 1.f, float g = 1.f, float b = 1.f, float a = 1.f);

    // ---- Pipeline access -------------------------------------------------

    /// Look up a pipeline by name. Returns nullptr if not registered.
    /// Non-const: small_flat_map exposes only non-const iterators.
    [[nodiscard]] SDL_GPUGraphicsPipeline* pipeline(std::string_view name);
};

// ---------------------------------------------------------------------------
// loadShader — load a compiled shader from disk.
//
// Returns the new SDL_GPUShader* (caller takes ownership via SDL_ReleaseGPUShader)
// on success, or a human-readable error string via std::unexpected on failure.
//
// Path resolution:
//   Metal : "assets/shaders/<name>.metallib"
//   SpirV : "assets/shaders/<name>.spv"
// ---------------------------------------------------------------------------

[[nodiscard]]
std::expected<SDL_GPUShader*, std::string>
loadShader(SDL_GPUDevice*     device,
           GPUBackend         backend,
           std::string_view   name,
           SDL_GPUShaderStage stage,
           uint32_t           num_samplers        = 0,
           uint32_t           num_uniform_buffers = 1);

// ---------------------------------------------------------------------------
// RenderNode — one node in the scene graph.
//
// Stored by value inside sdlos::core::slot_map<RenderNode>.
// Callers hold a NodeHandle (= core::SlotID), never a raw pointer.
//
// All three LCRS links are NodeHandles so the slot_map can reallocate its
// internal vector without invalidating any stored reference.
// ---------------------------------------------------------------------------

struct RenderNode {

    // ---- LCRS tree links -------------------------------------------------

    NodeHandle parent  = k_null_handle;   // one level up
    NodeHandle child   = k_null_handle;   // first child  (left)
    NodeHandle sibling = k_null_handle;   // next sibling (right)

    // ---- Layout (parent-relative pixels) ---------------------------------

    float x{}, y{}, w{}, h{};

    // ---- Dirty flags (bit-fields keep the struct tight) ------------------

    bool dirty_layout : 1 = true;   // geometry needs recompute
    bool dirty_render : 1 = true;   // GPU draw calls must be re-issued

    // ---- Layout ----------------------------------------------------------
    //
    // layout_kind — selects the built-in layout algorithm this node runs as a
    //               CONTAINER (positions its direct children).
    //               LayoutKind::None = no automatic layout (default).
    //
    // layout_props — sizing and alignment hints.
    //   As an ITEM   : width, height, flex_grow, flex_shrink, flex_basis.
    //   As a CONTAINER: gap, justify, align.
    //
    // Changing either field should be followed by markLayoutDirty(handle)
    // so the change is picked up on the next update() pass.

    LayoutKind  layout_kind  = LayoutKind::None;
    LayoutProps layout_props;

    // ---- Per-frame callbacks ---------------------------------------------
    //
    // Both callbacks receive a non-const reference to the node itself as the
    // first argument (`self`).  This removes the need for widgets to capture
    // a `RenderTree*` or a raw `RenderNode*` in their closures — the pointer
    // is valid for the entire duration of the callback because the traversal
    // contract forbids alloc()/free() while fn is executing.
    //
    // draw(self, ctx)  — issue GPU commands into ctx.pass.
    //                    Must be set for any node that produces visible output.
    //                    Read self.x/y/w/h for layout; read self.state for
    //                    widget-specific data via std::any_cast<State>(self.state).
    //
    // update(self)     — mutate time-dependent state before any draw calls.
    //                    May be null.  Setting self.dirty_render = true here
    //                    causes draw() to run on the same frame.

    std::function<void(RenderContext&)> draw;
    std::function<void()>               update;

    // ---- Type-erased widget state ----------------------------------------
    //
    // Store per-widget data (colour, text string, image path, …).
    // Access via std::any_cast<WidgetState>(n->state) in draw/update lambdas.
    // Prefer small, copyable value types to stay within std::any's SBO.

    std::any state;
};

// ---------------------------------------------------------------------------
// Signal<T> — reactive value cell.
//
// Calling set(v) stores the new value and synchronously notifies every
// registered observer. RenderTree::bind() connects a Signal to a node so
// the node is automatically marked dirty on every value change.
//
// C++23 — deducing this (P0847)
// --------------------------------
// observe() uses an explicit object parameter `this Self&& self`:
//   • Compiles on both lvalue and rvalue Signals.
//   • Returns the exact cv-ref-qualified type of *this — enables chaining.
//   • Eliminates separate const / non-const overloads.
// ---------------------------------------------------------------------------

template<typename T>
class Signal {
public:
    explicit Signal(T initial = T{})
        : value_(std::move(initial))
    {}

    Signal(const Signal&)            = delete;
    Signal& operator=(const Signal&) = delete;
    Signal(Signal&&)                 = default;
    Signal& operator=(Signal&&)      = default;

    // ---- Value access ----------------------------------------------------

    [[nodiscard]] const T& get()               const noexcept { return value_; }
    [[nodiscard]] operator const T&()          const noexcept { return value_; }

    // ---- Mutation --------------------------------------------------------

    void set(T v)
    {
        value_ = std::move(v);
        for (auto& obs : observers_) obs(value_);
    }

    // ---- Observer registration (C++23 deducing this) --------------------

    template<typename Self>
    auto& observe(this Self&& self, std::function<void(const T&)> fn)
    {
        self.observers_.push_back(std::move(fn));
        return self;
    }

    [[nodiscard]] std::size_t observerCount() const noexcept
    {
        return observers_.size();
    }

private:
    T                                          value_;
    std::vector<std::function<void(const T&)>> observers_;
};

// ---------------------------------------------------------------------------
// RenderTree — owns all RenderNode objects and drives the frame lifecycle.
// ---------------------------------------------------------------------------

class RenderTree {
public:
    /// `arena_size` — initial frame arena capacity in bytes (default 1 MiB).
    explicit RenderTree(std::size_t arena_size = 1u << 20);
    ~RenderTree() = default;

    RenderTree(const RenderTree&)            = delete;
    RenderTree& operator=(const RenderTree&) = delete;

    // ---- Node allocation -------------------------------------------------

    /// Allocate a new, unattached RenderNode. O(1) amortised.
    [[nodiscard]] NodeHandle alloc();

    /// Free `handle` and recursively free every node in its subtree.
    /// All handles in the subtree become stale (generation-invalidated).
    void free(NodeHandle handle);

    // ---- Node access -----------------------------------------------------

    /// Return a pointer to the live node, or nullptr for stale/null handles.
    /// Prefer asserting non-null in debug paths; handle nullptr gracefully
    /// in release (the traversal skips null nodes automatically).
    [[nodiscard]] RenderNode*       node(NodeHandle handle);
    [[nodiscard]] const RenderNode* node(NodeHandle handle) const;

    // ---- Tree structure --------------------------------------------------

    /// Append `child` as the last child of `parent`.
    /// If `child` already has a parent, it is detached first.
    void appendChild(NodeHandle parent, NodeHandle child);

    /// Append multiple children in order. std::span accepts any contiguous range.
    void appendChildren(NodeHandle parent, std::span<const NodeHandle> children);

    /// Remove `child` from its parent's child list without freeing it.
    void detach(NodeHandle child);

    // ---- Dirty propagation -----------------------------------------------

    /// Mark `handle` and every ancestor render-dirty.
    void markDirty(NodeHandle handle);

    /// Mark `handle` and every ancestor layout-dirty (implies render-dirty).
    void markLayoutDirty(NodeHandle handle);

    // ---- Signal binding --------------------------------------------------

    /// Whenever `signal` changes, markDirty(handle) is called automatically.
    /// The Signal must outlive the RenderTree (or be unbound before destruction).
    template<typename T>
    void bind(Signal<T>& signal, NodeHandle handle)
    {
        signal.observe([this, handle](const T&) {
            markDirty(handle);
        });
    }

    // ---- Frame lifecycle -------------------------------------------------

    /// Reset the frame arena. Call once at the top of each frame, before update().
    void beginFrame();

    /// Call each node's update() callback in depth-first preorder.
    /// Pass k_null_handle (default) to start from root_.
    void update(NodeHandle root = k_null_handle);

    /// Call each dirty node's draw() callback in depth-first preorder.
    /// Clears dirty_render on each node after its draw() executes.
    void render(NodeHandle root, RenderContext& ctx);

    // ---- Root ------------------------------------------------------------

    [[nodiscard]] NodeHandle root()           const noexcept { return root_; }
    void                     setRoot(NodeHandle h) noexcept { root_ = h;    }

    // ---- Arena access (for RenderContext wiring) -------------------------

    [[nodiscard]] core::frame_arena& arena() noexcept { return arena_; }

    // ---- Diagnostics -----------------------------------------------------

    [[nodiscard]] std::size_t nodeCount() const noexcept { return nodes_.size();     }
    [[nodiscard]] std::size_t capacity()  const noexcept { return nodes_.capacity(); }

private:
    // Iterative preorder DFS over the subtree rooted at `start`.
    // `fn` receives (NodeHandle, RenderNode&) for each live node.
    // Constraint: fn must not call alloc() or free() during traversal.
    template<std::invocable<NodeHandle, RenderNode&> Fn>
    void traverse(NodeHandle start, Fn&& fn);

    // Dispatch to the appropriate layout algorithm for node `n`.
    // Called from update() when n.dirty_layout is true.
    // Reads n.layout_kind and n.layout_props; writes x/y/w/h of n's direct
    // children and sets their dirty_layout / dirty_render flags so the
    // subsequent preorder traversal propagates layout to their subtrees.
    void resolveLayout(NodeHandle h, RenderNode& n);

    core::slot_map<RenderNode> nodes_;
    core::frame_arena          arena_;
    NodeHandle                 root_ = k_null_handle;
};

} // namespace pce::sdlos
