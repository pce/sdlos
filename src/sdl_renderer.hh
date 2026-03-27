#pragma once

// SDLRenderer.h
// SDL3 GPU-based renderer wrapper for pce::sdlos.
//
// Process-isolation model
// -----------------------
// Each SDLRenderer instance creates and owns its own SDL_GPUDevice. This means
// every Window gets an independent GPU context: tearing down one Window releases
// its device, shaders and pipeline without touching any other Window. The owned
// device is exposed via GetDevice() so that application-level draw code can
// submit into the same context without requiring a second device.
//
// Cross-platform shader backend
// -----------------------------
// SDL_GPU supports multiple shader formats at runtime:
//   SDL_GPU_SHADERFORMAT_SPIRV  → Vulkan  (Linux, Windows, cross-platform)
//   SDL_GPU_SHADERFORMAT_MSL    → Metal   (macOS / iOS)
//   SDL_GPU_SHADERFORMAT_DXIL   → D3D12   (Windows)
//
// CreateDeviceForWindow() tries SPIRV/Vulkan first (widest portability), then
// falls back to MSL/Metal on Apple platforms. The selected format is stored in
// shader_format_ so that CreatePipeline() can compile the right variant.
// For SPIRV shaders, compile GLSL → .spv with glslang/shaderc offline and place
// them alongside the MSL sources; the loader will pick the correct file.
//
// Usage summary:
//  - Call `Initialize(window)` once per Window to create the device, claim the
//    window, compile shaders and build the graphics pipeline.
//  - Call `Render(timeSeconds)` each frame.
//  - Call `ReloadShader(path)` to hot-reload the fragment shader from disk.
//  - Call `Shutdown()` to release all GPU resources (also called by destructor).
//  - Call `GetDevice()` to share the device with application draw code.
//
// Notes:
//  - The renderer does NOT take ownership of the SDL_Window* passed to Initialize().
//  - Hot-reload is file-mtime based; wire up a filesystem watcher for live updates.

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include <string>
#include <vector>
#include <atomic>
#include <cstdint>
#include <filesystem>

namespace pce::sdlos {

class SDLRenderer {
public:
    SDLRenderer() = default;
    ~SDLRenderer();

    // Initialize the renderer for the given SDL_Window.
    // Returns true on success (device + pipeline created), false on failure.
    // The renderer does not take ownership of the window pointer.
    bool Initialize(SDL_Window* window);

    // Shutdown and free GPU resources. Safe to call multiple times.
    void Shutdown();

    // Render a frame. `timeSeconds` is pushed to the fragment shader (as .x of a vec4).
    // This is non-blocking and submits the command buffer for presentation.
    void Render(double timeSeconds);

    // Reload the fragment shader from disk and rebuild the pipeline.
    // Assumes a vertex shader is present in the assets (or built-in fallback).
    // Returns true on success.
    bool ReloadShader(const std::string& shaderPath);

    // Returns whether the renderer is valid and ready to render.
    bool IsValid() const { return initialized_.load(); }

    // Enumerate available GPU drivers / backends known by SDL (e.g. "metal", "vulkan").
    std::vector<std::string> EnumerateAdapters() const;

    // Set the default shader source paths (optional convenience).
    void SetVertexShaderPath(const std::string& path)   { vertex_shader_path_ = path; }
    void SetFragmentShaderPath(const std::string& path) { shader_path_        = path; }

    // --- GPU context access -----------------------------------------------

    // Return the SDL_GPUDevice owned by this renderer.
    // Non-null only after a successful Initialize(). The device lifetime is
    // tied to this SDLRenderer — callers must not call SDL_DestroyGPUDevice()
    // on the returned pointer.
    SDL_GPUDevice* GetDevice() const { return device_; }

    // Return the shader format that was selected during Initialize().
    // Use this when creating additional pipelines against the same device so
    // that shader sources match the backend (SPIRV vs MSL vs DXIL).
    SDL_GPUShaderFormat GetShaderFormat() const { return shader_format_; }

private:
    // Probe available SDL GPU drivers and create a device for the given window.
    // Selection order: SPIRV/Vulkan → MSL/Metal → DXIL/D3D12.
    // Sets shader_format_ to reflect the chosen backend.
    bool CreateDeviceForWindow(SDL_Window* window);

    // Build shaders and graphics pipeline from source strings.
    // The format of vertSource / fragSource must match shader_format_.
    // Both vertex and fragment entry-points are expected to be named `main0`.
    bool CreatePipeline(const std::string& vertSource, const std::string& fragSource);

    // Internal helpers to load text files and check mtimes.
    static std::string ReadTextFile(const std::string& path);
    static std::uintmax_t GetFileMTime(const std::string& path);

    // Small uniform layout used when pushing fragment uniform data.
    // Must be 16 bytes so it respects std140/std430-style alignment for push uniforms.
    struct FragmentUniform {
        float time;
        float pad[3];
    };
    static_assert(sizeof(FragmentUniform) == 16, "FragmentUniform must be 16 bytes");

private:
    // --- SDL GPU objects (all owned by this instance) ---------------------
    SDL_GPUDevice*           device_{nullptr};
    SDL_GPUShader*           vertex_shader_{nullptr};
    SDL_GPUShader*           fragment_shader_{nullptr};
    SDL_GPUGraphicsPipeline* pipeline_{nullptr};

    // Non-owning reference to the window this renderer was initialized for.
    SDL_Window* sdl_window_{nullptr};

    // --- State ------------------------------------------------------------
    std::atomic<bool>    initialized_{false};
    SDL_GPUShaderFormat  shader_format_{SDL_GPU_SHADERFORMAT_INVALID}; // set by CreateDeviceForWindow
    std::string          shader_path_;         // fragment shader path (hot-reload)
    std::string          vertex_shader_path_;  // vertex shader path   (hot-reload)
    std::uintmax_t       shader_mtime_{0};
};

} // namespace pce::sdlos
