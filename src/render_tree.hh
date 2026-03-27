#pragma once

#include "core/slot_map.hh"
#include "core/small_flat_map.hh"
#include "core/frame_arena.hh"

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include <any>
#include <cassert>
#include <concepts>
#include <cstdint>
#include <expected>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pce::sdlos {

class TextRenderer;

// ---------------------------------------------------------------------------
// NodeHandle / k_null_handle
// ---------------------------------------------------------------------------

using NodeHandle = core::SlotID;
inline constexpr NodeHandle k_null_handle = core::k_null_slot;

// ---------------------------------------------------------------------------
// GPUBackend
// ---------------------------------------------------------------------------

enum class GPUBackend : uint8_t {
    Metal,  // MSL — macOS / iOS
    SpirV,  // GLSL → .spv — Vulkan / Linux
};

// ---------------------------------------------------------------------------
// LayoutKind  —  set on the CONTAINER, selects the layout algorithm.
// ---------------------------------------------------------------------------

enum class LayoutKind : uint8_t {
    None,       // absolute / caller-managed (default)
    Block,      // vertical stack
    FlexRow,    // flex, main axis = X
    FlexColumn, // flex, main axis = Y
    Grid,       // reserved — Phase 3
};

// ---------------------------------------------------------------------------
// LayoutProps  —  hints consumed by resolveLayout().
//
//   As an item       : width, height, flex_grow, flex_shrink, flex_basis
//   As a container   : gap, justify, align
//   -1.f             : auto (use intrinsic size / stretch)
// ---------------------------------------------------------------------------

struct LayoutProps {

    float width   = -1.f;
    float height  = -1.f;

    float flex_grow   = 0.f;
    float flex_shrink = 1.f;
    float flex_basis  = -1.f;   // -1 → use width / height

    float gap = 0.f;

    enum class Justify : uint8_t {
        Start, Center, End, SpaceBetween, SpaceAround,
    } justify = Justify::Start;

    enum class Align : uint8_t {
        Start, Center, End,
        Stretch,    // fills cross-axis when size is auto (default)
    } align = Align::Stretch;
};

// ---------------------------------------------------------------------------
// RenderContext  —  frame-scoped GPU state threaded into every draw callback.
// ---------------------------------------------------------------------------

struct RenderContext {

    GPUBackend            backend = GPUBackend::Metal;
    SDL_GPUDevice*        device  = nullptr;
    SDL_GPUCommandBuffer* cmd     = nullptr;
    SDL_GPURenderPass*    pass    = nullptr;

    float viewport_w = 0.f;    // physical pixels — from swapchain
    float viewport_h = 0.f;

    core::small_flat_map<std::string, SDL_GPUGraphicsPipeline*, 8> pipelines;

    core::frame_arena* arena         = nullptr;
    TextRenderer*      text_renderer = nullptr;  // null → drawText no-ops

    void drawRect(float x, float y, float w, float h,
                  float r, float g, float b, float a = 1.f);

    void drawText(std::string_view text,
                  float x, float y,
                  float size = 16.f,
                  float r = 1.f, float g = 1.f, float b = 1.f, float a = 1.f);

    // Four filled rects — no extra shader needed.
    void drawRectOutline(float x, float y, float w, float h,
                         float thickness,
                         float r, float g, float b, float a = 1.f);

    [[nodiscard]] SDL_GPUGraphicsPipeline* pipeline(std::string_view name);
};

// ---------------------------------------------------------------------------
// loadShader
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
// RenderNode  —  one node in the scene graph (stored by value in slot_map).
//
// See architecture.md for the LCRS traversal pattern and dirty-flag rules.
// ---------------------------------------------------------------------------

struct RenderNode {

    // LCRS links
    NodeHandle parent  = k_null_handle;
    NodeHandle child   = k_null_handle;
    NodeHandle sibling = k_null_handle;

    // Geometry — pixel space, parent-relative
    float x{}, y{}, w{}, h{};

    bool dirty_layout : 1 = true;
    bool dirty_render : 1 = true;

    LayoutKind  layout_kind  = LayoutKind::None;
    LayoutProps layout_props;

    std::function<void(RenderContext&)> draw;
    std::function<void()>               update;

    std::any state;  // widget-local; access via std::any_cast<State>(n->state)
};

// ---------------------------------------------------------------------------
// Signal<T>  —  reactive value cell.
//
// set() stores the value and synchronously notifies observers.
// RenderTree::bind() wires a Signal to markDirty() on a node.
// observe() uses C++23 deducing-this — one template, works on lvalue/rvalue.
// ---------------------------------------------------------------------------

template<typename T>
class Signal {
public:
    explicit Signal(T initial = T{}) : value_(std::move(initial)) {}

    Signal(const Signal&)            = delete;
    Signal& operator=(const Signal&) = delete;
    Signal(Signal&&)                 = default;
    Signal& operator=(Signal&&)      = default;

    [[nodiscard]] const T& get()      const noexcept { return value_; }
    [[nodiscard]] operator const T&() const noexcept { return value_; }

    void set(T v)
    {
        value_ = std::move(v);
        for (auto& obs : observers_) obs(value_);
    }

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
// RenderTree  —  owns all nodes; drives the frame lifecycle.
// ---------------------------------------------------------------------------

class RenderTree {
public:
    /// arena_size — initial frame arena capacity in bytes (default 1 MiB).
    explicit RenderTree(std::size_t arena_size = 1u << 20);
    ~RenderTree() = default;

    RenderTree(const RenderTree&)            = delete;
    RenderTree& operator=(const RenderTree&) = delete;

    // ---- Node allocation -------------------------------------------------

    [[nodiscard]] NodeHandle alloc();

    /// Recursively frees the node and its entire subtree.
    void free(NodeHandle handle);

    // ---- Node access -----------------------------------------------------

    [[nodiscard]] RenderNode*       node(NodeHandle handle);
    [[nodiscard]] const RenderNode* node(NodeHandle handle) const;

    // ---- Tree structure --------------------------------------------------

    void appendChild(NodeHandle parent, NodeHandle child);
    void appendChildren(NodeHandle parent, std::span<const NodeHandle> children);
    void detach(NodeHandle child);

    // ---- Dirty propagation  (upward to ancestors) ------------------------

    void markDirty(NodeHandle handle);
    void markLayoutDirty(NodeHandle handle);

    // ---- Signal binding --------------------------------------------------

    /// Signal must outlive this RenderTree.
    template<typename T>
    void bind(Signal<T>& signal, NodeHandle handle)
    {
        signal.observe([this, handle](const T&) { markDirty(handle); });
    }

    // ---- Frame lifecycle  (see architecture.md) --------------------------

    void beginFrame();                              // reset frame_arena
    void update(NodeHandle root = k_null_handle);   // layout cascade + update()
    void render(NodeHandle root, RenderContext& ctx);// draw() on dirty nodes

    // ---- Root ------------------------------------------------------------

    [[nodiscard]] NodeHandle root()               const noexcept { return root_; }
    void                     setRoot(NodeHandle h)      noexcept { root_ = h;    }

    // ---- Accessors -------------------------------------------------------

    [[nodiscard]] core::frame_arena& arena()      noexcept       { return arena_; }
    [[nodiscard]] std::size_t        nodeCount()  const noexcept { return nodes_.size();     }
    [[nodiscard]] std::size_t        capacity()   const noexcept { return nodes_.capacity(); }

private:
    // fn(NodeHandle, RenderNode&) — must not call alloc()/free() during traversal.
    template<std::invocable<NodeHandle, RenderNode&> Fn>
    void traverse(NodeHandle start, Fn&& fn);

    void resolveLayout(NodeHandle h, RenderNode& n);

    core::slot_map<RenderNode> nodes_;
    core::frame_arena          arena_;
    NodeHandle                 root_ = k_null_handle;
};

} // namespace pce::sdlos
