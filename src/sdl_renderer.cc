#include "sdl_renderer.h"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace pce::sdlos {
namespace fs = std::filesystem;

/**
 * @brief ~sdl renderer
 */
SDLRenderer::~SDLRenderer() {
    Shutdown();
}

/**
 * @brief Reads text file
 *
 * @param path  Filesystem path
 *
 * @return Integer result; negative values indicate an error code
 */
std::string SDLRenderer::ReadTextFile(const std::string &path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) {
        return {};
    }
    std::ostringstream ss;
    ss << ifs.rdbuf();
    return ss.str();
}

/**
 * @brief Returns file m time
 *
 * @param path  Filesystem path
 *
 * @return Integer result; negative values indicate an error code
 */
std::uintmax_t SDLRenderer::GetFileMTime(const std::string &path) {
    std::error_code ec;
    if (!fs::exists(path, ec) || ec)
        return 0;
    const auto t = fs::last_write_time(path, ec);
    if (ec)
        return 0;
    return static_cast<std::uintmax_t>(t.time_since_epoch().count());
}

/**
 * @brief Enumerate adapters
 *
 * @return Integer result; negative values indicate an error code
 */
std::vector<std::string> SDLRenderer::EnumerateAdapters() const {
    std::vector<std::string> result;
    int n = SDL_GetNumGPUDrivers();
    for (int i = 0; i < n; ++i) {
        const char *name = SDL_GetGPUDriver(i);
        if (name) {
            result.emplace_back(name);
        }
    }
    return result;
}

/**
 * @brief Creates and returns device for window
 *
 * @param window  Width in logical pixels
 *
 * @return true on success, false on failure
 */
bool SDLRenderer::CreateDeviceForWindow(SDL_Window *window) {
    if (!window) {
        std::cerr << "SDLRenderer::CreateDeviceForWindow - null window\n";
        return false;
    }

    // Prefer Metal + MSL; try named driver first, then any compatible driver.
    device_ = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_MSL, false, "metal");
    if (!device_) {
        device_ = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_MSL, false, nullptr);
    }
    if (!device_) {
        std::cerr << "SDLRenderer::CreateDeviceForWindow - SDL_CreateGPUDevice failed: "
                  << SDL_GetError() << "\n";
        return false;
    }

    if (!SDL_ClaimWindowForGPUDevice(device_, window)) {
        std::cerr << "SDLRenderer::CreateDeviceForWindow - SDL_ClaimWindowForGPUDevice failed: "
                  << SDL_GetError() << "\n";
        SDL_DestroyGPUDevice(device_);
        device_ = nullptr;
        return false;
    }

    sdl_window_ = window;

    // Query the device for the actual supported shader format and cache it.
    // GetShaderFormat() returns this value; callers such as GltfScene::init()
    // depend on it being non-zero.  Without this assignment it stays at the
    // default SDL_GPU_SHADERFORMAT_INVALID (0) and all 3D pipeline creation fails.
    const SDL_GPUShaderFormat avail = SDL_GetGPUShaderFormats(device_);
    if (avail & SDL_GPU_SHADERFORMAT_MSL) {
        shader_format_ = SDL_GPU_SHADERFORMAT_MSL;
    } else if (avail & SDL_GPU_SHADERFORMAT_SPIRV) {
        shader_format_ = SDL_GPU_SHADERFORMAT_SPIRV;
    } else if (avail & SDL_GPU_SHADERFORMAT_DXIL) {
        shader_format_ = SDL_GPU_SHADERFORMAT_DXIL;
    } else {
        std::cerr << "SDLRenderer::CreateDeviceForWindow - no recognised shader format in " << avail
                  << "\n";
    }

    return true;
}

/**
 * @brief Creates and returns pipeline
 *
 * @param vertSource  Red channel component [0, 1]
 * @param fragSource  Red channel component [0, 1]
 *
 * @return true on success, false on failure
 */
bool SDLRenderer::CreatePipeline(const std::string &vertSource, const std::string &fragSource) {
    if (!device_ || !sdl_window_) {
        std::cerr << "SDLRenderer::CreatePipeline - device/window not initialized\n";
        return false;
    }

    SDL_GPUShaderCreateInfo vsc{};
    vsc.code                = reinterpret_cast<const Uint8 *>(vertSource.data());
    vsc.code_size           = vertSource.size();
    vsc.entrypoint          = "main0";
    vsc.format              = SDL_GPU_SHADERFORMAT_MSL;
    vsc.stage               = SDL_GPU_SHADERSTAGE_VERTEX;
    vsc.num_samplers        = 0;
    vsc.num_uniform_buffers = 1;
    vsc.props               = 0;

    SDL_GPUShader *newVertex = SDL_CreateGPUShader(device_, &vsc);
    if (!newVertex) {
        std::cerr << "SDLRenderer::CreatePipeline - SDL_CreateGPUShader (vertex) failed: "
                  << SDL_GetError() << "\n";
        return false;
    }

    SDL_GPUShaderCreateInfo fsc{};
    fsc.code                = reinterpret_cast<const Uint8 *>(fragSource.data());
    fsc.code_size           = fragSource.size();
    fsc.entrypoint          = "main0";
    fsc.format              = SDL_GPU_SHADERFORMAT_MSL;
    fsc.stage               = SDL_GPU_SHADERSTAGE_FRAGMENT;
    fsc.num_samplers        = 0;
    fsc.num_uniform_buffers = 1;
    fsc.props               = 0;

    SDL_GPUShader *newFragment = SDL_CreateGPUShader(device_, &fsc);
    if (!newFragment) {
        std::cerr << "SDLRenderer::CreatePipeline - SDL_CreateGPUShader (fragment) failed: "
                  << SDL_GetError() << "\n";
        SDL_ReleaseGPUShader(device_, newVertex);
        return false;
    }

    SDL_GPUGraphicsPipelineCreateInfo pci{};
    // No vertex buffer: geometry generated from vertex_id in the shader.
    SDL_GPUVertexInputState vis{};
    vis.vertex_buffer_descriptions = nullptr;
    vis.num_vertex_buffers         = 0;
    vis.vertex_attributes          = nullptr;
    vis.num_vertex_attributes      = 0;
    pci.vertex_input_state         = vis;

    pci.vertex_shader   = newVertex;
    pci.fragment_shader = newFragment;
    pci.primitive_type  = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;

    SDL_GPURasterizerState ras{};
    pci.rasterizer_state = ras;

    SDL_GPUMultisampleState ms{};
    pci.multisample_state = ms;

    SDL_GPUDepthStencilState ds{};
    pci.depth_stencil_state = ds;

    SDL_GPUColorTargetDescription colorDesc{};
    colorDesc.format = SDL_GetGPUSwapchainTextureFormat(device_, sdl_window_);
    SDL_GPUGraphicsPipelineTargetInfo targetInfo{};
    targetInfo.color_target_descriptions = &colorDesc;
    targetInfo.num_color_targets         = 1;
    targetInfo.has_depth_stencil_target  = false;
    pci.target_info                      = targetInfo;

    SDL_GPUGraphicsPipeline *newPipeline = SDL_CreateGPUGraphicsPipeline(device_, &pci);
    if (!newPipeline) {
        std::cerr << "SDLRenderer::CreatePipeline - SDL_CreateGPUGraphicsPipeline failed: "
                  << SDL_GetError() << "\n";
        SDL_ReleaseGPUShader(device_, newVertex);
        SDL_ReleaseGPUShader(device_, newFragment);
        return false;
    }

    // Replace old pipeline and shaders atomically.
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

    pipeline_        = newPipeline;
    vertex_shader_   = newVertex;
    fragment_shader_ = newFragment;

    std::cerr << "SDLRenderer::CreatePipeline - pipeline created successfully\n";
    return true;
}

/**
 * @brief Initialises
 *
 * @param window  Width in logical pixels
 *
 * @return true on success, false on failure
 */
bool SDLRenderer::Initialize(SDL_Window *window) {
    if (!window) {
        std::cerr << "SDLRenderer::Initialize - null window\n";
        return false;
    }

    if (initialized_.load()) {
        std::cerr << "SDLRenderer::Initialize - already initialized\n";
        return true;
    }

    if (!CreateDeviceForWindow(window)) {
        return false;
    }

    // Search order for the desktop background shaders:
    //   1. app-supplied path (shader_path_ / vertex_shader_path_ set before Initialize)
    //   2. data/shaders/msl/ — next to the binary (CMake deploy target)
    //   3. assets/shaders/msl/ — source-tree path when running from repo root
    //   4. inline fallback (chrome SDF gradient, always works)
    auto pickPath = [](const std::string &forced,
                       std::initializer_list<const char *> candidates) -> std::string {
        if (!forced.empty())
            return forced;
        for (const char *p : candidates)
            if (fs::exists(p))
                return p;
        return {};
    };
    std::string vertPath = pickPath(
        vertex_shader_path_,
        {
            "data/shaders/msl/desktop.vert.metal",
            "assets/shaders/msl/desktop.vert.metal",
        });
    std::string fragPath = pickPath(
        shader_path_,
        {
            "data/shaders/msl/desktop.frag.metal",
            "assets/shaders/msl/desktop.frag.metal",
        });

    std::string vertCode = ReadTextFile(vertPath);
    if (vertCode.empty()) {
        // Fallback: fullscreen triangle using vertex_id
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
        // Fallback: grayscale chrome SDF gradient (matches assets/shaders/msl/desktop.frag.metal)
        fragCode = R"(
#include <metal_stdlib>
using namespace metal;

struct FragUniform { float time; float pad0; float pad1; float pad2; };
struct VertOut {
    float4 position [[position]];
    float2 uv [[user(locn0)]];
};

static float h(float2 p, float t) {
    float v = 0.0;
    v += 0.45 * sin(p.x * 1.05 + t * 0.13) * cos(p.y * 0.80 + t * 0.09);
    v += 0.28 * sin(p.x * 0.65 - p.y * 1.15 + t * 0.10);
    v += 0.18 * cos(p.x * 1.45 + p.y * 0.50 - t * 0.07);
    v += 0.12 * sin(p.y * 1.70 - p.x * 0.35 + t * 0.05);
    return v;
}
static float2 sdfGrad(float2 p, float t) {
    const float e = 0.016;
    return float2(h(p+float2(e,0),t)-h(p-float2(e,0),t),
                  h(p+float2(0,e),t)-h(p-float2(0,e),t)) * (0.5/e);
}
static float chromeBRDF(float3 N, float3 L) {
    float3 V = float3(0,0,1); float3 H = normalize(L+V);
    float d = max(dot(N,L),0.0), s = max(dot(N,H),0.0);
    return 0.03 + 0.10*d + 0.75*pow(s,56.0) + 0.20*pow(s,9.0);
}
fragment float4 main0(VertOut in [[stage_in]], constant FragUniform& u [[buffer(0)]]) {
    float2 p = (in.uv*2.0-1.0)*1.6;
    float  t = u.time;
    float2 g = sdfGrad(p, t);
    float3 N = normalize(float3(-g.x,-g.y,1.0));
    float la = t*0.055;
    float3 L1 = normalize(float3(cos(la), sin(la*0.71), 2.2));
    float3 L2 = normalize(float3(-cos(la+1.9), -sin(la*0.71+0.5), 1.6));
    float raw = chromeBRDF(N,L1) + 0.35*chromeBRDF(N,L2);
    float c = 1.0 - exp(-raw*2.4);
    float gray = mix(0.04, 0.90, c);
    float edge = smoothstep(1.45, 0.55, length(p));
    gray *= mix(0.72, 1.0, edge);
    return float4(gray,gray,gray,1.0);
}
)";
    }

    if (!CreatePipeline(vertCode, fragCode)) {
        std::cerr << "SDLRenderer::Initialize - failed to build pipeline from default shaders\n";
        Shutdown();
        return false;
    }

    if (fs::exists(fragPath)) {
        shader_path_  = fragPath;
        shader_mtime_ = GetFileMTime(fragPath);
    } else {
        shader_path_.clear();
        shader_mtime_ = 0;
    }

    // Non-fatal: wallpaper still works; UI overlay is just disabled.
    if (!CreateUIPipelines()) {
        std::cerr << "SDLRenderer::Initialize - UI pipelines unavailable (overlay disabled)\n";
    }

    // Non-fatal: drawText() no-ops gracefully when the renderer is absent.
    // init() never loads fonts — font loading is always explicit.
    text_renderer_ = std::make_unique<TextRenderer>();
    if (!text_renderer_->init(device_)) {
        std::cerr << "SDLRenderer::Initialize - TextRenderer unavailable (text disabled)\n";
        text_renderer_.reset();
    } else {
        // assets/fonts/ is a legacy CWD-relative search path.
        // The canonical location is data/fonts/ resolved via SetDataBasePath(),
        // which jade_host calls immediately after Initialize() succeeds.
        // We try assets/fonts/ opportunistically so a developer running from the
        // repo root still gets a font; in normal installed builds this will miss
        // and the system font below serves as a temporary placeholder until
        // SetDataBasePath() loads the bundled typeface.
        const bool font_ok = text_renderer_->loadFirstAvailable(
            {
                "assets/fonts/InterVariable.ttf",
                "assets/fonts/Inter-Regular.ttf",
                "assets/fonts/Roboto-Regular.ttf",
                "assets/fonts/LiberationSans-Regular.ttf",
            },
            17.f);

        if (!font_ok) {
            // Expected in normal builds — SetDataBasePath() will override this
            // with the app-bundled font from data/fonts/.
            std::cout << "[SDLRenderer] note: assets/fonts/ not found; "
                         "installing system font as placeholder\n";
            if (!text_renderer_->tryLoadSystemFont(17.f)) {
                std::cerr << "[SDLRenderer] warning: no font available — "
                             "text rendering disabled\n";
            }
        }
    }

    // ImageCache — GPU-backed image texture cache for <img src="..."> nodes.
    // Requires SDL_image (SDL_IMAGE_AVAILABLE) for full format support; falls
    // back to SDL_LoadBMP (BMP only) when SDL_image is not compiled in.
    // Non-fatal: img nodes simply draw nothing if the cache is unavailable.
    image_cache_ = std::make_unique<ImageCache>();
    if (!image_cache_->init(device_)) {
        std::cerr << "SDLRenderer::Initialize - ImageCache unavailable (img disabled)\n";
        image_cache_.reset();
    }

    // VideoTexture — camera / video frame source.
    video_texture_ = std::make_unique<VideoTexture>();
    if (!video_texture_->init(device_)) {
        std::cerr << "[SDLRenderer] VideoTexture init failed (no camera support?)\n";
        // Non-fatal: camera may not be available on all platforms.
        video_texture_.reset();
    }

    // Wavetable texture — 1D sin wave lookup for shader performance.
    // 512 samples × 2 bytes = 1 KB. Non-fatal if creation fails.
    wavetable_texture_ = CreateWavetableTexture();

    // Capture the logical→physical pixel ratio for the window that was just
    // claimed.  sdl_window_ is valid at this point (set by CreateDeviceForWindow).
    UpdatePixelScale();

    initialized_.store(true);
    std::cerr << "SDLRenderer::Initialize - initialized successfully"
              << "  scale=" << pixel_scale_x_ << "x" << pixel_scale_y_ << "\n";
    return true;
}

/**
 * @brief Sets data base path
 *
 * @param path  Filesystem path
 */
void SDLRenderer::SetDataBasePath(const std::string &path) noexcept {
    data_base_path_ = path;
    // Ensure trailing slash so we can concatenate sub-paths directly.
    if (!data_base_path_.empty() && data_base_path_.back() != '/')
        data_base_path_ += '/';
    if (image_cache_)
        image_cache_->setBasePath(data_base_path_);
    std::cout << "[SDLRenderer] data base path: " << data_base_path_ << "\n";
    // Font loading is NOT performed here.  Not every app ships a data/fonts/
    // directory — only those that declare DATA_DIR in sdlos_jade_app().
    // Font selection is the responsibility of:
    //   1. loadAppFonts() in jade_host (scans <jade_dir>/data/fonts/)
    //   2. SDLRenderer::SetFontPath() called explicitly from jade_host or a
    //      behaviour via the _font jade attribute on the root node.

    // Auto-load pipeline.pug if present in the app's data/ directory.
    // CMake copies DATA_DIR contents to <binary_dir>/data/, so the file lives
    // at data_base_path_ + "data/pipeline.pug" at runtime.
    // Non-fatal: the built-in FBM wallpaper is the fallback when absent.
    const std::string pug_path = data_base_path_ + "data/pipeline.pug";
    if (fs::exists(pug_path)) {
        if (LoadPipeline(pug_path)) {
            std::cout << "[SDLRenderer] pipeline.pug loaded from: " << pug_path << "\n";
        } else {
            std::cerr << "[SDLRenderer] pipeline.pug found but failed to load: " << pug_path
                      << "\n";
        }
    }
}

// ---------------------------------------------------------------------------
// SetFontPath — explicit font selection (e.g. from a _font jade attribute or
// from jade_app_init()).
//
// Relative paths are resolved against data_base_path_ so an app can write:
//
//   renderer.SetFontPath("data/fonts/Inter-Regular.ttf");
//
// or declare in jade:
//
//   app(_font="data/fonts/Inter-Regular.ttf")
//
// jade_host reads the _font attribute after jade_app_init() runs and calls
// this method, giving the behaviour the last word on font selection.
// ---------------------------------------------------------------------------
/**
 * @brief Sets font path
 *
 * @param path     Filesystem path
 * @param pt_size  Capacity or number of elements
 *
 * @return true on success, false on failure
 */
bool SDLRenderer::SetFontPath(const std::string &path, float pt_size) noexcept {
    if (!text_renderer_)
        return false;

    // Absolute paths bypass the base path; relative paths are resolved under it.
    const std::string full = (!path.empty() && path[0] == '/') ? path : data_base_path_ + path;

    if (text_renderer_->loadFont(full, pt_size)) {
        std::cout << "[SDLRenderer] font set: " << full << "\n";
        return true;
    }
    std::cerr << "[SDLRenderer] SetFontPath: failed to load '" << full << "'\n";
    return false;
}

// ---------------------------------------------------------------------------
// LoadPipeline — parse and cache a pipeline.pug render pipeline.
//
// The FrameGraph is created from the pug source immediately.  The first
// CompiledGraph build is deferred to the first Render() call so the
// swapchain texture format is available.
//
// Calling this again (hot-reload) replaces the old FrameGraph atomically:
// the old compiled_graph_ is cleared first (it holds only non-owning
// pointers) and then frame_graph_ is replaced — ResourcePool and
// ShaderLibrary destructor releases happen during the assignment.
// ---------------------------------------------------------------------------
/**
 * @brief Loads pipeline
 *
 * @param pug_path  Filesystem path
 *
 * @return true on success, false on failure
 */
bool SDLRenderer::LoadPipeline(std::string_view pug_path) noexcept {
    if (!device_) {
        std::cerr << "[SDLRenderer] LoadPipeline: device not ready\n";
        return false;
    }

    // Shader binaries live in data/shaders/<platform>/ alongside the other
    // compiled assets.  CMake copies DATA_DIR to <binary_dir>/data/ so the
    // full path is data_base_path_ + "data/shaders/<platform>/".
    // This mirrors the convention used by ensureNodeShaderPipeline().
    std::string shader_dir;
    {
        const char *platform_dir = "spv";
        if (shader_format_ & SDL_GPU_SHADERFORMAT_MSL)
            platform_dir = "msl";
        else if (shader_format_ & SDL_GPU_SHADERFORMAT_DXIL)
            platform_dir = "dxil";
        shader_dir  = data_base_path_;
        shader_dir += "data/shaders/";
        shader_dir += platform_dir;
        shader_dir += '/';
    }

    // Use the current window pixel size to pre-size swapchain resources.
    // If the window isn't visible yet (size == 0,0) we pass zeros and the
    // ResourcePool will defer texture creation until the first resize/compile.
    int pw = 0, ph = 0;
    if (sdl_window_)
        SDL_GetWindowSizeInPixels(sdl_window_, &pw, &ph);

    auto result = fg::FrameGraph::from_file(
        pug_path,
        device_,
        shader_dir,
        shader_format_,
        static_cast<uint32_t>(pw),
        static_cast<uint32_t>(ph));

    if (!result) {
        std::cerr << "[SDLRenderer] LoadPipeline failed: " << result.error() << "\n";
        return false;
    }

    // Clear the old compiled graph (non-owning pointers — safe to discard).
    // Then replace the FrameGraph; old ResourcePool + ShaderLibrary auto-release.
    compiled_graph_ = {};
    frame_graph_    = std::move(*result);

    // Schedule a compile on the next Render() call when swapchain_fmt is known.
    fg_needs_compile_ = true;
    fg_compiled_w_    = 0;
    fg_compiled_h_    = 0;
    fg_swapchain_fmt_ = SDL_GPU_TEXTUREFORMAT_INVALID;

    SDL_Log(
        "[SDLRenderer] LoadPipeline: %zu passes parsed from '%s'",
        frame_graph_->passes().size(),
        std::string(pug_path).c_str());
    return true;
}

/**
 * @brief Submits pipeline source
 *
 * @param source  Red channel component [0, 1]
 */
void SDLRenderer::SubmitPipelineSource(std::string source) noexcept {
    pending_pipeline_source_ = std::move(source);
    pending_pipeline_dirty_  = true;
}

/**
 * @brief Returns frame graph
 *
 * @return Pointer to the result, or nullptr on failure
 */
fg::FrameGraph *SDLRenderer::GetFrameGraph() noexcept {
    return frame_graph_.has_value() ? &*frame_graph_ : nullptr;
}

/**
 * @brief Returns compiled graph
 *
 * @return Pointer to the result, or nullptr on failure
 */
fg::CompiledGraph *SDLRenderer::GetCompiledGraph() noexcept {
    return (frame_graph_.has_value() && !compiled_graph_.empty()) ? &compiled_graph_ : nullptr;
}

/// Lazily load, compile, and cache a node shader pipeline.
/// Reuses ui_rect_vert_ (already compiled) plus a custom fragment shader
/// loaded from data/shaders/{platform}/{name}.frag.{ext}.
SDL_GPUGraphicsPipeline
    *
    /**
     * @brief Ensure node shader pipeline
     *
     * @param name  Human-readable name or identifier string
     *
     * @return Pointer to the result, or nullptr on failure
     */
    SDLRenderer::ensureNodeShaderPipeline(const std::string &name) noexcept {
    // Cache hit (including previously-failed loads stored as null).
    auto it = node_shader_cache_.find(name);
    if (it != node_shader_cache_.end())
        return it->second.pipeline;

    if (!device_ || !ui_rect_vert_) {
        node_shader_cache_[name] = {};
        return nullptr;
    }

    // Determine shader file path and GPU format from what the device supports.
    SDL_GPUShaderFormat avail = SDL_GetGPUShaderFormats(device_);
    std::string frag_path;
    SDL_GPUShaderFormat fmt = SDL_GPU_SHADERFORMAT_INVALID;

    if (avail & SDL_GPU_SHADERFORMAT_MSL) {
        frag_path = data_base_path_ + "data/shaders/msl/" + name + ".frag.metal";
        fmt       = SDL_GPU_SHADERFORMAT_MSL;
    } else if (avail & SDL_GPU_SHADERFORMAT_SPIRV) {
        frag_path = data_base_path_ + "data/shaders/spirv/" + name + ".frag.spv";
        fmt       = SDL_GPU_SHADERFORMAT_SPIRV;
    } else {
        std::cerr << "[NodeShader] no supported shader format for '" << name << "'\n";
        node_shader_cache_[name] = {};
        return nullptr;
    }

    // Load source / bytecode.
    std::string source;
    std::vector<Uint8> binary;
    const Uint8 *code     = nullptr;
    std::size_t code_size = 0;

    if (fmt == SDL_GPU_SHADERFORMAT_MSL) {
        source = ReadTextFile(frag_path);
        if (source.empty()) {
            std::cerr << "[NodeShader] shader file not found: " << frag_path << "\n";
            node_shader_cache_[name] = {};
            return nullptr;
        }
        code      = reinterpret_cast<const Uint8 *>(source.data());
        code_size = source.size();
    } else {
        // SPIRV — read raw bytes.
        std::ifstream f(frag_path, std::ios::binary | std::ios::ate);
        if (!f) {
            std::cerr << "[NodeShader] shader file not found: " << frag_path << "\n";
            node_shader_cache_[name] = {};
            return nullptr;
        }
        const auto sz = f.tellg();
        f.seekg(0);
        binary.resize(static_cast<std::size_t>(sz));
        f.read(reinterpret_cast<char *>(binary.data()), sz);
        code      = binary.data();
        code_size = binary.size();
    }

    // Compile fragment shader.
    SDL_GPUShaderCreateInfo fci{};
    fci.code                = code;
    fci.code_size           = code_size;
    fci.entrypoint          = "main0";
    fci.format              = fmt;
    fci.stage               = SDL_GPU_SHADERSTAGE_FRAGMENT;
    fci.num_samplers        = 1;
    fci.num_uniform_buffers = 1;
    fci.props               = 0;

    SDL_GPUShader *frag = SDL_CreateGPUShader(device_, &fci);
    if (!frag) {
        std::cerr << "[NodeShader] compile failed for '" << name << "': " << SDL_GetError() << "\n";
        node_shader_cache_[name] = {};
        return nullptr;
    }

    // Build pipeline — same blend state as the existing text/image pipeline.
    SDL_GPUColorTargetDescription colorDesc{};
    colorDesc.format                   = SDL_GetGPUSwapchainTextureFormat(device_, sdl_window_);
    colorDesc.blend_state.enable_blend = true;
    colorDesc.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    colorDesc.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    colorDesc.blend_state.color_blend_op        = SDL_GPU_BLENDOP_ADD;
    colorDesc.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    colorDesc.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    colorDesc.blend_state.alpha_blend_op        = SDL_GPU_BLENDOP_ADD;

    SDL_GPUGraphicsPipelineTargetInfo targetInfo{};
    targetInfo.color_target_descriptions = &colorDesc;
    targetInfo.num_color_targets         = 1;

    SDL_GPUVertexInputState vis{};
    vis.vertex_buffer_descriptions = nullptr;
    vis.num_vertex_buffers         = 0;
    vis.vertex_attributes          = nullptr;
    vis.num_vertex_attributes      = 0;

    SDL_GPUGraphicsPipelineCreateInfo pci{};
    pci.vertex_shader      = ui_rect_vert_;
    pci.fragment_shader    = frag;
    pci.vertex_input_state = vis;
    pci.primitive_type     = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pci.target_info        = targetInfo;
    pci.props              = 0;

    SDL_GPUGraphicsPipeline *pipe = SDL_CreateGPUGraphicsPipeline(device_, &pci);
    SDL_ReleaseGPUShader(device_, frag);  // pipeline holds the only ref

    if (!pipe) {
        std::cerr << "[NodeShader] pipeline creation failed for '" << name
                  << "': " << SDL_GetError() << "\n";
        node_shader_cache_[name] = {};
        return nullptr;
    }

    node_shader_cache_[name] = {pipe};
    std::cerr << "[NodeShader] loaded '" << name << "' (" << frag_path << ")\n";
    return pipe;
}

/**
 * @brief Shuts down
 */
void SDLRenderer::Shutdown() noexcept {
    // Run GPU resource cleanup before the device is destroyed.
    if (gpu_pre_shutdown_hook_) {
        gpu_pre_shutdown_hook_();
        gpu_pre_shutdown_hook_ = nullptr;
    }
    scene3d_hook_ = nullptr;

    if (!device_) {
        initialized_.store(false);
        return;
    }

    // Text renderer and image cache both hold GPU textures against this device
    // — shut them down before releasing pipelines and the device itself.
    if (text_renderer_) {
        text_renderer_->shutdown();
        text_renderer_.reset();
    }

    if (image_cache_) {
        image_cache_->shutdown();
        image_cache_.reset();
    }

    scene_tree_ = nullptr;
    scene_root_ = k_null_handle;

    if (ui_texture_) {
        SDL_ReleaseGPUTexture(device_, ui_texture_);
        ui_texture_   = nullptr;
        ui_texture_w_ = 0;
        ui_texture_h_ = 0;
    }

    if (ui_rect_pipeline_) {
        SDL_ReleaseGPUGraphicsPipeline(device_, ui_rect_pipeline_);
        ui_rect_pipeline_ = nullptr;
    }
    if (ui_text_pipeline_) {
        SDL_ReleaseGPUGraphicsPipeline(device_, ui_text_pipeline_);
        ui_text_pipeline_ = nullptr;
    }
    if (ui_rect_vert_) {
        SDL_ReleaseGPUShader(device_, ui_rect_vert_);
        ui_rect_vert_ = nullptr;
    }
    if (ui_rect_frag_) {
        SDL_ReleaseGPUShader(device_, ui_rect_frag_);
        ui_rect_frag_ = nullptr;
    }
    if (ui_text_frag_) {
        SDL_ReleaseGPUShader(device_, ui_text_frag_);
        ui_text_frag_ = nullptr;
    }

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

    // Release lazily-compiled node shader pipelines.
    for (auto &[unused_name, entry] : node_shader_cache_) {
        if (entry.pipeline)
            SDL_ReleaseGPUGraphicsPipeline(device_, entry.pipeline);
    }
    node_shader_cache_.clear();

    if (video_texture_) {
        video_texture_->shutdown();
        video_texture_.reset();
    }

    // Release wavetable texture (1D sin wave lookup).
    if (wavetable_texture_) {
        SDL_ReleaseGPUTexture(device_, wavetable_texture_);
        wavetable_texture_ = nullptr;
    }

    // Release frame graph resources (ResourcePool textures + ShaderLibrary PSOs)
    // BEFORE the GPU device is destroyed.  The CompiledGraph holds only
    // non-owning pointers — clear it first so no dangling access can happen
    // even if a destructor elsewhere triggers an indirect execute().
    compiled_graph_ = {};
    frame_graph_.reset();
    fg_needs_compile_ = false;

    // Release window claim before destroying the device.
    if (sdl_window_) {
        SDL_ReleaseWindowFromGPUDevice(device_, sdl_window_);
        sdl_window_ = nullptr;
    }

    SDL_DestroyGPUDevice(device_);
    device_ = nullptr;

    initialized_.store(false);
    shader_path_.clear();
    shader_mtime_ = 0;

    std::cerr << "SDLRenderer::Shutdown - resources released\n";
}

// ---- HiDPI pixel scale ---------------------------------------------------

/**
 * @brief Updates pixel scale
 */
void SDLRenderer::UpdatePixelScale() noexcept {
    if (!sdl_window_)
        return;

    int lw = 0, lh = 0, pw = 0, ph = 0;
    SDL_GetWindowSize(sdl_window_, &lw, &lh);
    SDL_GetWindowSizeInPixels(sdl_window_, &pw, &ph);

    pixel_scale_x_ = (lw > 0 && pw > 0) ? static_cast<float>(pw) / static_cast<float>(lw) : 1.f;
    pixel_scale_y_ = (lh > 0 && ph > 0) ? static_cast<float>(ph) / static_cast<float>(lh) : 1.f;
}

/**
 * @brief Refresh pixel scale
 */
void SDLRenderer::RefreshPixelScale() noexcept {
    UpdatePixelScale();
}

// ---- UI offscreen texture ------------------------------------------------

/**
 * @brief Creates and returns or resize ui texture
 *
 * @param w  Width in logical pixels
 * @param h  Opaque resource handle
 *
 * @return true on success, false on failure
 */
bool SDLRenderer::CreateOrResizeUITexture(Uint32 w, Uint32 h) noexcept {
    if (!device_ || w == 0 || h == 0)
        return false;
    if (ui_texture_ && ui_texture_w_ == w && ui_texture_h_ == h)
        return true;

    // Release the old texture first (safe to call even if null).
    if (ui_texture_) {
        SDL_ReleaseGPUTexture(device_, ui_texture_);
        ui_texture_   = nullptr;
        ui_texture_w_ = 0;
        ui_texture_h_ = 0;
    }

    // Use the swapchain format so the existing UI pipelines (built for that
    // format) can render into this texture without a separate pipeline variant.
    SDL_GPUTextureFormat fmt = SDL_GetGPUSwapchainTextureFormat(device_, sdl_window_);
    if (fmt == SDL_GPU_TEXTUREFORMAT_INVALID)
        fmt = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;  // safe fallback

    SDL_GPUTextureCreateInfo tci{};
    tci.type                 = SDL_GPU_TEXTURETYPE_2D;
    tci.format               = fmt;
    tci.usage                = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    tci.width                = w;
    tci.height               = h;
    tci.layer_count_or_depth = 1;
    tci.num_levels           = 1;
    tci.sample_count         = SDL_GPU_SAMPLECOUNT_1;
    tci.props                = 0;

    ui_texture_ = SDL_CreateGPUTexture(device_, &tci);
    if (!ui_texture_) {
        std::cerr << "SDLRenderer: failed to create UI texture (" << w << "×" << h
                  << "): " << SDL_GetError() << "\n";
        return false;
    }

    ui_texture_w_ = w;
    ui_texture_h_ = h;
    std::cerr << "SDLRenderer: UI texture " << w << "×" << h << "\n";
    return true;
}

/**
 * @brief Sets scene
 *
 * @param tree  Red channel component [0, 1]
 * @param root  Red channel component [0, 1]
 *
 * @warning Parameter 'tree' is a non-const raw pointer — Raw pointer parameter —
 *          ownership is ambiguous; consider std::span (non-owning view),
 *          std::unique_ptr (transfer), or const T* (borrow)
 */
void SDLRenderer::SetScene(RenderTree *tree, NodeHandle root) noexcept {
    scene_tree_ = tree;
    scene_root_ = root;
}

/**
 * @brief Creates and returns ui pipelines
 *
 * @return true on success, false on failure
 */
bool SDLRenderer::CreateUIPipelines() {
    if (!device_ || !sdl_window_)
        return false;

    //  Load shader sources

    std::string vert_src = ReadTextFile("assets/shaders/ui_rect.vert.metal");
    if (vert_src.empty()) {
        // Minimal inline fallback (matches ui_rect.vert.metal exactly).
        vert_src = R"(
#include <metal_stdlib>
using namespace metal;
struct RectUniform { float x,y,w,h,vw,vh,uv_x,uv_y,uv_w,uv_h,_p0,_p1; };
struct VertOut { float4 position [[position]]; float2 uv [[user(locn0)]]; };
static inline float2 pxn(float2 p,float vw,float vh){
    return float2((p.x/vw)*2.0f-1.0f,-(p.y/vh)*2.0f+1.0f);
}
vertex VertOut main0(uint vid[[vertex_id]],constant RectUniform&r[[buffer(0)]]){
    float2 A=float2(r.x,r.y),B=float2(r.x+r.w,r.y);
    float2 C=float2(r.x+r.w,r.y+r.h),D=float2(r.x,r.y+r.h);
    float2 vs[6]={A,B,D,B,C,D};
    float2 us[6]={float2(0,0),float2(1,0),float2(0,1),float2(1,0),float2(1,1),float2(0,1)};
    VertOut o; o.position=float4(pxn(vs[vid],r.vw,r.vh),0,1);
    o.uv=float2(r.uv_x+us[vid].x*r.uv_w, r.uv_y+us[vid].y*r.uv_h); return o;
}
)";
    }

    std::string rect_frag_src = ReadTextFile("assets/shaders/ui_rect.frag.metal");
    if (rect_frag_src.empty()) {
        rect_frag_src = R"(
#include <metal_stdlib>
using namespace metal;
struct ColorUniform { float r,g,b,a; };
struct VertOut { float4 position [[position]]; float2 uv [[user(locn0)]]; };
fragment float4 main0(VertOut in[[stage_in]],constant ColorUniform&c[[buffer(0)]]){
    return float4(c.r,c.g,c.b,c.a);
}
)";
    }

    std::string text_frag_src = ReadTextFile("assets/shaders/ui_text.frag.metal");
    if (text_frag_src.empty()) {
        text_frag_src = R"(
#include <metal_stdlib>
using namespace metal;
struct TintUniform { float r,g,b,a; };
struct VertOut { float4 position [[position]]; float2 uv [[user(locn0)]]; };
fragment float4 main0(VertOut in[[stage_in]],
                      texture2d<float> atlas[[texture(0)]],
                      sampler samp[[sampler(0)]],
                      constant TintUniform&tint[[buffer(0)]]){
    return atlas.sample(samp,in.uv)*float4(tint.r,tint.g,tint.b,tint.a);
}
)";
    }

    //  Compile shaders

    auto make_shader = [&](const std::string &src,
                           SDL_GPUShaderStage stage,
                           Uint32 num_samplers,
                           Uint32 num_uniform_buffers) -> SDL_GPUShader * {
        SDL_GPUShaderCreateInfo ci{};
        ci.code                = reinterpret_cast<const Uint8 *>(src.data());
        ci.code_size           = src.size();
        ci.entrypoint          = "main0";
        ci.format              = SDL_GPU_SHADERFORMAT_MSL;
        ci.stage               = stage;
        ci.num_samplers        = num_samplers;
        ci.num_uniform_buffers = num_uniform_buffers;
        ci.props               = 0;
        return SDL_CreateGPUShader(device_, &ci);
    };

    ui_rect_vert_ = make_shader(vert_src, SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
    ui_rect_frag_ = make_shader(rect_frag_src, SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 1);
    ui_text_frag_ = make_shader(text_frag_src, SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);

    if (!ui_rect_vert_ || !ui_rect_frag_ || !ui_text_frag_) {
        std::cerr << "SDLRenderer::CreateUIPipelines - shader compile failed: " << SDL_GetError()
                  << "\n";
        if (ui_rect_vert_) {
            SDL_ReleaseGPUShader(device_, ui_rect_vert_);
            ui_rect_vert_ = nullptr;
        }
        if (ui_rect_frag_) {
            SDL_ReleaseGPUShader(device_, ui_rect_frag_);
            ui_rect_frag_ = nullptr;
        }
        if (ui_text_frag_) {
            SDL_ReleaseGPUShader(device_, ui_text_frag_);
            ui_text_frag_ = nullptr;
        }
        return false;
    }

    //  Common pipeline state

    // No vertex buffer: all geometry is generated in the vertex shader.
    SDL_GPUVertexInputState vis{};
    vis.vertex_buffer_descriptions = nullptr;
    vis.num_vertex_buffers         = 0;
    vis.vertex_attributes          = nullptr;
    vis.num_vertex_attributes      = 0;

    // Straight-alpha blending (SRC_ALPHA / ONE_MINUS_SRC_ALPHA).
    SDL_GPUColorTargetDescription colorDesc{};
    colorDesc.format                   = SDL_GetGPUSwapchainTextureFormat(device_, sdl_window_);
    colorDesc.blend_state.enable_blend = true;
    colorDesc.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    colorDesc.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    colorDesc.blend_state.color_blend_op        = SDL_GPU_BLENDOP_ADD;
    colorDesc.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    colorDesc.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    colorDesc.blend_state.alpha_blend_op        = SDL_GPU_BLENDOP_ADD;

    SDL_GPUGraphicsPipelineTargetInfo targetInfo{};
    targetInfo.color_target_descriptions = &colorDesc;
    targetInfo.num_color_targets         = 1;
    targetInfo.has_depth_stencil_target  = false;

    //  ui_rect pipeline

    SDL_GPUGraphicsPipelineCreateInfo rpci{};
    rpci.vertex_shader      = ui_rect_vert_;
    rpci.fragment_shader    = ui_rect_frag_;
    rpci.primitive_type     = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    rpci.vertex_input_state = vis;
    rpci.target_info        = targetInfo;

    ui_rect_pipeline_ = SDL_CreateGPUGraphicsPipeline(device_, &rpci);
    if (!ui_rect_pipeline_) {
        std::cerr << "SDLRenderer::CreateUIPipelines - rect pipeline failed: " << SDL_GetError()
                  << "\n";
        SDL_ReleaseGPUShader(device_, ui_rect_vert_);
        ui_rect_vert_ = nullptr;
        SDL_ReleaseGPUShader(device_, ui_rect_frag_);
        ui_rect_frag_ = nullptr;
        SDL_ReleaseGPUShader(device_, ui_text_frag_);
        ui_text_frag_ = nullptr;
        return false;
    }

    //  ui_text pipeline (same vert, sampled frag)

    SDL_GPUGraphicsPipelineCreateInfo tpci{};
    tpci.vertex_shader      = ui_rect_vert_;  // shared vertex shader
    tpci.fragment_shader    = ui_text_frag_;
    tpci.primitive_type     = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    tpci.vertex_input_state = vis;
    tpci.target_info        = targetInfo;

    ui_text_pipeline_ = SDL_CreateGPUGraphicsPipeline(device_, &tpci);
    if (!ui_text_pipeline_) {
        std::cerr << "SDLRenderer::CreateUIPipelines - text pipeline failed: " << SDL_GetError()
                  << "\n";
        SDL_ReleaseGPUGraphicsPipeline(device_, ui_rect_pipeline_);
        ui_rect_pipeline_ = nullptr;
        SDL_ReleaseGPUShader(device_, ui_rect_vert_);
        ui_rect_vert_ = nullptr;
        SDL_ReleaseGPUShader(device_, ui_rect_frag_);
        ui_rect_frag_ = nullptr;
        SDL_ReleaseGPUShader(device_, ui_text_frag_);
        ui_text_frag_ = nullptr;
        return false;
    }

    std::cerr << "SDLRenderer::CreateUIPipelines - rect + text pipelines ready\n";
    return true;
}

/**
 * @brief Renders
 *
 * @param timeSeconds  Interpolation parameter in [0, 1]
 */
void SDLRenderer::Render(double timeSeconds) {
    if (!initialized_.load() || !device_) {
        return;
    }

    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(device_);
    if (!cmd) {
        std::cerr << "SDLRenderer::Render - SDL_AcquireGPUCommandBuffer failed: " << SDL_GetError()
                  << "\n";
        return;
    }

    SDL_GPUTexture *swap = nullptr;
    Uint32 width = 0, height = 0;

    if (!SDL_AcquireGPUSwapchainTexture(cmd, sdl_window_, &swap, &width, &height)) {
        std::cerr << "SDLRenderer::Render - SDL_AcquireGPUSwapchainTexture error: "
                  << SDL_GetError() << "\n";
        SDL_SubmitGPUCommandBuffer(cmd);
        return;
    }

    if (!swap) {
        SDL_SubmitGPUCommandBuffer(cmd);
        return;
    }

    //  Deferred pipeline rebuild (SubmitPipelineSource)
    // SubmitPipelineSource() stores a new pipeline.pug source and sets a flag.
    // We process it here — at the top of the frame, before any render pass —
    // so GPU object creation always happens on the render thread, and the old
    // pipeline finishes its current frame before being replaced.
    //
    // This mirrors the "render command queue" pattern used in production engines
    // (UE5's ENQUEUE_RENDER_COMMAND, Unity's RenderThread.Execute, bgfx's
    // frame-boundary flush): the calling thread enqueues a description;
    // the render thread executes it at a safe boundary.
    //
    // In sdlos the "queue" is a single pending string (latest-wins) because
    // pipeline rebuilds are driven by UI interaction, not by high-frequency
    // game simulation — at most one rebuild per user gesture is needed.
    if (pending_pipeline_dirty_) {
        pending_pipeline_dirty_ = false;

        std::string shader_dir;
        {
            const char *platform_dir = "spv";
            if (shader_format_ & SDL_GPU_SHADERFORMAT_MSL)
                platform_dir = "msl";
            else if (shader_format_ & SDL_GPU_SHADERFORMAT_DXIL)
                platform_dir = "dxil";
            shader_dir  = data_base_path_;
            shader_dir += "data/shaders/";
            shader_dir += platform_dir;
            shader_dir += '/';
        }

        int pw = 0, ph = 0;
        if (sdl_window_)
            SDL_GetWindowSizeInPixels(sdl_window_, &pw, &ph);

        auto result = fg::FrameGraph::from_pug(
            pending_pipeline_source_,
            device_,
            shader_dir,
            shader_format_,
            static_cast<uint32_t>(pw),
            static_cast<uint32_t>(ph));

        if (result) {
            compiled_graph_   = {};
            frame_graph_      = std::move(*result);
            fg_needs_compile_ = true;
            fg_compiled_w_    = 0;
            fg_compiled_h_    = 0;
            fg_swapchain_fmt_ = SDL_GPU_TEXTUREFORMAT_INVALID;
            SDL_Log(
                "[SDLRenderer] SubmitPipelineSource: %zu passes compiled",
                frame_graph_->passes().size());
        } else {
            SDL_Log("[SDLRenderer] SubmitPipelineSource failed: %s", result.error().c_str());
        }

        pending_pipeline_source_.clear();
    }

    //  Frame graph: zombie prevention — recompile before execute()
    //
    // CompiledGraph holds raw SDL_GPUTexture* pointers into the ResourcePool.
    // If the swapchain was resized since the last compile, those pointers are
    // stale (the pool already freed them).  We MUST call resize() + compile()
    // here — before any execute() — to obtain fresh pointers.
    //
    // Additional triggers: first frame after LoadPipeline (fg_needs_compile_)
    // and swapchain format change (extremely rare but possible on display
    // reconfiguration).
    if (frame_graph_.has_value()) {
        const bool size_changed = (width != fg_compiled_w_ || height != fg_compiled_h_);

        // SDL_GetGPUSwapchainTextureFormat is only called when we actually
        // need to (re)compile — not every frame.  On the steady-state hot
        // path (nothing changed) the check short-circuits here with zero
        // API calls.  The format is cached in fg_swapchain_fmt_ after the
        // first successful compile and changes only on display reconfiguration.
        const bool needs_compile = fg_needs_compile_ || size_changed
                                || (fg_swapchain_fmt_ == SDL_GPU_TEXTUREFORMAT_INVALID);

        if (needs_compile) {
            const SDL_GPUTextureFormat sc_fmt =
                SDL_GetGPUSwapchainTextureFormat(device_, sdl_window_);

            const bool fmt_changed = (sc_fmt != fg_swapchain_fmt_);

            if (fg_needs_compile_ || size_changed || fmt_changed) {
                // resize() releases stale swapchain-sized textures from the
                // pool.  Do this before compile() so the new allocations use
                // the correct dimensions.
                if (size_changed)
                    frame_graph_->resize(width, height);

                fg_swapchain_fmt_ = sc_fmt;
                fg_compiled_w_    = width;
                fg_compiled_h_    = height;

                // compile() re-acquires textures from the pool and resolves
                // all pointers — compiled_graph_ now has live, non-dangling
                // handles.
                compiled_graph_   = frame_graph_->compile(fg_swapchain_fmt_);
                fg_needs_compile_ = false;

                SDL_Log(
                    "[SDLRenderer] frame graph compiled: %zu passes (%zu active)",
                    compiled_graph_.pass_count(),
                    compiled_graph_.active_count());
            }
        }
    }

    //  Ensure persistent UI texture exists and matches swapchain size
    // A size mismatch means the window was resized.  Recreate the texture and
    // force a full repaint so no node renders into the wrong-sized buffer.
    if (ui_texture_w_ != width || ui_texture_h_ != height) {
        if (CreateOrResizeUITexture(width, height)) {
            // Layout cascade from root resize will dirty the whole tree via
            // markLayoutDirty → resolveLayout → child dirty flags.  Call
            // forceAllDirty as an additional safety net (no-op if already set).
            if (scene_tree_ && scene_root_.valid())
                scene_tree_->forceAllDirty(scene_root_);
        }
    }

    //  Pass 1: copy pass — flush pending texture uploads
    // Both the text renderer (glyph atlas) and the image cache may have
    // surfaces queued from the previous frame's draw callbacks.  Open a
    // single copy pass and drain both queues — SDL3 GPU guarantees that
    // upload commands recorded here complete before the render pass begins.
    {
        // Pull the latest camera frame (CPU-side) before opening the copy pass.
        if (video_texture_)
            video_texture_->updateFrame();

        const bool needText  = text_renderer_ && text_renderer_->hasPendingUploads();
        const bool needImage = image_cache_ && image_cache_->hasPendingUploads();
        const bool needVideo = video_texture_ && video_texture_->hasPendingUpload();

        if (needText || needImage || needVideo) {
            SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
            if (cp) {
                if (needText)
                    text_renderer_->flushUploads(cp);
                if (needImage)
                    image_cache_->flushUploads(cp);
                if (needVideo)
                    video_texture_->flushUpload(cp);
                SDL_EndGPUCopyPass(cp);
            }
        }
    }

    //  Pass 2: UI offscreen render pass
    //
    // Renders the scene into ui_texture_ (persistent, same format as the
    // swapchain) with SDL_GPU_LOADOP_CLEAR on active frames.
    //
    // IDLE-FRAME SKIP: when no node has dirty_render == true, this entire
    // pass is omitted.  ui_texture_ retains its content from the last active
    // frame and is composited as-is in Pass 3 — zero UI GPU commands issued.
    //
    // FULL-REPAINT POLICY: if *any* node is dirty we call forceAllDirty()
    // before rendering.  This guarantees every node re-emits its draw commands
    // into the freshly-cleared texture, avoiding alpha-compositing artefacts
    // that would occur if only a subset of overlapping nodes repainted.
    //
    // The "touched node self-marks dirty" contract is therefore real and
    // meaningful: static nodes never install an update callback that sets
    // dirty_render; only setStyle() / markDirty() / Animated<T> ticks do.

    const bool scene_active =
        scene_tree_ && scene_root_.valid() && ui_rect_pipeline_ && ui_text_pipeline_ && ui_texture_;

    if (scene_active) {
        //  Update root dimensions (may trigger markLayoutDirty)
        {
            RenderNode *root = scene_tree_->node(scene_root_);
            if (root) {
                // Size the root node to the physical backbuffer dimensions.
                // This ensures the RenderTree matches the UI texture size
                // (ui_texture_w_ / ui_texture_h_) which is also physical pixels.
                const float pw = static_cast<float>(width);
                const float ph = static_cast<float>(height);
                if (root->w != pw || root->h != ph) {
                    root->w = pw;
                    root->h = ph;
                    // Also update layout_props so the layout engine knows the base logical size.
                    // This is especially important for the first frame's layout pass.
                    root->layout_props.width  = pw / pixel_scale_x_;
                    root->layout_props.height = ph / pixel_scale_y_;
                    scene_tree_->markLayoutDirty(scene_root_);
                }
            }
        }

        //  Layout + update callbacks (may set dirty_render)
        scene_tree_->beginFrame();
        scene_tree_->update(scene_root_);

        //  Check dirty and render if needed
        // We force a UI repaint if anything is dirty. On HiDPI we must
        // ensure Pass 2 actually runs and fills ui_texture_.
        if (scene_tree_->anyDirty(scene_root_)) {
            // Propagate dirty to every node so the full scene repaints into
            // the freshly-cleared texture, avoiding alpha-compositing artefacts
            // that would occur if only a subset of overlapping nodes repainted.
            scene_tree_->forceAllDirty(scene_root_);

            SDL_GPUColorTargetInfo ui_ct{};
            ui_ct.texture     = ui_texture_;
            ui_ct.load_op     = SDL_GPU_LOADOP_CLEAR;
            ui_ct.store_op    = SDL_GPU_STOREOP_STORE;
            ui_ct.clear_color = {0.f, 0.f, 0.f, 0.f};  // transparent black
            ui_ct.cycle       = true;

            SDL_GPURenderPass *ui_pass = SDL_BeginGPURenderPass(cmd, &ui_ct, 1, nullptr);
            if (ui_pass) {
                RenderContext ctx;
                ctx.backend       = GPUBackend::Metal;
                ctx.device        = device_;
                ctx.cmd           = cmd;
                ctx.pass          = ui_pass;
                ctx.viewport_w    = static_cast<float>(width);
                ctx.viewport_h    = static_cast<float>(height);
                ctx.arena         = &scene_tree_->arena();
                ctx.text_renderer = text_renderer_.get();
                ctx.image_cache   = image_cache_.get();
                ctx.video_texture = video_texture_.get();

                ctx.pipelines["rect"]  = ui_rect_pipeline_;
                ctx.pipelines["text"]  = ui_text_pipeline_;
                ctx.time               = static_cast<float>(timeSeconds);
                ctx.nodeShaderPipeline = [this](std::string_view n) noexcept {
                    return ensureNodeShaderPipeline(std::string(n));
                };

                scene_tree_->render(scene_root_, ctx);
                SDL_EndGPURenderPass(ui_pass);
            }
        }
        // else: nothing dirty — ui_texture_ already holds the correct frame.
    }

    //  Pass 3: swapchain render pass — wallpaper + composite UI
    //
    // The wallpaper always redraws (it's an animated FBM shader).
    // The UI texture is then alpha-composited on top using the text pipeline
    // (textured quad, straight-alpha blend, tint = {1,1,1,1}).

    //  Pass 3a: background — frame graph OR built-in FBM wallpaper
    //
    // When pipeline.pug is loaded and compiled with ≥1 passes:
    //   CompiledGraph::execute() drives the scene.  Each enabled pass opens
    //   and closes its own SDL_GPURenderPass; the final pass writes to the
    //   swapchain (pass.output == nullptr sentinel).  Any "time" param is
    //   injected per-frame via a stack copy — no heap traffic.
    //
    // When pipeline.pug is loaded but declares NO passes (empty pipeline):
    //   Interpreted as "suppress FBM — plain dark background".  A single
    //   render pass clears the swapchain to a neutral dark colour with no
    //   shader draws.  Useful for apps that supply their own page backgrounds
    //   and do not want the animated wallpaper (e.g. the styleguide).
    //
    // When no pipeline.pug (or compile failed):
    //   The built-in animated FBM wallpaper runs in a single render pass
    //   with LOADOP_CLEAR — preserving the original behaviour exactly.
    SDL_GPURenderPass *pass = nullptr;

    if (frame_graph_.has_value() && !compiled_graph_.empty()) {
        // execute() manages its own SDL_GPURenderPass per enabled pass.
        // On success, `swap` contains the frame graph's final output.
        // On a pass failure (null pipeline/target), execute() skips that
        // pass in O(1) — no crash, no partial GPU state.
        compiled_graph_.execute(cmd, swap, width, height, static_cast<float>(timeSeconds));

    } else if (frame_graph_.has_value()) {
        // pipeline.pug loaded but has zero passes → plain solid-colour clear.
        // No FBM, no shader draw — just wipe the swapchain to a neutral dark bg.
        SDL_GPUColorTargetInfo ct{};
        ct.texture     = swap;
        ct.load_op     = SDL_GPU_LOADOP_CLEAR;
        ct.store_op    = SDL_GPU_STOREOP_STORE;
        ct.clear_color = {0.08f, 0.08f, 0.10f, 1.f};
        ct.cycle       = true;

        pass = SDL_BeginGPURenderPass(cmd, &ct, 1, nullptr);
        if (!pass) {
            std::cerr << "SDLRenderer::Render - SDL_BeginGPURenderPass (solid clear) failed: "
                      << SDL_GetError() << "\n";
            SDL_SubmitGPUCommandBuffer(cmd);
            return;
        }
        SDL_EndGPURenderPass(pass);
        pass = nullptr;

    } else {
        SDL_GPUColorTargetInfo ct{};
        ct.texture     = swap;
        ct.load_op     = SDL_GPU_LOADOP_CLEAR;
        ct.store_op    = SDL_GPU_STOREOP_STORE;
        ct.clear_color = {0.f, 0.f, 0.f, 1.f};
        ct.cycle       = true;

        pass = SDL_BeginGPURenderPass(cmd, &ct, 1, nullptr);
        if (!pass) {
            std::cerr << "SDLRenderer::Render - SDL_BeginGPURenderPass failed: " << SDL_GetError()
                      << "\n";
            SDL_SubmitGPUCommandBuffer(cmd);
            return;
        }

        // 3a. Wallpaper (fullscreen FBM, 3 vertices, no buffer)
        SDL_BindGPUGraphicsPipeline(pass, pipeline_);

        FragmentUniform fu{};
        fu.time   = static_cast<float>(timeSeconds);
        fu.pad[0] = fu.pad[1] = fu.pad[2] = 0.0f;
        SDL_PushGPUFragmentUniformData(cmd, 0, &fu, sizeof(fu));

        SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
        SDL_EndGPURenderPass(pass);
        pass = nullptr;
    }

    // 3b. 3D scene pre-pass (optional)
    // The hook begins its own render pass (LOADOP_LOAD on swap) with a
    // depth buffer. Runs after the wallpaper but before UI composite.
    if (scene3d_hook_) {
        scene3d_hook_(cmd, swap, static_cast<float>(width), static_cast<float>(height));
    }

    // 3c. UI composite (re-open swapchain pass, LOADOP_LOAD)
    // Reuses the text/image pipeline: samples a texture and multiplies by a
    // tint uniform.  Tint {1,1,1,1} preserves the UI texture's RGBA exactly.
    // The pipeline has straight-alpha blending enabled so transparent UI
    // regions correctly reveal the wallpaper underneath.
    if (ui_texture_ && ui_text_pipeline_) {
        SDL_GPUColorTargetInfo ct2{};
        ct2.texture  = swap;
        ct2.load_op  = SDL_GPU_LOADOP_LOAD;  // preserve wallpaper + 3D scene
        ct2.store_op = SDL_GPU_STOREOP_STORE;
        ct2.cycle    = false;

        pass = SDL_BeginGPURenderPass(cmd, &ct2, 1, nullptr);
        if (pass) {
            SDL_BindGPUGraphicsPipeline(pass, ui_text_pipeline_);

            // Must be 48 bytes — matches struct RectUniform in the vertex shader
            // { x, y, w, h, vw, vh, uv_x, uv_y, uv_w, uv_h, _p0, _p1 }.
            // Previously only 32 bytes were pushed (uv_x/y/w/h missing), causing
            // Metal to read zeros for uv_w/uv_h → all 6 vertices sampled UV (0,0)
            // → the entire UI texture collapsed to one transparent pixel → clouds only.
            struct alignas(4) RectUniform {
                float x, y, w, h;
                float vw, vh;
                float uv_x, uv_y;
                float uv_w, uv_h;
                float _p0, _p1;
            };
            static_assert(
                sizeof(RectUniform) == 48,
                "RectUniform must be 48 bytes to match the vertex shader");
            const RectUniform vu{
                0.f,
                0.f,
                static_cast<float>(width),
                static_cast<float>(height),
                static_cast<float>(width),
                static_cast<float>(height),
                0.f,
                0.f,
                1.f,
                1.f,  // uv_x=0, uv_y=0, uv_w=1, uv_h=1 — full texture
                0.f,
                0.f};
            SDL_PushGPUVertexUniformData(cmd, 0, &vu, sizeof(vu));

            struct alignas(4) TintUniform {
                float r, g, b, a;
            };
            const TintUniform tu{1.f, 1.f, 1.f, 1.f};
            SDL_PushGPUFragmentUniformData(cmd, 0, &tu, sizeof(tu));

            // Prefer image_cache sampler; fall back to text_renderer sampler.
            // Both are bilinear / clamp-to-edge — identical result at 1:1 mapping.
            SDL_GPUSampler *samp = image_cache_   ? image_cache_->sampler()
                                 : text_renderer_ ? text_renderer_->sampler()
                                                  : nullptr;
            if (samp) {
                SDL_GPUTextureSamplerBinding sb{};
                sb.texture = ui_texture_;
                sb.sampler = samp;
                SDL_BindGPUFragmentSamplers(pass, 0, &sb, 1);
                SDL_DrawGPUPrimitives(pass, 6, 1, 0, 0);
            }
            SDL_EndGPURenderPass(pass);
        }
    }

    // Do NOT call SDL_ReleaseGPUTexture on `swap` — swapchain textures are
    // non-owning views; releasing them corrupts the Metal pending-destroy queue.
    SDL_SubmitGPUCommandBuffer(cmd);
}

/**
 * @brief Reload shader
 *
 * @param path  Filesystem path
 *
 * @return true on success, false on failure
 */
bool SDLRenderer::ReloadShader(const std::string &path) {
    std::error_code ec;

    if (!fs::exists(path, ec) || ec) {
        std::cerr << "SDLRenderer::ReloadShader - shader file not found: " << path << "\n";
        return false;
    }

    const auto mtime = GetFileMTime(path);
    if (mtime == shader_mtime_) {
        return true;  // unchanged — nothing to do
    }

    const std::string source = ReadTextFile(path);
    if (source.empty()) {
        std::cerr << "SDLRenderer::ReloadShader - failed to read shader: " << path << "\n";
        return false;
    }

    std::string vertSource;
    if (!vertex_shader_path_.empty() && fs::exists(vertex_shader_path_, ec) && !ec) {
        vertSource = ReadTextFile(vertex_shader_path_);
    }

    if (vertSource.empty()) {
        std::cerr << "SDLRenderer::ReloadShader - fallback to build-in\n";
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

    if (!CreatePipeline(vertSource, source)) {
        std::cerr << "SDLRenderer::ReloadShader - pipeline rebuild failed\n";
        return false;
    }

    shader_path_  = path;
    shader_mtime_ = mtime;
    std::cerr << "SDLRenderer::ReloadShader - reloaded: " << path << "\n";
    return true;
}

/**
 * @brief Create wavetable texture
 *
 * @return Pointer to the texture, or nullptr on failure
 */
SDL_GPUTexture *SDLRenderer::CreateWavetableTexture() noexcept {
    if (!device_) {
        std::cerr << "SDLRenderer::CreateWavetableTexture - device not initialized\n";
        return nullptr;
    }

    constexpr int SIZE = 512;  // Resolution: 512 samples per sine cycle
    std::vector<uint16_t> samples(SIZE);

    // Generate sin(0 to 2π) as 16-bit normalized
    for (int i = 0; i < SIZE; ++i) {
        float t     = static_cast<float>(i) / static_cast<float>(SIZE);
        float angle = t * 6.28318530718f;  // 2π

        // sin maps [-1, 1] to [0, 1] for 16-bit storage
        float sin_val                   = (std::sin(angle) + 1.0f) * 0.5f;
        samples[static_cast<size_t>(i)] = static_cast<uint16_t>(sin_val * 65535.0f);
    }

    SDL_GPUTextureCreateInfo info = {};
    info.type       = SDL_GPU_TEXTURETYPE_2D;  // Use 2D texture 512×1 (SDL3 doesn't have 1D)
    info.format     = SDL_GPU_TEXTUREFORMAT_R16_UNORM;
    info.width      = SIZE;
    info.height     = 1;
    info.num_levels = 1;
    info.usage      = SDL_GPU_TEXTUREUSAGE_SAMPLER;

    SDL_GPUTexture *tex = SDL_CreateGPUTexture(device_, &info);
    if (!tex) {
        std::cerr << "SDLRenderer::CreateWavetableTexture - SDL_CreateGPUTexture failed: "
                  << SDL_GetError() << "\n";
        return nullptr;
    }

    // Upload data
    SDL_GPUTransferBufferCreateInfo transfer_info = {};
    transfer_info.usage                           = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transfer_info.size                            = SIZE * sizeof(uint16_t);
    transfer_info.props                           = 0;

    SDL_GPUTransferBuffer *transfer = SDL_CreateGPUTransferBuffer(device_, &transfer_info);
    if (!transfer) {
        std::cerr << "SDLRenderer::CreateWavetableTexture - SDL_CreateGPUTransferBuffer failed: "
                  << SDL_GetError() << "\n";
        SDL_ReleaseGPUTexture(device_, tex);
        return nullptr;
    }

    void *mapped = SDL_MapGPUTransferBuffer(device_, transfer, false);
    if (mapped) {
        std::memcpy(mapped, samples.data(), SIZE * sizeof(uint16_t));
        SDL_UnmapGPUTransferBuffer(device_, transfer);
    } else {
        std::cerr << "SDLRenderer::CreateWavetableTexture - SDL_MapGPUTransferBuffer failed\n";
        SDL_ReleaseGPUTransferBuffer(device_, transfer);
        SDL_ReleaseGPUTexture(device_, tex);
        return nullptr;
    }

    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(device_);
    if (!cmd) {
        std::cerr << "SDLRenderer::CreateWavetableTexture - SDL_AcquireGPUCommandBuffer failed\n";
        SDL_ReleaseGPUTransferBuffer(device_, transfer);
        SDL_ReleaseGPUTexture(device_, tex);
        return nullptr;
    }

    SDL_GPUCopyPass *pass = SDL_BeginGPUCopyPass(cmd);

    SDL_GPUTextureTransferInfo source_info = {};
    source_info.transfer_buffer            = transfer;
    source_info.offset                     = 0;

    SDL_GPUTextureRegion destination = {};
    destination.texture              = tex;
    destination.x                    = 0;
    destination.y                    = 0;
    destination.z                    = 0;
    destination.w                    = SIZE;
    destination.h                    = 1;
    destination.d                    = 1;
    destination.mip_level            = 0;
    destination.layer                = 0;

    SDL_UploadToGPUTexture(pass, &source_info, &destination, false);
    SDL_EndGPUCopyPass(pass);
    SDL_SubmitGPUCommandBuffer(cmd);

    SDL_ReleaseGPUTransferBuffer(device_, transfer);

    std::cout << "[SDLRenderer] Created wavetable texture: 512 samples × 2 bytes = 1 KB\n";
    return tex;
}

}  // namespace pce::sdlos
