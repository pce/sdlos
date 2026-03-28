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
#include <utility>
#include <vector>

namespace pce::sdlos {

class TextRenderer;
class ImageCache;
class VideoTexture;

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

    // Percent-of-parent sizing (resolved during layout; takes priority over
    // explicit width/height when >= 0).
    float width_pct  = -1.f;   // -1 = not set; 0..100 = % of parent dimension
    float height_pct = -1.f;

    // Post-layout translate (percent of this node's own size, draw-time only).
    // Does not affect layout geometry or hit testing.
    float translate_x_pct = 0.f;  // % of own width  (negative = shift left)
    float translate_y_pct = 0.f;  // % of own height (negative = shift up)

    float gap = 0.f;

    // flex-wrap: allow items to wrap onto the next line when the container
    // overflows.  The layout engine checks this flag; multi-line logic is
    // Phase 2 remaining work.
    bool flex_wrap = false;

    // overflow — controls what happens when children exceed this node's bounds.
    //   Visible — default: children render outside bounds, no clipping.
    //   Hidden  — clip children to this node's bounds via a GPU scissor rect.
    //             draw() sets the scissor; after_draw() restores it.
    //   Scroll  — reserved: requires ScrollContainer (Phase 3).
    enum class Overflow : uint8_t {
        Visible,
        Hidden,
        Scroll,     // reserved
    } overflow = Overflow::Visible;

    enum class Justify : uint8_t {
        Start, Center, End, SpaceBetween, SpaceAround,
    } justify = Justify::Start;

    enum class Align : uint8_t {
        Start, Center, End,
        Stretch,    // fills cross-axis when size is auto (default)
    } align = Align::Stretch;
};


struct VisualProps {

    enum class TextAlign : uint8_t {
        Left,
        Center,   // default — preserves the existing centred-text behaviour
        Right,
    } text_align = TextAlign::Center;

    enum class FontWeight : uint8_t {
        Normal,
        Bold,
    } font_weight = FontWeight::Normal;

    // Inner padding — shrinks the content area used for child layout and
    // text positioning.  Expressed in the same physical-pixel space as w/h.
    float padding_left   = 0.f;
    float padding_right  = 0.f;
    float padding_top    = 0.f;
    float padding_bottom = 0.f;

    // Corner radius (physical px).  Stored here for use by a future SDF
    // rounded-rect shader; the current CPU rasteriser ignores it.
    float border_radius = 0.f;

    // Controls how an image fills its bounding box when the aspect ratio
    // of the source file and the node's w×h differ.
    //   Fill    — stretch to exactly w×h (default; may distort).
    //   Contain — scale uniformly to fit inside w×h; letterbox empty space.
    //   Cover   — scale to fill w×h, UV-crop center
    enum class ObjectFit : uint8_t {
        Fill,       // stretch to w×h (default)
        Contain,    // scale to fit inside w×h, letterbox
        Cover,      // scale to fill w×h, UV-crop center
    } object_fit = ObjectFit::Fill;
};

// ---------------------------------------------------------------------------
// NodeShaderParams  —  standard 32-byte uniform block for _shader fragments.
//
// Every custom fragment shader (loaded via the _shader="name" jade attribute)
// must declare this struct at buffer(0) with entry point main0:
//
//   fragment float4 main0(VertOut in [[stage_in]],
//                         constant NodeShaderParams& p [[buffer(0)]],
//                         texture2d<float> tex         [[texture(0)]],
//                         sampler          samp         [[sampler(0)]])
//
// Populated by style_draw.cxx from _shader_param0/1/2, _shader_focusX/Y attrs.
// ---------------------------------------------------------------------------

struct NodeShaderParams {
    float u_width;    // node width  in physical pixels
    float u_height;   // node height in physical pixels
    float u_focusX;   // normalised focus point X [0..1]  (default 0.5)
    float u_focusY;   // normalised focus point Y [0..1]  (default 0.5)
    float u_param0;   // shader-defined  (blurScale, contrast, etc.)
    float u_param1;   // shader-defined
    float u_param2;   // shader-defined
    float u_time;     // seconds since start (jitter, animation)
};
static_assert(sizeof(NodeShaderParams) == 32, "NodeShaderParams must be 32 bytes");

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
    ImageCache*        image_cache   = nullptr;  // null → drawImage no-ops
    VideoTexture*      video_texture = nullptr;  ///< null → drawVideo is a no-op

    // Seconds since app start — forwarded to _shader fragment uniforms
    // (u_time) so shaders can use it for jitter, animation, etc.
    float time = 0.f;

    // Callback that returns (or lazily compiles) a named node shader pipeline.
    // Set by SDLRenderer::Render(); null means _shader nodes fall back to
    // regular drawImage.
    std::function<SDL_GPUGraphicsPipeline*(std::string_view)> nodeShaderPipeline;

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

    // Draw a GPU-resident image from `src` path using the ImageCache.
    // Missing / failed paths produce no draw call (silent no-op after the
    // first load attempt, which logs the error and caches the failure).
    //
    // `opacity`: multiplied into the image's own alpha channel.
    void drawImage(std::string_view src,
                   float x, float y, float w, float h,
                   float opacity = 1.f,
                   VisualProps::ObjectFit fit = VisualProps::ObjectFit::Fill);

    // Draw an image through a user-supplied fragment shader loaded from
    // data/shaders/{platform}/{shader_name}.frag.{ext}.
    // NodeShaderParams is pushed to the fragment's buffer(0); the image
    // texture is bound to sampler(0) — same as drawImage.
    // Falls back to drawImage() when the shader pipeline is unavailable.
    void drawImageWithShader(std::string_view src,
                              std::string_view shader_name,
                              float x, float y, float w, float h,
                              float opacity,
                              VisualProps::ObjectFit fit,
                              const struct NodeShaderParams& shader_params);

    /// Draw the current webcam/video frame (no shader).
    void drawVideo(float x, float y, float w, float h, float opacity = 1.f);

    /// Draw the current webcam/video frame through a named node shader.
    /// Falls back to drawVideo() when the shader pipeline is unavailable.
    void drawVideoWithShader(std::string_view shader_name,
                             float x, float y, float w, float h,
                             float opacity,
                             const struct NodeShaderParams& shader_params);

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
// StyleMap  —  flat string→string property bag attached to each RenderNode.
//
// Populated by the JadeLite / XML parsers after parsing.
// The StyleApplier reads it and writes typed values into layout_kind,
// layout_props, x/y/w/h.  The draw callback may also read it directly
// (e.g. "backgroundColor", "fontSize").
//
// Implemented as a vector of pairs:
//   - O(N) linear scan, but N is always tiny (< 20 entries per node).
//   - Empty vector = zero heap allocation — three pointer words on the stack.
//   - C++-constructed nodes (widgets, overlays) carry zero overhead.
// ---------------------------------------------------------------------------

using StyleMap = std::vector<std::pair<std::string, std::string>>;

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
    VisualProps visual_props;

    std::function<void(RenderContext&)> draw;
    std::function<void()>               update;

    // after_draw — called once after ALL children of this node have been
    // rendered.  Used to restore GPU state set in draw() that should only
    // apply to the subtree (e.g. scissor rect for overflow:hidden, stencil
    // pop for mask effects).  Leave null when not needed.
    std::function<void(RenderContext&)> after_draw;

    std::any state;  // widget-local; access via std::any_cast<State>(n->state)

    // ── Parsed styles ─────────────────────────────────────────────────────
    //
    // Set by the JadeLite / XML parser; empty on all C++-constructed nodes.
    // Common keys: "tag", "id", "class", "text",
    //              "width", "height", "x", "y",
    //              "flexDirection", "flexGrow", "flexShrink", "flexBasis",
    //              "gap", "backgroundColor", "color", "fontSize".
    StyleMap styles;

    // Return the value for key, or an empty string_view if not present.
    [[nodiscard]] std::string_view style(std::string_view key) const noexcept
    {
        for (const auto& [k, v] : styles)
            if (k == key) return v;
        return {};
    }

    // Insert or overwrite a style entry.
    void setStyle(std::string_view key, std::string value)
    {
        for (auto& [k, v] : styles)
            if (k == key) { v = std::move(value); return; }
        styles.emplace_back(std::string(key), std::move(value));
    }

    [[nodiscard]] bool hasStyle(std::string_view key) const noexcept
    {
        for (const auto& [k, v] : styles)
            if (k == key) return true;
        return false;
    }
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

    // ---- Query -----------------------------------------------------------

    /// DFS walk from 'start'; returns the first node whose style("id") == id.
    /// Returns k_null_handle if not found.
    [[nodiscard]] NodeHandle findById(NodeHandle start, std::string_view id) const;

    /// DFS walk from 'start'; returns every node whose space-separated
    /// style("class") list contains the given token.
    [[nodiscard]] std::vector<NodeHandle> findByClass(NodeHandle start,
                                                      std::string_view cls) const;

    // ---- Dirty helpers — used by SDLRenderer for the idle-skip optimisation

    /// Returns true if any node in the subtree rooted at `start` has
    /// dirty_render == true.  O(N) walk; only called once per frame.
    [[nodiscard]] bool anyDirty(NodeHandle start) const noexcept;

    /// Set dirty_render = true on every node in the subtree rooted at `start`.
    /// Called before a full repaint so that nodes which did NOT mark themselves
    /// dirty still re-emit their draw commands into the freshly-cleared UI
    /// texture.
    void forceAllDirty(NodeHandle start) noexcept;

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
