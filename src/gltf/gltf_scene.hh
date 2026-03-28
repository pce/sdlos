#pragma once
// gltf_scene.hh — 3D render layer for sdlos.
//
// GltfScene loads glTF files and injects LayoutKind::None proxy nodes
// (tag "object3d") into the RenderTree for every mesh primitive.  The proxy
// nodes sit in the same tree as any other RenderNode regardless of which input
// syntax (jade, XML, HTML, …) built that tree.  Their projected screen-space
// AABB is written into x/y/w/h so css::StyleSheet::tickHover() and hitTest()
// work without any 3D-specific logic.
//
// scene3d node (source syntax-agnostic):
//   Any RenderNode with style("tag") == "scene3d" and a src= attribute is
//   recognised by attach().  Example in a jade-like syntax:
//     scene3d(src="altar.glb" mesh-id="altar")
//   The mesh-id attribute overrides the proxy node id for the first mesh:
//     scene3d(src="Ball.glb" mesh-id="ball")  →  proxy id = "ball"
//
// Blender naming convention (fallback when mesh-id is absent):
//   Object "Coin Slot"   → id="coin-slot"
//   Material "Metal Dark" → class token "metal-dark"
//
// Style-driven transform properties (read by buildModelMatrix() each frame)
// ──────────────────────────────────────────────────────────────────────────
// Set via a named "map CSS" file (applyMapCSS / css::load + applyTo) or by
// calling RenderNode::setStyle() directly for physics-driven objects.
//
//   --scale          → uniform scale applied to all three axes
//   --scale-x/y/z    → per-axis scale; each falls back to --scale when absent
//   --translate-x/y/z→ world-space translation (pixels in ortho, metres in persp)
//   --rotation-x/y/z → Euler rotation in degrees, applied in X → Y → Z order
//
// When any transform property is present the CSS TRS is composed on top of
// the glTF world matrix (CSS_TRS * world_mat), preserving the model's native
// scale — including Blender's unit-scale correction (×0.01 for cm exports).
// When no CSS transform props are set world_mat is used directly.
//
// Scene visibility
// ──────────────────────────────────────────────────────────────────────────
// Setting display="none" on a scene3d RenderNode hides all of its mesh
// proxies from both rendering (render()) and AABB projection (tick()).

#include "../render_tree.hh"
#include "math3d.hh"

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace pce::sdlos::gltf {

struct GltfCamera {
    float view[16];
    float proj[16];
    float pos[3];
    float vw = 1.f, vh = 1.f;

    GltfCamera() noexcept;

    void lookAt(float ex, float ey, float ez,
                float cx, float cy, float cz) noexcept;

    void perspective(float fov_y_deg, float aspect,
                     float near_z = 0.1f, float far_z = 500.f) noexcept;

    void setViewport(float w, float h) noexcept;
};

struct GpuMesh {
    SDL_GPUBuffer* vertex_buf  = nullptr;
    SDL_GPUBuffer* index_buf   = nullptr;
    Uint32         index_count = 0;
    float          aabb_min[3]{};
    float          aabb_max[3]{};

    [[nodiscard]] bool valid() const noexcept
    {
        return vertex_buf && index_buf && index_count > 0;
    }
};

// 32 bytes, matches pbr_mesh.vert.metal vertex descriptor.
struct GpuVertex {
    float px, py, pz;   // offset  0
    float nx, ny, nz;   // offset 12
    float u, v;         // offset 24
};
static_assert(sizeof(GpuVertex) == 32);

// Vertex uniform slot 0.
struct alignas(16) VertPush {
    float mvp[16];    // proj * view * model
    float model[16];
};
static_assert(sizeof(VertPush) == 128);

// Fragment uniform slot 0.
struct alignas(16) FragPush {
    float base_color[4];  // CSS 'color'
    float emissive[4];    // CSS '--emissive' rgb + intensity (a)
    float light_dir_i[4]; // xyz = world dir, w = intensity
    float light_color[4]; // rgb + 0
    float cam_pos[4];     // xyz + 0
    float roughness;      // CSS '--roughness'
    float metallic;       // CSS '--metallic'
    float hover_t;        // 1.0 when border-width > 0
    float opacity;        // CSS 'opacity'
};
static_assert(sizeof(FragPush) == 96);

class GltfScene {
public:
    GltfScene()  = default;
    ~GltfScene() { shutdown(); }

    GltfScene(const GltfScene&)            = delete;
    GltfScene& operator=(const GltfScene&) = delete;

    // init() — call once after SDLRenderer::Initialize().
    // Shaders are resolved from base_path + "data/shaders/msl/pbr_mesh.*".
    bool init(SDL_GPUDevice*       device,
              SDL_GPUShaderFormat  fmt,
              const std::string&   base_path,
              SDL_GPUTextureFormat swapchain_fmt) noexcept;

    // Scan the RenderTree for nodes with tag="scene3d", load their src= files,
    // and inject object3d proxy children (one per mesh primitive).
    // Returns the total number of mesh primitives uploaded to the GPU.
    int attach(RenderTree& tree, NodeHandle root, const std::string& base_path);

    // Mark all proxy RenderNodes dirty so draw callbacks re-read StyleMap.
    // Call after css.applyTo().
    void applyCSS(RenderTree& tree) noexcept;

    // Convenience: load a named CSS layout file and apply it to the tree.
    // The CSS may contain --scale, --translate-x/y/z, --rotation-x/y/z and
    // any other style properties consumed by drawEntry().  Call after attach()
    // so the injected proxy nodes are present in the RenderTree.
    // Returns false when the file cannot be opened or produces no rules.
    bool applyMapCSS(RenderTree& tree, NodeHandle root,
                     const std::string& path) noexcept;

    // Project world AABBs to screen and write x/y/w/h on each proxy node.
    // Call before css.tickHover() each frame.
    void tick(RenderTree& tree, float vw, float vh) noexcept;

    // Draw all meshes in a single render pass (LOADOP_LOAD on color_target).
    // Call before the jade 2D pass each frame.
    void render(SDL_GPUCommandBuffer* cmd,
                SDL_GPUTexture*       color_target,
                float vw, float vh) noexcept;

    void shutdown() noexcept;

    [[nodiscard]] GltfCamera&       camera()     noexcept { return camera_; }
    [[nodiscard]] const GltfCamera& camera() const noexcept { return camera_; }
    [[nodiscard]] bool              ready()  const noexcept { return pipeline_ != nullptr; }
    [[nodiscard]] std::size_t       meshCount() const noexcept { return entries_.size(); }

private:
    struct MeshEntry {
        GpuMesh    gpu;
        float      world_mat[16];
        NodeHandle proxy_handle;     ///< object3d proxy RenderNode (style reads)
        NodeHandle scene3d_handle;   ///< parent scene3d RenderNode (display:none)
    };

    void drawEntry(SDL_GPURenderPass*    pass,
                   SDL_GPUCommandBuffer* cmd,
                   const MeshEntry&      e,
                   RenderTree&           tree,
                   float vw, float vh) noexcept;

    // Build the model matrix for an entry.
    // Reads --scale[-x/y/z], --translate-x/y/z, --rotation-x/y/z from the
    // proxy StyleMap.  Result: T(css) * Rz * Ry * Rx * S(css) * world_mat.
    //
    //   --scale: 1        natural glTF size (world_mat scale preserved)
    //   --scale: 2        doubles from natural glTF size
    //   --translate-x: 3  3 world units from the model's glTF origin
    //   (no CSS props)    world_mat used directly; has_style == false
    //
    // Used in both tick() (AABB projection) and drawEntry() (GPU draw).
    static math3d::Mat4 buildModelMatrix(const MeshEntry& e, RenderTree& tree) noexcept;

    bool createPipeline(SDL_GPUTextureFormat swapchain_fmt) noexcept;
    bool createOrResizeDepth(Uint32 w, Uint32 h) noexcept;

    // Returns {1,1,1,1} for empty or malformed input.
    static void parseHexColor(std::string_view hex, float (&out)[4]) noexcept;
    static float parseFloat(std::string_view s, float fallback) noexcept;

    // Lowercase, spaces/underscores → hyphens, strip non-alphanumeric.
    static std::string normalizeId(std::string_view s) noexcept;

    bool loadFile(const std::filesystem::path& path,
                  RenderTree& tree, NodeHandle parent_handle);

    SDL_GPUDevice*           device_   = nullptr;
    SDL_GPUShaderFormat      fmt_      = SDL_GPU_SHADERFORMAT_INVALID;
    std::string              base_;
    SDL_GPUShader*           vert_     = nullptr;
    SDL_GPUShader*           frag_     = nullptr;
    SDL_GPUGraphicsPipeline* pipeline_ = nullptr;
    SDL_GPUTexture*          depth_    = nullptr;
    Uint32                   depth_w_  = 0;
    Uint32                   depth_h_  = 0;

    std::vector<MeshEntry>   entries_;
    GltfCamera               camera_;
    RenderTree*              tree_ref_ = nullptr;

    struct Light {
        float dir[3]    = {-0.4f, -0.9f, -0.2f};
        float color[3]  = { 1.0f,  0.97f, 0.9f};
        float intensity = 2.f;
    } light_;
};

} // namespace pce::sdlos::gltf
