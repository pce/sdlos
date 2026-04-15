#pragma once


#include "core/animated.h"
#include "core/frame_arena.h"
#include "core/slot_map.h"
#include "core/small_flat_map.h"
#include "core/rgba.h"

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

using NodeHandle                          = core::SlotID;
inline constexpr NodeHandle k_null_handle = core::k_null_slot;

enum class GPUBackend : uint8_t {
    Metal,  // MSL — macOS / iOS
    SpirV,  // GLSL → .spv — Vulkan / Linux
};

enum class LayoutKind : uint8_t {
    None,        // absolute / caller-managed (default)
    Block,       // vertical stack
    FlexRow,     // flex, main axis = X
    FlexColumn,  // flex, main axis = Y
    Grid,        // reserved — Phase 3
};

// LayoutProps  —  hints consumed by resolveLayout().
//
//   As an item       : width, height, flex_grow, flex_shrink, flex_basis
//   As a container   : gap, justify, align
//   -1.f             : auto (use intrinsic size / stretch)

struct LayoutProps {
    float width  = -1.f;
    float height = -1.f;

    float flex_grow   = 0.f;
    float flex_shrink = 1.f;
    float flex_basis  = -1.f;  // -1 → use width / height

    // Percent-of-parent sizing (resolved during layout; takes priority over
    // explicit width/height when >= 0).
    float width_pct  = -1.f;  // -1 = not set; 0..100 = % of parent dimension
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
        Scroll,  // reserved
    } overflow = Overflow::Visible;

    enum class Justify : uint8_t {
        Start,
        Center,
        End,
        SpaceBetween,
        SpaceAround,
    } justify = Justify::Start;

    enum class Align : uint8_t {
        Start,
        Center,
        End,
        Stretch,  // fills cross-axis when size is auto (default)
    } align = Align::Stretch;
};

struct VisualProps {
    // CSS `direction` — controls text shaping direction and FlexRow/Block item
    // placement.  LTR is the default.  Set `direction: rtl` in CSS or via
    // setStyle("direction","rtl") for Arabic, Hebrew and other RTL scripts.
    //
    // Container (FlexRow + direction:rtl): children are placed right→left so
    //   the first child sits on the RIGHT edge of the container.
    // Block + direction:rtl: fixed-width children are right-aligned.
    // Text node + direction:rtl: SDL_ttf/HarfBuzz shapes the glyphs RTL;
    //   the default textAlign becomes Right (unless overridden).
    enum class Direction : uint8_t {
        LTR,
        RTL
    } direction = Direction::LTR;

    enum class TextAlign : uint8_t {
        Left,
        Center,  // default — preserves the existing centred-text behaviour
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
    //   Fill     - stretch to exactly w×h (default; may distort).
    //   Contain  - scale uniformly to fit inside w×h; letterbox empty space.
    //   Cover    - scale to fill w×h, UV-crop center
    enum class ObjectFit : uint8_t {
        Fill,     // stretch to w×h (default)
        Contain,  // scale to fit inside w×h, letterbox
        Cover,    // scale to fill w×h, UV-crop center
    } object_fit = ObjectFit::Fill;
};

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
//
/// TODO SoC: RenderTree GodObject Resolution
/// TODO atm Populated by style_draw.cc from _shader_param0/1/2, _shader_focusX/Y attrs.
/// RGBA colour value used by RenderContext::drawRoundedRect.
/// Defined here (not in style_draw.cc) so render_tree.cc can access members.



struct NodeShaderParams {
    float u_width;   // node width  in physical pixels
    float u_height;  // node height in physical pixels
    float u_focusX;  // normalised focus point X [0..1]  (default 0.5)
    float u_focusY;  // normalised focus point Y [0..1]  (default 0.5)
    float u_param0;  // shader-defined  (blurScale, contrast, etc.)
    float u_param1;  // shader-defined
    float u_param2;  // shader-defined
    float u_time;    // seconds since start (jitter, animation)
};
static_assert(sizeof(NodeShaderParams) == 32, "NodeShaderParams must be 32 bytes");

struct RenderContext {
    GPUBackend backend        = GPUBackend::Metal;
    SDL_GPUDevice *device     = nullptr;
    SDL_GPUCommandBuffer *cmd = nullptr;
    SDL_GPURenderPass *pass   = nullptr;

    float viewport_w = 0.f;  // physical pixels — from swapchain
    float viewport_h = 0.f;

    core::small_flat_map<std::string, SDL_GPUGraphicsPipeline *, 8> pipelines;

    core::frame_arena *arena    = nullptr;
    TextRenderer *text_renderer = nullptr;  // null → drawText no-ops
    ImageCache *image_cache     = nullptr;  // null → drawImage no-ops
    VideoTexture *video_texture = nullptr;  ///< null → drawVideo is a no-op

    // Seconds since app start — forwarded to _shader fragment uniforms
    // (u_time) so shaders can use it for jitter, animation, etc.
    float time = 0.f;

    // Absolute screen-space origin of the node currently being drawn.
    // Set by RenderTree::render() before each draw() call by accumulating
    // parent-relative x/y down the traversal stack.
    // Draw callbacks must use (offset_x + local_x, offset_y + local_y) for
    // all drawRect / drawText / drawImage calls.
    float offset_x = 0.f;
    float offset_y = 0.f;

    // Callback that returns (or lazily compiles) a named node shader pipeline.
    // Set by SDLRenderer::Render(); null means _shader nodes fall back to
    // regular drawImage.
    std::function<SDL_GPUGraphicsPipeline *(std::string_view)> nodeShaderPipeline;

    /**
     * @brief Draws rect
     *
     * @param x  Horizontal coordinate in logical pixels
     * @param y  Vertical coordinate in logical pixels
     * @param w  Width in logical pixels
     * @param h  Opaque resource handle
     * @param r  Red channel component [0, 1]
     * @param g  Green channel component [0, 1]
     * @param b  Blue channel component [0, 1]
     * @param a  Alpha channel component [0, 1]
     */
    void drawRect(float x, float y, float w, float h, float r, float g, float b, float a = 1.f);

    /**
     * @brief Draws text
     *
     * `rtl` enables HarfBuzz RTL shaping via TTF_SetFontDirection(RTL).
     * Pass `vp.direction == VisualProps::Direction::RTL` from draw callbacks.
     * drawRect — no vertex buffer; ui_rect.vert generates a 6-vertex CCW quad
     * entirely from push-uniform data.
     *
     * Push uniform layout (must match shader structs):
     * slot 0, vertex stage   → {x, y, w, h, viewport_w, viewport_h, _pad, _pad}
     * slot 0, fragment stage → {r, g, b, a}
     *
     * @param text  UTF-8 text content
     * @param x     Horizontal coordinate in logical pixels
     * @param y     Vertical coordinate in logical pixels
     * @param size  Font size in points
     * @param r     Red channel component [0, 1]
     * @param g     Green channel component [0, 1]
     * @param b     Blue channel component [0, 1]
     * @param a     Alpha channel component [0, 1]
     * @param rtl   Enable right-to-left HarfBuzz text shaping; pass
     *              `vp.direction == VisualProps::Direction::RTL` (default: false)
     */
    void drawText(std::string_view text, float x, float y, float size, float r, float g, float b, float a = 1.f, bool rtl = false);

    void drawImage(std::string_view src, float x, float y, float w, float h, float opacity, VisualProps::ObjectFit fit);
    void drawImageWithShader(std::string_view src, std::string_view shader_name, float x, float y, float w, float h, float opacity, VisualProps::ObjectFit fit, const struct NodeShaderParams &shader_params);
    void drawVideo(float x, float y, float w, float h, float opacity);
    void drawVideoWithShader(std::string_view shader_name, float x, float y, float w, float h, float opacity, const struct NodeShaderParams &shader_params);
    void drawRoundedRect(float x, float y, float w, float h, float radius, float bw, const struct RGBAf &fill, const struct RGBAf &border);
    void drawRectOutline(float x, float y, float w, float h, float thickness, float r, float g, float b, float a);
    SDL_GPUGraphicsPipeline *pipeline(std::string_view name);
};

struct RenderNode {
    NodeHandle handle  = k_null_handle;
    NodeHandle parent  = k_null_handle;
    NodeHandle child   = k_null_handle;  // standard LCRS (left-child, right-sibling) tree
    NodeHandle sibling = k_null_handle;

    LayoutKind layout_kind = LayoutKind::None;
    LayoutProps layout_props;
    VisualProps visual_props;

    // x, y, w, h: physical pixels (resolved by RenderTree::update)
    float x = 0.f;
    float y = 0.f;
    float w = 0.f;
    float h = 0.f;

    bool hidden = false;

    // Per-node alpha multiplier applied to all draw calls on this node.
    // Set by StyleApplier::apply() from the `opacity` CSS attribute.
    // Range [0, 1]; 1 = fully opaque (default), 0 = fully transparent.
    float opacity = 1.f;

    // Type-erased state container for UI widgets.
    std::any state;

    // style_attrs — raw Jade attributes (e.g. "class", "id", "gap")
    core::small_flat_map<std::string, std::string, 8> style_attrs;

    // transient state; move to RenderContext?
    bool dirty_render = true;  ///< repaint this node only
    bool dirty_layout = true;  ///< redo layout for this node's subtree

    std::function<void(RenderContext &)> draw;
    std::function<void(RenderContext &)> after_draw;  ///< Post-children hook
    std::function<void()> update;                     ///< Behavior tick

    /**
     * @brief Sets Style attribute — replaces existing value if key matches.
     *
     * @param key    Human-readable name or identifier string
     * @param value  Operand value
     */
    void setStyle(std::string_view key, std::string_view value) {
        for (auto &[k, v] : style_attrs) {
            if (k == key) {
                v = value;
                return;
            }
        }
        style_attrs.insert({std::string(key), std::string(value)});
    }

    /**
     * @brief Has style
     *
     * @param key  Human-readable name or identifier string
     *
     * @return true on success, false on failure
     */
    bool hasStyle(std::string_view key) const noexcept {
        for (const auto &[k, v] : style_attrs) {
            if (k == key)
                return true;
        }
        return false;
    }

    // Convenience property accessors. (O(n) but n is small ~4-10)
    std::string_view style(std::string_view key) const noexcept {
        for (const auto &[k, v] : style_attrs) {
            if (k == key)
                return v;
        }
        return "";
    }
};

class RenderTree {
  public:
    explicit RenderTree(std::size_t arena_size = 1024 * 1024);

    NodeHandle alloc();
    void free(NodeHandle handle);

    core::frame_arena &arena() noexcept { return arena_; }
    const core::frame_arena &arena() const noexcept { return arena_; }

    RenderNode *node(NodeHandle handle);
    const RenderNode *node(NodeHandle handle) const;

    std::size_t nodeCount() const noexcept { return nodes_.size(); }

    void appendChild(NodeHandle parent, NodeHandle child);
    void appendChildren(NodeHandle parent, std::span<const NodeHandle> children);
    void detach(NodeHandle child);

    void markDirty(NodeHandle handle);
    void markLayoutDirty(NodeHandle handle);

    bool anyDirty(NodeHandle start) const noexcept;
    void forceAllDirty(NodeHandle start) noexcept;

    NodeHandle findById(NodeHandle start, std::string_view id) const;
    std::vector<NodeHandle> findByClass(NodeHandle start, std::string_view cls) const;

    template <std::invocable<NodeHandle, RenderNode &> Fn>
    void traverse(NodeHandle start, Fn &&fn);

    void beginFrame();
    void update(NodeHandle root = k_null_handle);
    void render(NodeHandle root, RenderContext &ctx);

    void setRoot(NodeHandle root) { root_ = root; }
    NodeHandle root() const noexcept { return root_; }

  private:
    void resolveLayout(NodeHandle h, RenderNode &n);

    core::slot_map<RenderNode> nodes_;
    core::frame_arena arena_;
    NodeHandle root_ = k_null_handle;
};

}  // namespace pce::sdlos
