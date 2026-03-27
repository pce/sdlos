#pragma once

// sdl_renderer.hh — SDL3 GPU renderer for one Window.
//
// Process-isolation model
// -----------------------
// Each SDLRenderer instance creates and owns its own SDL_GPUDevice.  Every
// Window gets an independent GPU context; tearing one down releases its device,
// shaders, and pipeline without touching any other Window.
//
// Cross-platform shader backend
// -----------------------------
//   SDL_GPU_SHADERFORMAT_MSL   → Metal  (macOS / iOS)  — inline source
//   SDL_GPU_SHADERFORMAT_SPIRV → Vulkan (Linux, Windows) — pre-compiled .spv
//   SDL_GPU_SHADERFORMAT_DXIL  → D3D12  (Windows, future)
//
// CreateDeviceForWindow() probes available drivers and stores the selected
// format in shader_format_ so every subsequent pipeline creation stays
// format-agnostic from the caller's perspective.
//
// Frame pipeline (two GPU passes in one command buffer)
// ------------------------------------------------------
//   1. Copy pass  — flushes any pending text-glyph texture uploads from
//                   TextRenderer::flushUploads().  Skipped when nothing is
//                   pending.
//   2. Render pass
//       a. Wallpaper pipeline  — fullscreen FBM triangle, time uniform.
//       b. UI pipeline (opt.)  — if SetScene() was called, traverses the
//                                RenderTree and issues rect/text draw calls
//                                using the "rect" and "text" pipelines
//                                registered in RenderContext::pipelines.
//
// Usage
// -----
//   Initialize(window)              — create device, build all pipelines.
//   SetScene(tree, root)            — attach a RenderTree for UI rendering.
//   Render(timeSeconds)             — submit one full frame.
//   ReloadShader(path)              — hot-reload the wallpaper fragment shader.
//   Shutdown()                      — release all GPU resources (dtor calls this).
//   GetDevice()                     — access the owned device.
//   GetTextRenderer()               — access the TextRenderer for font queries.
//
// No try/catch.  SDL GPU calls never throw; filesystem errors are handled
// via std::error_code.  Assert on programmer bugs; return false on runtime
// failures so callers can decide how to proceed.

#include "render_tree.hh"      // RenderTree, NodeHandle, k_null_handle
#include "text_renderer.hh"    // TextRenderer

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
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

    // ---- Lifecycle -------------------------------------------------------

    /// Create the GPU device, claim the window, build all pipelines
    /// (wallpaper + ui_rect + ui_text) and initialise TextRenderer.
    /// Returns false on any unrecoverable failure.
    bool Initialize(SDL_Window* window);

    /// Release all GPU resources (pipelines, shaders, device, sampler).
    /// Safe to call multiple times; called automatically by the destructor.
    void Shutdown() noexcept;

    // ---- Per-frame -------------------------------------------------------

    /// Submit one complete frame:
    ///   copy pass  — flush pending glyph texture uploads (skipped if none).
    ///   render pass — wallpaper shader, then UI scene tree (if set).
    /// `timeSeconds` is forwarded to the wallpaper fragment shader uniform.
    void Render(double timeSeconds);

    // ---- UI scene attachment ---------------------------------------------

    /// Attach a RenderTree subtree for overlay UI rendering.
    /// `tree`  — non-owning; must outlive this renderer.
    /// `root`  — root handle inside `tree`; pass k_null_handle to detach.
    /// The tree's update() / render() are driven inside Render() after the
    /// wallpaper pass, within the same SDL_GPURenderPass.
    void SetScene(RenderTree* tree,
                  NodeHandle  root = k_null_handle) noexcept;

    // ---- Shader hot-reload -----------------------------------------------

    /// Reload the wallpaper fragment shader from disk and rebuild the pipeline.
    /// Returns true on success; the previous pipeline stays active on failure.
    bool ReloadShader(const std::string& shaderPath);

    // ---- Queries ---------------------------------------------------------

    [[nodiscard]] bool IsValid() const noexcept { return initialized_.load(); }

    /// Enumerate SDL GPU driver names available on this machine.
    [[nodiscard]] std::vector<std::string> EnumerateAdapters() const;

    // ---- Shader source path hints ----------------------------------------

    void SetVertexShaderPath(const std::string& path)   { vertex_shader_path_ = path; }
    void SetFragmentShaderPath(const std::string& path) { shader_path_        = path; }

    // ---- GPU context access (non-owning views) ---------------------------

    /// The SDL_GPUDevice owned by this renderer.
    /// Valid only after a successful Initialize(); lifetime tied to this instance.
    [[nodiscard]] SDL_GPUDevice*        GetDevice()        const noexcept { return device_;        }

    /// The shader format selected during Initialize() (MSL, SPIRV, …).
    [[nodiscard]] SDL_GPUShaderFormat   GetShaderFormat()  const noexcept { return shader_format_; }

    /// The TextRenderer that backs ctx.drawText() for this window.
    /// Non-null only after a successful Initialize().
    [[nodiscard]] TextRenderer*  GetTextRenderer()  const noexcept { return text_renderer_.get(); }

private:
    // ---- Internal pipeline builders -------------------------------------

    /// Probe available SDL GPU drivers; create device; claim window.
    /// Sets shader_format_ on success.
    bool CreateDeviceForWindow(SDL_Window* window);

    /// Build the wallpaper graphics pipeline from MSL/GLSL source strings.
    /// Entry-point for both stages must be `main0`.
    bool CreatePipeline(const std::string& vertSource,
                        const std::string& fragSource);

    /// Build the ui_rect and ui_text pipelines.
    /// Called from Initialize() after the wallpaper pipeline succeeds.
    /// Returns false if either pipeline cannot be created (non-fatal: UI
    /// rendering is disabled but the wallpaper still works).
    bool CreateUIPipelines();

    // ---- File helpers ---------------------------------------------------

    static std::string    ReadTextFile(const std::string& path);
    static std::uintmax_t GetFileMTime(const std::string& path);

    // ---- Uniform layout pushed to the wallpaper fragment shader ---------

    struct FragmentUniform {
        float time;
        float pad[3];
    };
    static_assert(sizeof(FragmentUniform) == 16,
                  "FragmentUniform must be 16 bytes for push-uniform alignment");

private:
    // ---- SDL GPU objects (all owned) ------------------------------------

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

    // Non-owning reference to the window (lifetime managed by Window).
    SDL_Window* sdl_window_{nullptr};

    // ---- Text renderer --------------------------------------------------

    std::unique_ptr<TextRenderer> text_renderer_;

    // ---- UI scene (non-owning) -----------------------------------------

    RenderTree* scene_tree_{nullptr};
    NodeHandle  scene_root_{k_null_handle};

    // ---- State ----------------------------------------------------------

    std::atomic<bool>   initialized_{false};
    SDL_GPUShaderFormat shader_format_{SDL_GPU_SHADERFORMAT_INVALID};
    std::string         shader_path_;
    std::string         vertex_shader_path_;
    std::uintmax_t      shader_mtime_{0};
};

} // namespace pce::sdlos
