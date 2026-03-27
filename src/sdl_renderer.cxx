
// SDLRenderer.cxx
// SDL3 GPU-based renderer implementation for pce::sdlos
//
// - Uses SDL's GPU API to create a device (preferring Metal on macOS via MSL).
// - Compiles MSL vertex/fragment shaders at runtime and creates a graphics pipeline.
// - Provides a simple fullscreen pass renderer with a time uniform pushed every frame.
// - Supports hot-reload of the fragment shader via file mtime (ReloadShader).
//
// Notes:
//  - The shader entrypoints for MSL are expected to be named `main0` (vertex + fragment).
//  - The fragment shader should declare a small uniform buffer at [[buffer(0)]] with a
//    float (we push a 16-byte block where .x == time).
//
// This file is intended to be self-contained and should compile with the SDL3 GPU
// headers included in the repository deps.

#include "sdl_renderer.hh"

#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <cstring>

namespace pce::sdlos {
namespace fs = std::filesystem;

SDLRenderer::~SDLRenderer() {
    try {
        Shutdown();
    } catch (...) {
        // Destructor must not throw.
    }
}

// Helper: read an entire text file into a string.
std::string SDLRenderer::ReadTextFile(const std::string& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) {
        return {};
    }
    std::ostringstream ss;
    ss << ifs.rdbuf();
    return ss.str();
}

std::uintmax_t SDLRenderer::GetFileMTime(const std::string& path) {
    try {
        if (!fs::exists(path)) {
            return 0;
        }
        auto t = fs::last_write_time(path);
        return static_cast<std::uintmax_t>(t.time_since_epoch().count());
    } catch (...) {
        return 0;
    }
}

std::vector<std::string> SDLRenderer::EnumerateAdapters() const {
    std::vector<std::string> result;
    int n = SDL_GetNumGPUDrivers();
    for (int i = 0; i < n; ++i) {
        const char* name = SDL_GetGPUDriver(i);
        if (name) {
            result.emplace_back(name);
        }
    }
    return result;
}

bool SDLRenderer::CreateDeviceForWindow(SDL_Window* window) {
    if (!window) {
        std::cerr << "SDLRenderer::CreateDeviceForWindow - null window\n";
        return false;
    }

    // Prefer Metal + MSL on platforms that support it.
    device_ = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_MSL, false, "metal");
    if (!device_) {
        // Try without forcing a specific driver name; allow any driver that supports MSL.
        device_ = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_MSL, false, nullptr);
    }
    if (!device_) {
        std::cerr << "SDLRenderer::CreateDeviceForWindow - SDL_CreateGPUDevice failed: " << SDL_GetError() << "\n";
        return false;
    }

    // Claim the window so SDL manages swapchain / presentation for it.
    if (!SDL_ClaimWindowForGPUDevice(device_, window)) {
        std::cerr << "SDLRenderer::CreateDeviceForWindow - SDL_ClaimWindowForGPUDevice failed: " << SDL_GetError() << "\n";
        SDL_DestroyGPUDevice(device_);
        device_ = nullptr;
        return false;
    }

    sdl_window_ = window;
    return true;
}

bool SDLRenderer::CreatePipeline(const std::string& vertSource, const std::string& fragSource) {
    if (!device_ || !sdl_window_) {
        std::cerr << "SDLRenderer::CreatePipeline - device/window not initialized\n";
        return false;
    }

    // Create vertex shader
    SDL_GPUShaderCreateInfo vsc{};
    vsc.code = reinterpret_cast<const Uint8*>(vertSource.data());
    vsc.code_size = vertSource.size();
    vsc.entrypoint = "main0";
    vsc.format = SDL_GPU_SHADERFORMAT_MSL;
    vsc.stage = SDL_GPU_SHADERSTAGE_VERTEX;
    vsc.num_samplers = 0;
    vsc.num_uniform_buffers = 1;
    vsc.props = 0;

    SDL_GPUShader* newVertex = SDL_CreateGPUShader(device_, &vsc);
    if (!newVertex) {
        std::cerr << "SDLRenderer::CreatePipeline - SDL_CreateGPUShader (vertex) failed: " << SDL_GetError() << "\n";
        return false;
    }

    // Create fragment shader
    SDL_GPUShaderCreateInfo fsc{};
    fsc.code = reinterpret_cast<const Uint8*>(fragSource.data());
    fsc.code_size = fragSource.size();
    fsc.entrypoint = "main0";
    fsc.format = SDL_GPU_SHADERFORMAT_MSL;
    fsc.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    fsc.num_samplers = 0;
    fsc.num_uniform_buffers = 1; // we push a small time uniform
    fsc.props = 0;

    SDL_GPUShader* newFragment = SDL_CreateGPUShader(device_, &fsc);
    if (!newFragment) {
        std::cerr << "SDLRenderer::CreatePipeline - SDL_CreateGPUShader (fragment) failed: " << SDL_GetError() << "\n";
        SDL_ReleaseGPUShader(device_, newVertex);
        return false;
    }

    // Pipeline create info
    SDL_GPUGraphicsPipelineCreateInfo pci{};
    // Vertex input state: empty (we rely on vertex_id in shader)
    SDL_GPUVertexInputState vis{};
    vis.vertex_buffer_descriptions = nullptr;
    vis.num_vertex_buffers = 0;
    vis.vertex_attributes = nullptr;
    vis.num_vertex_attributes = 0;
    pci.vertex_input_state = vis;

    pci.vertex_shader = newVertex;
    pci.fragment_shader = newFragment;

    pci.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;

    // Rasterizer / multisample / depth-stencil default (zero-initialized)
    SDL_GPURasterizerState ras{};
    pci.rasterizer_state = ras;

    SDL_GPUMultisampleState ms{};
    pci.multisample_state = ms;

    SDL_GPUDepthStencilState ds{};
    pci.depth_stencil_state = ds;

    // Target info: use the swapchain texture format for this window/device.
    SDL_GPUColorTargetDescription colorDesc{};
    colorDesc.format = SDL_GetGPUSwapchainTextureFormat(device_, sdl_window_);
    SDL_GPUGraphicsPipelineTargetInfo targetInfo{};
    targetInfo.color_target_descriptions = &colorDesc;
    targetInfo.num_color_targets = 1;
    targetInfo.has_depth_stencil_target = false;
    pci.target_info = targetInfo;

    SDL_GPUGraphicsPipeline* newPipeline = SDL_CreateGPUGraphicsPipeline(device_, &pci);
    if (!newPipeline) {
        std::cerr << "SDLRenderer::CreatePipeline - SDL_CreateGPUGraphicsPipeline failed: " << SDL_GetError() << "\n";
        SDL_ReleaseGPUShader(device_, newVertex);
        SDL_ReleaseGPUShader(device_, newFragment);
        return false;
    }

    // Success: replace old pipeline & shaders atomically
    if (pipeline_) {
        SDL_ReleaseGPUGraphicsPipeline(device_, pipeline_);
        pipeline_ = nullptr;
    }
    if (vertex_shader_) {
        SDL_ReleaseGPUShader(device_, vertex_shader_);
        vertex_shader_ = nullptr;
    }
    if (fragment_shader_) {
        SDL_ReleaseGPUShader(device_, fragment_shader_);
        fragment_shader_ = nullptr;
    }

    pipeline_ = newPipeline;
    vertex_shader_ = newVertex;
    fragment_shader_ = newFragment;

    std::cerr << "SDLRenderer::CreatePipeline - pipeline created successfully\n";
    return true;
}

bool SDLRenderer::Initialize(SDL_Window* window) {
    if (!window) {
        std::cerr << "SDLRenderer::Initialize - null window\n";
        return false;
    }

    if (initialized_.load()) {
        std::cerr << "SDLRenderer::Initialize - already initialized\n";
        return true;
    }

    // Create device and claim window
    if (!CreateDeviceForWindow(window)) {
        return false;
    }

    // Attempt to find shader files in the standard assets location
    // Vertex shader (optional; fallback to built-in)
    std::string vertPath = vertex_shader_path_.empty() ? "assets/shaders/desktop.vert.metal" : vertex_shader_path_;
    std::string fragPath = shader_path_.empty() ? "assets/shaders/desktop.frag.metal" : shader_path_;

    std::string vertCode = ReadTextFile(vertPath);
    if (vertCode.empty()) {
        // Fallback vertex shader: fullscreen triangle using vertex_id
        vertCode = R"(
#include <metal_stdlib>
using namespace metal;
struct VertOut {
    float4 position [[position]];
    float2 uv [[user(locn0)]];
};
vertex VertOut main0(uint vid [[vertex_id]]) {
    VertOut out;
    float2 pos[3] = { float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0) };
    out.position = float4(pos[vid], 0.0, 1.0);
    out.uv = out.position.xy * 0.5 + float2(0.5, 0.5);
    return out;
}
)";
    }

    std::string fragCode = ReadTextFile(fragPath);
    if (fragCode.empty()) {
        // Fallback fragment shader: simple animated FBM-ish gradient
        fragCode = R"(
#include <metal_stdlib>
using namespace metal;

struct FragUniform {
    float time;
    float pad0;
    float pad1;
    float pad2;
};

struct VertOut {
    float4 position [[position]];
    float2 uv [[user(locn0)]];
};

float hash21(float2 p) {
    float3 p3 = float3(p.xyx) * float3(0.1031, 0.11369, 0.13787);
    return fract(sin(dot(p3, float3(12.9898, 78.233, 37.719))) * 43758.5453);
}

float noise(float2 p) {
    float2 i = floor(p);
    float2 f = fract(p);
    float a = hash21(i + float2(0.0,0.0));
    float b = hash21(i + float2(1.0,0.0));
    float c = hash21(i + float2(0.0,1.0));
    float d = hash21(i + float2(1.0,1.0));
    float2 u = f * f * (3.0 - 2.0 * f);
    return mix(a, b, u.x) + (c - a) * u.y * (1.0 - u.x) + (d - b) * u.x * u.y;
}

float fbm(float2 p) {
    float v = 0.0;
    float a = 0.5;
    for (int i = 0; i < 5; ++i) {
        v += a * noise(p);
        p = p * 2.0 + float2(1.0, 0.5);
        a *= 0.5;
    }
    return v;
}

fragment float4 main0(VertOut in [[stage_in]], constant FragUniform &u [[buffer(0)]]) {
    float2 uv = in.uv * 2.0 - 1.0;
    float t = u.time * 0.33;
    float n = fbm(uv * 1.5 + float2(t, t * 0.5));
    float3 col = mix(float3(0.1, 0.3, 0.6), float3(0.9, 0.6, 0.3), n);
    return float4(col, 1.0);
}
)";
    }

    // Build pipeline from the sources
    if (!CreatePipeline(vertCode, fragCode)) {
        std::cerr << "SDLRenderer::Initialize - failed to build pipeline from default shaders\n";
        Shutdown();
        return false;
    }

    // Record fragment shader path mtime if the file exists
    if (fs::exists(fragPath)) {
        shader_path_ = fragPath;
        shader_mtime_ = GetFileMTime(fragPath);
    } else {
        shader_path_.clear();
        shader_mtime_ = 0;
    }

    initialized_.store(true);
    std::cerr << "SDLRenderer::Initialize - initialized successfully\n";
    return true;
}

void SDLRenderer::Shutdown() {
    if (!device_) {
        initialized_.store(false);
        return;
    }

    // Release pipeline & shaders
    if (pipeline_) {
        SDL_ReleaseGPUGraphicsPipeline(device_, pipeline_);
        pipeline_ = nullptr;
    }
    if (vertex_shader_) {
        SDL_ReleaseGPUShader(device_, vertex_shader_);
        vertex_shader_ = nullptr;
    }
    if (fragment_shader_) {
        SDL_ReleaseGPUShader(device_, fragment_shader_);
        fragment_shader_ = nullptr;
    }

    // Release window claim (if any)
    if (sdl_window_) {
        SDL_ReleaseWindowFromGPUDevice(device_, sdl_window_);
        sdl_window_ = nullptr;
    }

    // Destroy the device if it wasn't provided externally
    SDL_DestroyGPUDevice(device_);
    device_ = nullptr;

    initialized_.store(false);
    shader_path_.clear();
    shader_mtime_ = 0;

    std::cerr << "SDLRenderer::Shutdown - resources released\n";
}

void SDLRenderer::Render(double timeSeconds) {
    if (!initialized_.load() || !device_) {
        return;
    }

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    if (!cmd) {
        std::cerr << "SDLRenderer::Render - SDL_AcquireGPUCommandBuffer failed: " << SDL_GetError() << "\n";
        return;
    }

    SDL_GPUTexture* swap = nullptr;
    Uint32 width = 0, height = 0;

    if (!SDL_AcquireGPUSwapchainTexture(cmd, sdl_window_, &swap, &width, &height)) {
        // Hard acquire error (device lost, window invalid, etc.).
        // The command buffer must still be submitted — SDL3 GPU does not
        // provide a cancel path; every acquired cmd buf must be submitted.
        std::cerr << "SDLRenderer::Render - SDL_AcquireGPUSwapchainTexture error: "
                  << SDL_GetError() << "\n";
        SDL_SubmitGPUCommandBuffer(cmd);
        return;
    }

    if (!swap) {
        // Swapchain not ready this frame — normal during minimise / resize.
        // Submit the empty command buffer and skip rendering.
        SDL_SubmitGPUCommandBuffer(cmd);
        return;
    }

    // Configure color target to clear and store
    SDL_GPUColorTargetInfo ct{};
    ct.texture = swap;
    ct.mip_level = 0;
    ct.layer_or_depth_plane = 0;
    ct.clear_color = {0.0f, 0.0f, 0.0f, 1.0f};
    ct.load_op = SDL_GPU_LOADOP_CLEAR;
    ct.store_op = SDL_GPU_STOREOP_STORE;
    ct.resolve_texture = nullptr;
    ct.resolve_mip_level = 0;
    ct.resolve_layer = 0;
    ct.cycle = true;

    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &ct, 1, nullptr);
    if (!pass) {
        std::cerr << "SDLRenderer::Render - SDL_BeginGPURenderPass failed: " << SDL_GetError() << "\n";
        SDL_SubmitGPUCommandBuffer(cmd);
        return;
    }

    // Bind pipeline and push uniform
    SDL_BindGPUGraphicsPipeline(pass, pipeline_);

    FragmentUniform fu{};
    fu.time = static_cast<float>(timeSeconds);
    fu.pad[0] = fu.pad[1] = fu.pad[2] = 0.0f;
    SDL_PushGPUFragmentUniformData(cmd, 0, &fu, sizeof(fu));

    // Draw fullscreen triangle (3 vertices)
    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);

    SDL_EndGPURenderPass(pass);

    // Submit the command buffer. Do NOT call SDL_ReleaseGPUTexture on `swap` —
    // swapchain textures are non-owning views managed entirely by the device.
    // Releasing them corrupts the Metal/Vulkan pending-destroy queue and causes
    // a null-dereference in METAL_INTERNAL_PerformPendingDestroys on next submit.
    SDL_SubmitGPUCommandBuffer(cmd);
}

bool SDLRenderer::ReloadShader(const std::string& path) {
    try {
        if (!fs::exists(path)) {
            std::cerr << "SDLRenderer::ReloadShader - shader file not found: " << path << "\n";
            return false;
        }
        auto mtime = GetFileMTime(path);
        if (mtime == shader_mtime_) {
            // Unchanged
            return true;
        }

        std::string source = ReadTextFile(path);
        if (source.empty()) {
            std::cerr << "SDLRenderer::ReloadShader - failed to read shader: " << path << "\n";
            return false;
        }

        // We need a vertex shader source too. Try to load the previously used vertex shader
        // from disk (if a path was provided) or use a default vertex shader as in Initialize.
        std::string vertSource;
        if (!vertex_shader_path_.empty() && fs::exists(vertex_shader_path_)) {
            vertSource = ReadTextFile(vertex_shader_path_);
        }
        if (vertSource.empty()) {
            // fallback same default used in Initialize
            vertSource = R"(
#include <metal_stdlib>
using namespace metal;
struct VertOut {
    float4 position [[position]];
    float2 uv [[user(locn0)]];
};
vertex VertOut main0(uint vid [[vertex_id]]) {
    VertOut out;
    float2 pos[3] = { float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0) };
    out.position = float4(pos[vid], 0.0, 1.0);
    out.uv = out.position.xy * 0.5 + float2(0.5, 0.5);
    return out;
}
)";
        }

        // Build a new pipeline with the new fragment shader
        if (!CreatePipeline(vertSource, source)) {
            std::cerr << "SDLRenderer::ReloadShader - pipeline rebuild from updated shader failed\n";
            return false;
        }

        shader_path_ = path;
        shader_mtime_ = mtime;
        std::cerr << "SDLRenderer::ReloadShader - shader reloaded: " << path << "\n";
        return true;
    } catch (const std::exception& e) {
        std::cerr << "SDLRenderer::ReloadShader - exception: " << e.what() << "\n";
        return false;
    } catch (...) {
        std::cerr << "SDLRenderer::ReloadShader - unknown exception\n";
        return false;
    }
}

} // namespace pce::sdlos
