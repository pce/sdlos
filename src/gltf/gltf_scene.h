#pragma once
// gltf_scene.h — 3D render layer for sdlos.
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
//
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
//
// Setting display="none" on a scene3d RenderNode hides all of its mesh
// proxies from both rendering (render()) and AABB projection (tick()).

#include "../render_tree.h"
#include "math3d.h"

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

    // ── Orbit camera state ───────────────────────────────────────────────────
    // Used only when the orbit*() methods are called.  lookAt() / perspective()
    // still work independently as an escape hatch.
    float orbit_yaw_deg   = 30.f;             ///< horizontal angle (degrees), +CCW from +Z
    float orbit_pitch_deg = 20.f;             ///< vertical angle (degrees), clamped ±89°
    float orbit_dist      = 5.f;              ///< distance from target
    float orbit_target[3] = {0.f, 0.f, 0.f};  ///< world-space pivot

    /**
     * @brief Gltf camera
     */
    GltfCamera() noexcept;

    /**
     * @brief Look at
     *
     * @param ex  Horizontal coordinate in logical pixels
     * @param ey  Vertical coordinate in logical pixels
     * @param ez  Depth coordinate
     * @param cx  Horizontal coordinate in logical pixels
     * @param cy  Vertical coordinate in logical pixels
     * @param cz  Depth coordinate
     */
    void lookAt(float ex, float ey, float ez, float cx, float cy, float cz) noexcept;

    /**
     * @brief Perspective
     *
     * @param fov_y_deg  Vertical coordinate in logical pixels
     * @param aspect     Alpha channel component [0, 1]
     * @param near_z     Depth coordinate
     * @param far_z      Depth coordinate
     */
    void
    perspective(float fov_y_deg, float aspect, float near_z = 0.1f, float far_z = 500.f) noexcept;

    /**
     * @brief Sets viewport
     *
     * @param w  Width in logical pixels
     * @param h  Opaque resource handle
     */
    void setViewport(float w, float h) noexcept;

    // ── Orbit API ────────────────────────────────────────────────────────────

    /** Set the world-space point the camera orbits around.
     *  Defaults to origin (0, 0, 0).  Triggers a view-matrix update. */
    void setOrbitTarget(float tx, float ty, float tz) noexcept;

    /** Set absolute orbit angles and distance, then recompute the view matrix.
     *
     *  Typical usage (from a behavior or the host auto-wire):
     *    camera.orbit(30.f, 20.f, 5.f);   // yaw=30°, pitch=20°, dist=5 units
     */
    void orbit(float yaw_deg, float pitch_deg, float dist) noexcept;

    /** Increment yaw and pitch by the given deltas, then recompute.
     *  Pitch is automatically clamped to ±89° to prevent gimbal flip.
     *
     *  Typical usage (mouse drag in a behavior):
     *    camera.orbitBy(dx * sensitivity, -dy * sensitivity);
     */
    void orbitBy(float dyaw, float dpitch) noexcept;

    /** Multiply the orbit distance by @p factor, clamped to [min_dist, max_dist].
     *
     *  Typical usage (mouse scroll):
     *    camera.orbitZoom(1.f - scroll_y * 0.1f);
     */
    void orbitZoom(float factor, float min_dist = 0.5f, float max_dist = 500.f) noexcept;

    /** Recompute the view matrix from the current orbit state.
     *  Called automatically by all orbit*() methods; exposed so behaviors can
     *  mutate the raw fields and call this once at the end of a frame. */
    void updateOrbit() noexcept;
};

struct GpuMesh {
    SDL_GPUBuffer *vertex_buf = nullptr;
    SDL_GPUBuffer *index_buf  = nullptr;
    Uint32 index_count        = 0;
    float aabb_min[3]{};
    float aabb_max[3]{};

    /**
     * @brief Valid
     *
     * @return true on success, false on failure
     */
    [[nodiscard]]
    bool valid() const noexcept {
        return vertex_buf && index_buf && index_count > 0;
    }
};

// 32 bytes, matches pbr_mesh.vert.metal vertex descriptor.
struct GpuVertex {
    float px, py, pz;  // offset  0
    float nx, ny, nz;  // offset 12
    float u, v;        // offset 24
};
static_assert(sizeof(GpuVertex) == 32);

// Vertex uniform slot 0.
struct alignas(16) VertPush {
    float mvp[16];  // proj * view * model
    float model[16];
};
static_assert(sizeof(VertPush) == 128);

// Fragment uniform slot 0.
struct alignas(16) FragPush {
    float base_color[4];   // CSS 'color'
    float emissive[4];     // CSS '--emissive' rgb + intensity (a)
    float light_dir_i[4];  // xyz = world dir, w = intensity
    float light_color[4];  // rgb + 0
    float cam_pos[4];      // xyz + 0
    float roughness;       // CSS '--roughness'
    float metallic;        // CSS '--metallic'
    float hover_t;         // 1.0 when border-width > 0
    float opacity;         // CSS 'opacity'
};
static_assert(sizeof(FragPush) == 96);

class GltfScene {
  public:
    /**
     * @brief Gltf scene
     */
    GltfScene() = default;
    /**
     * @brief ~gltf scene
     */
    ~GltfScene() { shutdown(); }

    /**
     * @brief Gltf scene
     *
     * @param param0  Red channel component [0, 1]
     */
    GltfScene(const GltfScene &)            = delete;
    GltfScene &operator=(const GltfScene &) = delete;

    // init() — call once after SDLRenderer::Initialize().
    // Shaders are resolved from base_path + "data/shaders/msl/pbr_mesh.*".
    /**
     * @brief Initialises
     *
     * @param device         SDL3 GPU device handle
     * @param fmt            printf-style format string
     * @param base_path      Filesystem path
     * @param swapchain_fmt  printf-style format string
     *
     * @return true on success, false on failure
     */
    bool init(
        SDL_GPUDevice *device,
        SDL_GPUShaderFormat fmt,
        const std::string &base_path,
        SDL_GPUTextureFormat swapchain_fmt) noexcept;

    // Scan the RenderTree for nodes with tag="scene3d", load their src= files,
    // and inject object3d proxy children (one per mesh primitive).
    // Returns the total number of mesh primitives uploaded to the GPU.
    /**
     * @brief Attaches
     *
     * @param tree       Red channel component [0, 1]
     * @param root       Red channel component [0, 1]
     * @param base_path  Filesystem path
     *
     * @return Integer result; negative values indicate an error code
     */
    int attach(RenderTree &tree, NodeHandle root, const std::string &base_path);

    // Release all loaded mesh primitives and remove their proxy RenderNodes
    // from *tree*.  The GPU pipeline, shaders, and depth texture are kept so
    // a subsequent attach() can reuse them without re-compiling shaders.
    //
    // Call this before re-attach()ing a different GLTF to the same scene3d
    // node (e.g. swapping city meshes in a weather app).
    //
    // Safe to call when entries_ is already empty (no-op).
    /**
     * @brief Clears all loaded mesh entries and removes their proxy RenderNodes.
     *
     * The GPU pipeline, depth texture, and compiled shaders are preserved so
     * a subsequent attach() can reuse them without re-initialising.
     *
     * @param tree  RenderTree that holds the proxy nodes to remove.
     */
    void clearMeshes(RenderTree &tree) noexcept;

    // Reload the GLB for a single scene3d node without disturbing other
    // entries in the scene.  Targeted version of clearMeshes+attach:
    //   1. Finds and releases all entries whose scene3d_handle == scene3d_node.
    //   2. Reads the current src= attribute on that node.
    //   3. Resolves the path via resolveGltfPath and calls loadFile().
    // Returns the number of new primitives loaded, or -1 on failure.
    // Call this when swapping models at runtime (e.g. animal carousel).
    /**
     * @brief Reload the GLB for one scene3d node, leaving others untouched.
     *
     * @param tree          RenderTree that owns the proxy nodes.
     * @param scene3d_node  Handle of the scene3d node whose src= has changed.
     * @param base_path     Base directory used by resolveGltfPath() (SDL_GetBasePath()).
     * @return Number of primitives loaded from the new GLB, or -1 on error.
     */
    int
    reloadNode(RenderTree &tree, NodeHandle scene3d_node, const std::string &base_path) noexcept;

    // Mark all proxy RenderNodes dirty so draw callbacks re-read StyleMap.
    // Call after css.applyTo().
    /**
     * @brief Applies css
     *
     * @param tree  Red channel component [0, 1]
     */
    void applyCSS(RenderTree &tree) noexcept;

    // Convenience: load a named CSS layout file and apply it to the tree.
    // The CSS may contain --scale, --translate-x/y/z, --rotation-x/y/z and
    // any other style properties consumed by drawEntry().  Call after attach()
    // so the injected proxy nodes are present in the RenderTree.
    // Returns false when the file cannot be opened or produces no rules.
    /**
     * @brief Applies map css
     *
     * @param tree  Red channel component [0, 1]
     * @param root  Red channel component [0, 1]
     * @param path  Filesystem path
     *
     * @return true on success, false on failure
     */
    bool applyMapCSS(RenderTree &tree, NodeHandle root, const std::string &path) noexcept;

    // Project world AABBs to screen and write x/y/w/h on each proxy node.
    // Call before css.tickHover() each frame.
    /**
     * @brief Ticks one simulation frame for
     *
     * @param tree  Red channel component [0, 1]
     * @param vw    Width in logical pixels
     * @param vh    Opaque resource handle
     */
    void tick(RenderTree &tree, float vw, float vh) noexcept;

    // Draw all meshes in a single render pass (LOADOP_LOAD on color_target).
    // Call before the jade 2D pass each frame.
    /**
     * @brief Renders
     *
     * @param cmd           SDL_GPUCommandBuffer * value
     * @param color_target  RGBA colour value
     * @param vw            Width in logical pixels
     * @param vh            Opaque resource handle
     */
    void
    render(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *color_target, float vw, float vh) noexcept;

    /**
     * @brief Shuts down
     */
    void shutdown() noexcept;

    // Use a reference-counted handle for the device so child scenes
    // can safely reach the GPU device even if the renderer was destroyed.
    // In SDL3, SDL_GPUDevice is usually managed by the application.
    /**
     * @brief Sets device
     *
     * @param device  SDL3 GPU device handle
     */
    void setDevice(SDL_GPUDevice *device) noexcept { device_ = device; }

    /**
     * @brief Mesh count
     *
     * @return Integer result; negative values indicate an error code
     */
    /**
     * @brief Reads y
     *
     * @return true on success, false on failure
     */
    /**
     * @brief Camera
     *
     * @return Reference to the result
     */
    /**
     * @brief Camera
     *
     * @return Reference to the result
     */
    /**
     * @brief Mesh count
     *
     * @return Integer result; negative values indicate an error code
     */
    /**
     * @brief Reads y
     *
     * @return true on success, false on failure
     */
    /**
     * @brief Camera
     *
     * @return Reference to the result
     */
    /**
     * @brief Mesh count
     *
     * @return Integer result; negative values indicate an error code
     */
    /**
     * @brief Reads y
     *
     * @return true on success, false on failure
     */
    /**
     * @brief Camera
     *
     * @return Reference to the result
     */
    /**
     * @brief Mesh count
     *
     * @return Integer result; negative values indicate an error code
     */
    /**
     * @brief Reads y
     *
     * @return true on success, false on failure
     */
    /**
     * @brief Camera
     *
     * @return Reference to the result
     */
    /**
     * @brief Mesh count
     *
     * @return Integer result; negative values indicate an error code
     */
    /**
     * @brief Reads y
     *
     * @return true on success, false on failure
     */
    /**
     * @brief Camera
     *
     * @return Reference to the result
     */

    /**
     * @brief Camera
     *
     * @return Reference to the result
     */
    /**
     * @brief Camera
     *
     * @return Reference to the result
     */
    /**
     * @brief Camera
     *
     * @return Reference to the result
     */
    /**
     * @brief Camera
     *
     * @return Reference to the result
     */
    GltfCamera &camera() noexcept { return camera_; }
    /**
     * @brief Camera
     *
     * @return Reference to the result
     */
    const GltfCamera &camera() const noexcept { return camera_; }
    /**
     * @brief Reads y
     *
     * @return true on success, false on failure
     */

    bool ready() const noexcept { return pipeline_ != nullptr; }
    /**
     * @brief Mesh count
     *
     * @return Integer result; negative values indicate an error code
     */
    std::size_t meshCount() const noexcept { return entries_.size(); }

    // Override the scene directional light.
    //   dir       — direction the light travels (toward the surface, world space).
    //               Normalised internally. Default: oblique SW sun at ~42° elevation.
    //   color_rgb — linear RGB tint for the directional beam (default warm white).
    //   intensity — multiplier applied to diffuse + specular (default 2.2).
    void setLight(
        const float dir[3],
        const float color_rgb[3] = nullptr,
        float intensity          = -1.f) noexcept {
        light_.dir[0] = dir[0];
        light_.dir[1] = dir[1];
        light_.dir[2] = dir[2];
        if (color_rgb) {
            light_.color[0] = color_rgb[0];
            light_.color[1] = color_rgb[1];
            light_.color[2] = color_rgb[2];
        }
        if (intensity >= 0.f)
            light_.intensity = intensity;
    }

  private:
    struct MeshEntry {
        GpuMesh gpu;
        float world_mat[16];
        NodeHandle proxy_handle;    ///< object3d proxy RenderNode (style reads)
        NodeHandle scene3d_handle;  ///< parent scene3d RenderNode (display:none)
    };

    /**
     * @brief Draws entry
     *
     * @param pass  Alpha channel component [0, 1]
     * @param cmd   SDL_GPUCommandBuffer * value
     * @param e     const MeshEntry & value
     * @param tree  Red channel component [0, 1]
     * @param vw    Width in logical pixels
     * @param vh    Opaque resource handle
     */
    void drawEntry(
        SDL_GPURenderPass *pass,
        SDL_GPUCommandBuffer *cmd,
        const MeshEntry &e,
        RenderTree &tree,
        float vw,
        float vh) noexcept;

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
    /**
     * @brief Builds model matrix
     *
     * @param e     const MeshEntry & value
     * @param tree  Red channel component [0, 1]
     *
     * @return math3d::Mat4 result
     */
    static math3d::Mat4 buildModelMatrix(const MeshEntry &e, RenderTree &tree) noexcept;

    /**
     * @brief Creates and returns pipeline
     *
     * @param swapchain_fmt  printf-style format string
     *
     * @return true on success, false on failure
     */
    bool createPipeline(SDL_GPUTextureFormat swapchain_fmt) noexcept;
    /**
     * @brief Creates and returns or resize depth
     *
     * @param w  Width in logical pixels
     * @param h  Opaque resource handle
     *
     * @return true on success, false on failure
     */
    bool createOrResizeDepth(Uint32 w, Uint32 h) noexcept;

    // Returns {1,1,1,1} for empty or malformed input.
    /**
     * @brief Parses hex color
     *
     * @param hex  Opaque resource handle
     * @param out  Output parameter written by the callee
     */
    static void parseHexColor(std::string_view hex, float (&out)[4]) noexcept;
    /**
     * @brief Parses float
     *
     * @param s         Signed 32-bit integer
     * @param fallback  Blue channel component [0, 1]
     *
     * @return Computed floating-point value
     */
    static float parseFloat(std::string_view s, float fallback) noexcept;

    // Lowercase, spaces/underscores → hyphens, strip non-alphanumeric.
    /**
     * @brief Normalize id
     *
     * @param s  Signed 32-bit integer
     *
     * @return Integer result; negative values indicate an error code
     */
    static std::string normalizeId(std::string_view s) noexcept;

    /**
     * @brief Loads file
     *
     * @param path           Filesystem path
     * @param tree           Red channel component [0, 1]
     * @param parent_handle  Opaque resource handle
     *
     * @return true on success, false on failure
     */
    bool loadFile(const std::filesystem::path &path, RenderTree &tree, NodeHandle parent_handle);

    SDL_GPUDevice *device_   = nullptr;
    SDL_GPUShaderFormat fmt_ = SDL_GPU_SHADERFORMAT_INVALID;
    std::string base_;
    SDL_GPUShader *vert_               = nullptr;
    SDL_GPUShader *frag_               = nullptr;
    SDL_GPUGraphicsPipeline *pipeline_ = nullptr;
    SDL_GPUTexture *depth_             = nullptr;
    Uint32 depth_w_                    = 0;
    Uint32 depth_h_                    = 0;

    std::vector<MeshEntry> entries_;
    GltfCamera camera_;
    RenderTree *tree_ref_ = nullptr;

    struct Light {
        // Direction the light travels toward the surface (world space, need not be
        // unit length — the shader normalises).  Default: oblique SW sun at ~42°
        // elevation gives good shadow relief on vertical building facades vs. the
        // previous near-overhead 63° angle which left façades almost black.
        float dir[3]    = {-0.52f, -0.67f, -0.54f};
        float color[3]  = {1.0f, 0.97f, 0.88f};  // slightly warmer afternoon tint
        float intensity = 2.2f;                  // slightly higher to compensate
    } light_;
};

}  // namespace pce::sdlos::gltf
