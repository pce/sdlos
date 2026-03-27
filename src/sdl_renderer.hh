#pragma once

#include "render_tree.hh"
#include "text_renderer.hh"

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

    /// Returns false on any unrecoverable failure.
    bool Initialize(SDL_Window* window);

    /// Safe to call multiple times; called automatically by the destructor.
    void Shutdown() noexcept;

    // ---- Per-frame -------------------------------------------------------

    /// `timeSeconds` forwarded to the wallpaper fragment shader uniform.
    void Render(double timeSeconds);

    // ---- UI scene attachment ---------------------------------------------

    /// `tree` is non-owning and must outlive this renderer.
    /// Pass k_null_handle to detach.
    void SetScene(RenderTree* tree,
                  NodeHandle  root = k_null_handle) noexcept;

    // ---- Shader hot-reload -----------------------------------------------

    /// Returns true on success; the previous pipeline stays active on failure.
    bool ReloadShader(const std::string& shaderPath);

    // ---- Queries ---------------------------------------------------------

    [[nodiscard]] bool IsValid() const noexcept { return initialized_.load(); }

    [[nodiscard]] std::vector<std::string> EnumerateAdapters() const;

    // ---- Shader source path hints ----------------------------------------

    void SetVertexShaderPath(const std::string& path)   { vertex_shader_path_ = path; }
    void SetFragmentShaderPath(const std::string& path) { shader_path_        = path; }

    // ---- GPU context access (non-owning views) ---------------------------

    /// Valid only after a successful Initialize().
    [[nodiscard]] SDL_GPUDevice*        GetDevice()        const noexcept { return device_;        }

    /// Shader format selected during Initialize() (MSL, SPIRV, …).
    [[nodiscard]] SDL_GPUShaderFormat   GetShaderFormat()  const noexcept { return shader_format_; }

    /// Non-null only after a successful Initialize().
    [[nodiscard]] TextRenderer*         GetTextRenderer()  const noexcept { return text_renderer_.get(); }

private:
    // ---- Internal pipeline builders --------------------------------------

    /// Sets shader_format_ on success.
    bool CreateDeviceForWindow(SDL_Window* window);

    /// Entry-point for both stages must be `main0`.
    bool CreatePipeline(const std::string& vertSource,
                        const std::string& fragSource);

    /// Non-fatal if either pipeline fails; wallpaper still renders without UI.
    bool CreateUIPipelines();

    // ---- File helpers ----------------------------------------------------

    static std::string    ReadTextFile(const std::string& path);
    static std::uintmax_t GetFileMTime(const std::string& path);

    // ---- Wallpaper fragment shader uniform -------------------------------

    struct FragmentUniform {
        float time;
        float pad[3];
    };
    static_assert(sizeof(FragmentUniform) == 16,
                  "FragmentUniform must be 16 bytes for push-uniform alignment");

private:
    // ---- SDL GPU objects (all owned) -------------------------------------

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

    // Non-owning reference; lifetime managed by Window.
    SDL_Window* sdl_window_{nullptr};

    // ---- Text renderer ---------------------------------------------------

    std::unique_ptr<TextRenderer> text_renderer_;

    // ---- UI scene (non-owning) -------------------------------------------

    RenderTree* scene_tree_{nullptr};
    NodeHandle  scene_root_{k_null_handle};

    // ---- State -----------------------------------------------------------

    std::atomic<bool>   initialized_{false};
    SDL_GPUShaderFormat shader_format_{SDL_GPU_SHADERFORMAT_INVALID};
    std::string         shader_path_;
    std::string         vertex_shader_path_;
    std::uintmax_t      shader_mtime_{0};
};

} // namespace pce::sdlos
