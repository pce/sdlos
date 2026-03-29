#pragma once

#include "render_tree.hh"
#include "text_renderer.hh"
#include "image_cache.hh"
#include "video_texture.hh"
#include "frame_graph/frame_graph.hh"

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace pce::sdlos {

class SDLRenderer {
public:
    SDLRenderer() = default;
    ~SDLRenderer();

    // Non-copyable / non-movable: owns a GPU device tied to a specific window.
    SDLRenderer(const SDLRenderer&)            = delete;
    SDLRenderer& operator=(const SDLRenderer&) = delete;
    SDLRenderer(SDLRenderer&&)                 = delete;
    SDLRenderer& operator=(SDLRenderer&&)      = delete;

    // Lifecycle

    /// Returns false on any unrecoverable failure.
    bool Initialize(SDL_Window* window);

    /// Safe to call multiple times; called automatically by the destructor.
    void Shutdown() noexcept;

    // Per-frame

    /// `timeSeconds` forwarded to the wallpaper fragment shader uniform.
    void Render(double timeSeconds);

    // UI scene attachment

    /// `tree` is non-owning and must outlive this renderer.
    /// Pass k_null_handle to detach.
    void SetScene(RenderTree* tree,
                  NodeHandle  root = k_null_handle) noexcept;


    /// Returns true on success; the previous pipeline stays active on failure.
    bool ReloadShader(const std::string& shaderPath);

    // FrameGraph (data-driven post-process pipeline) ─────────────────────────
    //
    // An optional render pipeline loaded from a `pipeline.pug` descriptor.
    // When loaded, the FrameGraph replaces the built-in FBM wallpaper pass and
    // drives the scene through a compiled sequence of GPU render passes whose
    // structure, shader variants, and float parameters are all CSS-addressable.
    //
    // Lifecycle:
    //   1. Initialize(window)                          — creates GPU device
    //   2. SetDataBasePath(path)                       — auto-loads pipeline.pug
    //      OR LoadPipeline("data/pipeline.pug")        — explicit load
    //   3. Every frame:  Render()                      — executes compiled graph
    //   4. On CSS change: GetCompiledGraph()->apply_style(...)
    //      On theme change: GetFrameGraph()->add_class(...)
    //
    // Zombie prevention
    // -----------------
    // CompiledGraph holds raw SDL_GPUTexture* pointers into the ResourcePool.
    // After every window resize, Render() automatically calls
    // FrameGraph::resize() followed by compile() BEFORE execute() runs —
    // ensuring stale (freed) texture pointers are never accessed.
    // The fg_compiled_w_/h_ dimensions track when a recompile is needed.

    /// Load (or reload) a pipeline.pug file.
    /// The file is parsed, resources are allocated, and the compiled graph is
    /// deferred until the first Render() call when the swapchain format is known.
    /// Returns false on parse/file error; the previous pipeline stays active.
    bool LoadPipeline(std::string_view pug_path) noexcept;

    /// Non-null after a successful LoadPipeline().
    /// Use for CSS class mutations (add_class / remove_class) and wire_bus().
    [[nodiscard]] fg::FrameGraph*    GetFrameGraph()    noexcept;

    /// Non-null after the first successful compile (first Render() after load).
    /// Use for per-frame param patches (apply_style / patch / set_enabled).
    [[nodiscard]] fg::CompiledGraph* GetCompiledGraph() noexcept;

    [[nodiscard]] bool IsValid() const noexcept { return initialized_.load(); }

    [[nodiscard]] std::vector<std::string> EnumerateAdapters() const;

    // HiDPI pixel scale
    //
    // SDL mouse/touch events arrive in *logical* pixels.  The GPU swapchain
    // and all RenderNode geometry live in *physical* pixels.  Multiply
    // incoming event coordinates by these factors before hit-testing.
    //
    // Updated once during Initialize() and again whenever the caller invokes
    // RefreshPixelScale() (e.g. on SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED or
    // SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED).
    //
    // Typical values: 1.0 on standard DPI, 2.0 on Retina / HiDPI displays.

    [[nodiscard]] float pixelScaleX() const noexcept { return pixel_scale_x_; }
    [[nodiscard]] float pixelScaleY() const noexcept { return pixel_scale_y_; }

    /// Re-query SDL for the current logical→physical scale.
    /// Call this from the event loop whenever the window changes display.
    void RefreshPixelScale() noexcept;

    // Shader source path hints

    void SetVertexShaderPath(const std::string& path)   { vertex_shader_path_ = path; }
    void SetFragmentShaderPath(const std::string& path) { shader_path_        = path; }

    // GPU context access (non-owning views)

    /// Valid only after a successful Initialize().
    [[nodiscard]] SDL_GPUDevice*        GetDevice()        const noexcept { return device_;        }

    /// Shader format selected during Initialize() (MSL, SPIRV, …).
    [[nodiscard]] SDL_GPUShaderFormat   GetShaderFormat()  const noexcept { return shader_format_; }

    /// Non-null only after a successful Initialize().
    [[nodiscard]] TextRenderer*         GetTextRenderer()  const noexcept { return text_renderer_.get(); }

    /// Non-null only after a successful Initialize().
    [[nodiscard]] ImageCache*           GetImageCache()    const noexcept { return image_cache_.get();   }

    /// Non-null only after a successful Initialize().
    [[nodiscard]] VideoTexture* GetVideoTexture() const noexcept { return video_texture_.get(); }

    // Data base path
    //
    // Set once from SDL_GetBasePath() so both the ImageCache (relative src=
    // paths in jade) and the node shader loader (data/shaders/…) resolve
    // paths relative to the binary's directory — the same place CMake copies
    // each app's data/ folder as a post-build step.
    //
    // Font loading is NOT performed here — not every app ships a data/fonts/
    // directory.  Font selection is handled by loadAppFonts() in jade_host
    // (scans <jade_dir>/data/fonts/) or via the _font jade attribute / an
    // explicit SDLRenderer::SetFontPath() call.
    void SetDataBasePath(const std::string& path) noexcept;

    // Font selection
    //
    // Loads the TTF/OTF file at `path` at `pt_size` points and makes it the
    // active face for all subsequent text rendering.  Relative paths are
    // resolved against the data base path set by SetDataBasePath().
    //
    // Returns true on success; on failure the previously loaded font (if any)
    // stays active and an error is printed to stderr.
    //
    // App-level usage — call from jade_app_init() or anywhere in the host:
    //
    //   renderer.SetFontPath("data/fonts/Inter-Regular.ttf");
    //
    // Jade attribute — set `_font` on the root (or any) node and jade_host
    // will call SetFontPath() after the behaviour's jade_app_init() runs:
    //
    //   app(_font="data/fonts/Inter-Regular.ttf")
    //     ...
    //
    // An optional `_font_size` attribute (float, points) is also honoured;
    // it defaults to 17 pt when absent.
    bool SetFontPath(const std::string& path, float pt_size = 17.f) noexcept;

    // 3D scene pre-pass hook ─────────────────────────────────────────────────
    //
    // Called once per frame BEFORE the 2D UI pass, inside the same command
    // buffer that owns the swapchain acquire.
    //
    // The hook receives:
    //   cmd          — active command buffer (do NOT submit or cancel it)
    //   color_target — the acquired swapchain texture (LOADOP_LOAD preserves wallpaper)
    //   vw, vh       — physical viewport size in pixels
    //
    // The hook is responsible for creating and ending its own SDL_GPURenderPass
    // (with a depth attachment if needed). After the hook returns, the renderer
    // continues with the 2D UI pass using LOADOP_LOAD on the same color_target.
    //
    // Set by GltfScene::attach() or any other 3D subsystem. Only one hook is
    // active at a time — set to nullptr to disable.
    using Scene3DHook = std::function<void(SDL_GPUCommandBuffer* cmd,
                                            SDL_GPUTexture*       color_target,
                                            float vw, float vh)>;

    void setScene3DHook(Scene3DHook hook) noexcept { scene3d_hook_ = std::move(hook); }

    // Fired at the top of Shutdown() before the GPU device is destroyed.
    // Use to call GltfScene::shutdown() from behaviors that own GPU resources
    // in static-duration objects.  Cleared after first call.
    using GpuPreShutdownHook = std::function<void()>;

    void setGpuPreShutdownHook(GpuPreShutdownHook hook) noexcept {
        gpu_pre_shutdown_hook_ = std::move(hook);
    }

private:
    // Internal helpers

    /// Reads SDL_GetWindowSize / SDL_GetWindowSizeInPixels and updates the
    /// pixel_scale_x/y_ members.  Requires sdl_window_ to be non-null.
    void UpdatePixelScale() noexcept;

    // Internal pipeline builders

    /// Sets shader_format_ on success.
    bool CreateDeviceForWindow(SDL_Window* window);

    /// Entry-point for both stages must be `main0`.
    bool CreatePipeline(const std::string& vertSource,
                        const std::string& fragSource);

    /// Non-fatal if either pipeline fails; wallpaper still renders without UI.
    bool CreateUIPipelines();

    /// Allocate (or reallocate on resize) the persistent UI offscreen texture.
    /// Returns false if allocation fails; the previous texture is released first.
    /// Must be called after CreateDeviceForWindow() succeeds.
    bool CreateOrResizeUITexture(Uint32 w, Uint32 h) noexcept;

    // File helpers

    static std::string    ReadTextFile(const std::string& path);
    static std::uintmax_t GetFileMTime(const std::string& path);

    // Wallpaper fragment shader uniform

    struct FragmentUniform {
        float time;
        float pad[3];
    };
    static_assert(sizeof(FragmentUniform) == 16,
                  "FragmentUniform must be 16 bytes for push-uniform alignment");

private:
    // SDL GPU objects (all owned)

    SDL_GPUDevice*           device_{nullptr};

    // Wallpaper pipeline
    SDL_GPUShader*           vertex_shader_{nullptr};
    SDL_GPUShader*           fragment_shader_{nullptr};
    SDL_GPUGraphicsPipeline* pipeline_{nullptr};

    // UI pipelines (alpha-blended; built by CreateUIPipelines)
    SDL_GPUShader*           ui_rect_vert_{nullptr};   // shared between rect + text
    SDL_GPUShader*           ui_rect_frag_{nullptr};
    SDL_GPUShader*           ui_text_frag_{nullptr};
    SDL_GPUGraphicsPipeline* ui_rect_pipeline_{nullptr};
    SDL_GPUGraphicsPipeline* ui_text_pipeline_{nullptr};

    // Persistent UI offscreen texture.
    //
    // The UI scene is rendered into this texture with SDL_GPU_LOADOP_CLEAR
    // on active frames (any node dirty) and then composited over the wallpaper
    // in the swapchain render pass.  On idle frames (nothing dirty) the render
    // pass is skipped entirely and the stale texture is composited as-is —
    // zero GPU commands for a fully static UI.
    //
    // Required usage flags: COLOR_TARGET (render target) + SAMPLER (composite).
    // Format: R8G8B8A8_UNORM — independent of swapchain format.
    SDL_GPUTexture* ui_texture_{nullptr};
    Uint32          ui_texture_w_{0};
    Uint32          ui_texture_h_{0};

    // Non-owning reference; lifetime managed by Window.
    SDL_Window* sdl_window_{nullptr};

    // HiDPI scale (logical → physical pixels)
    float pixel_scale_x_{1.f};
    float pixel_scale_y_{1.f};

    // Text renderer
    std::unique_ptr<TextRenderer> text_renderer_;
    std::unique_ptr<ImageCache>   image_cache_;
    std::unique_ptr<VideoTexture> video_texture_;

    // UI scene (non-owning)
    RenderTree* scene_tree_{nullptr};
    NodeHandle  scene_root_{k_null_handle};

    // State
    std::atomic<bool>   initialized_{false};
    SDL_GPUShaderFormat shader_format_{SDL_GPU_SHADERFORMAT_INVALID};
    std::string         shader_path_;
    std::string         vertex_shader_path_;
    std::uintmax_t      shader_mtime_{0};



    struct NodeShaderEntry {
        SDL_GPUGraphicsPipeline* pipeline = nullptr;
    };

    std::unordered_map<std::string, NodeShaderEntry> node_shader_cache_;

    // Base path set from SDL_GetBasePath() in jade_host after Initialize().
    std::string data_base_path_;

    // 3D scene pre-pass hook (optional; nullptr by default)
    Scene3DHook scene3d_hook_;

    GpuPreShutdownHook gpu_pre_shutdown_hook_;

    // -------------------------------------------------------------------------
    // FrameGraph — optional data-driven render pipeline
    //
    // Ownership model:
    //   frame_graph_   — owns ResourcePool (textures) + ShaderLibrary (PSOs)
    //   compiled_graph_ — NON-OWNING view into frame_graph_'s resources;
    //                     all pointers are raw and become dangling if the
    //                     ResourcePool releases textures (e.g. on resize).
    //
    // Zombie prevention contract:
    //   Render() checks fg_compiled_w_/h_ against the current swapchain size.
    //   If they differ: frame_graph_->resize() + compile() run BEFORE execute().
    //   This guarantees compiled_graph_ always holds live pointers.
    // -------------------------------------------------------------------------
    std::optional<fg::FrameGraph> frame_graph_;
    fg::CompiledGraph             compiled_graph_;

    bool                 fg_needs_compile_{false};     ///< set by LoadPipeline
    uint32_t             fg_compiled_w_{0};            ///< swapchain w at last compile
    uint32_t             fg_compiled_h_{0};            ///< swapchain h at last compile
    SDL_GPUTextureFormat fg_swapchain_fmt_{            ///< swapchain fmt at last compile
        SDL_GPU_TEXTUREFORMAT_INVALID};

    /// Load and compile a node shader pipeline on first use; cache the result.
    /// Returns nullptr on failure (and caches the failure so it is not retried).
    [[nodiscard]] SDL_GPUGraphicsPipeline*
    ensureNodeShaderPipeline(const std::string& name) noexcept;
};

} // namespace pce::sdlos
