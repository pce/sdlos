#include "sdl_renderer.hh"

#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <cstring>

namespace pce::sdlos {
namespace fs = std::filesystem;

SDLRenderer::~SDLRenderer() {
    Shutdown();
}

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
    std::error_code ec;
    if (!fs::exists(path, ec) || ec) return 0;
    const auto t = fs::last_write_time(path, ec);
    if (ec) return 0;
    return static_cast<std::uintmax_t>(t.time_since_epoch().count());
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

    // Prefer Metal + MSL; try named driver first, then any compatible driver.
    device_ = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_MSL, false, "metal");
    if (!device_) {
        device_ = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_MSL, false, nullptr);
    }
    if (!device_) {
        std::cerr << "SDLRenderer::CreateDeviceForWindow - SDL_CreateGPUDevice failed: " << SDL_GetError() << "\n";
        return false;
    }

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

    SDL_GPUShaderCreateInfo fsc{};
    fsc.code = reinterpret_cast<const Uint8*>(fragSource.data());
    fsc.code_size = fragSource.size();
    fsc.entrypoint = "main0";
    fsc.format = SDL_GPU_SHADERFORMAT_MSL;
    fsc.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    fsc.num_samplers = 0;
    fsc.num_uniform_buffers = 1;
    fsc.props = 0;

    SDL_GPUShader* newFragment = SDL_CreateGPUShader(device_, &fsc);
    if (!newFragment) {
        std::cerr << "SDLRenderer::CreatePipeline - SDL_CreateGPUShader (fragment) failed: " << SDL_GetError() << "\n";
        SDL_ReleaseGPUShader(device_, newVertex);
        return false;
    }

    SDL_GPUGraphicsPipelineCreateInfo pci{};
    // No vertex buffer: geometry generated from vertex_id in the shader.
    SDL_GPUVertexInputState vis{};
    vis.vertex_buffer_descriptions = nullptr;
    vis.num_vertex_buffers = 0;
    vis.vertex_attributes = nullptr;
    vis.num_vertex_attributes = 0;
    pci.vertex_input_state = vis;

    pci.vertex_shader = newVertex;
    pci.fragment_shader = newFragment;
    pci.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;

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

    if (!CreateDeviceForWindow(window)) {
        return false;
    }

    std::string vertPath = vertex_shader_path_.empty() ? "assets/shaders/desktop.vert.metal" : vertex_shader_path_;
    std::string fragPath = shader_path_.empty() ? "assets/shaders/desktop.frag.metal" : shader_path_;

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
        // Fallback: simple animated FBM gradient
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

    if (!CreatePipeline(vertCode, fragCode)) {
        std::cerr << "SDLRenderer::Initialize - failed to build pipeline from default shaders\n";
        Shutdown();
        return false;
    }

    if (fs::exists(fragPath)) {
        shader_path_ = fragPath;
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
        const bool font_ok = text_renderer_->loadFirstAvailable({
            "assets/fonts/InterVariable.ttf",
            "assets/fonts/Inter-Regular.ttf",
            "assets/fonts/Roboto-Regular.ttf",
            "assets/fonts/LiberationSans-Regular.ttf",
        }, 17.f);

        if (!font_ok) {
            std::cerr << "SDLRenderer: no bundled font found in assets/fonts/\n"
                      << "             trying system fonts as fallback...\n";
            if (!text_renderer_->tryLoadSystemFont(17.f)) {
                std::cerr << "SDLRenderer: no font available — text rendering disabled\n";
            }
        }
    }

    initialized_.store(true);
    std::cerr << "SDLRenderer::Initialize - initialized successfully\n";
    return true;
}

void SDLRenderer::Shutdown() noexcept {
    if (!device_) {
        initialized_.store(false);
        return;
    }

    // Text renderer holds GPU textures against this device — shut it down first.
    if (text_renderer_) {
        text_renderer_->shutdown();
        text_renderer_.reset();
    }

    scene_tree_ = nullptr;
    scene_root_ = k_null_handle;

    if (ui_rect_pipeline_) { SDL_ReleaseGPUGraphicsPipeline(device_, ui_rect_pipeline_); ui_rect_pipeline_ = nullptr; }
    if (ui_text_pipeline_) { SDL_ReleaseGPUGraphicsPipeline(device_, ui_text_pipeline_); ui_text_pipeline_ = nullptr; }
    if (ui_rect_vert_)     { SDL_ReleaseGPUShader(device_, ui_rect_vert_);               ui_rect_vert_     = nullptr; }
    if (ui_rect_frag_)     { SDL_ReleaseGPUShader(device_, ui_rect_frag_);               ui_rect_frag_     = nullptr; }
    if (ui_text_frag_)     { SDL_ReleaseGPUShader(device_, ui_text_frag_);               ui_text_frag_     = nullptr; }

    if (pipeline_)        { SDL_ReleaseGPUGraphicsPipeline(device_, pipeline_);        pipeline_        = nullptr; }
    if (vertex_shader_)   { SDL_ReleaseGPUShader(device_, vertex_shader_);             vertex_shader_   = nullptr; }
    if (fragment_shader_) { SDL_ReleaseGPUShader(device_, fragment_shader_);           fragment_shader_ = nullptr; }

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

void SDLRenderer::SetScene(RenderTree* tree,
                            NodeHandle  root) noexcept
{
    scene_tree_ = tree;
    scene_root_ = root;
}

bool SDLRenderer::CreateUIPipelines()
{
    if (!device_ || !sdl_window_) return false;

    // ---- Load shader sources --------------------------------------------

    std::string vert_src = ReadTextFile("assets/shaders/ui_rect.vert.metal");
    if (vert_src.empty()) {
        // Minimal inline fallback (matches ui_rect.vert.metal exactly).
        vert_src = R"(
#include <metal_stdlib>
using namespace metal;
struct RectUniform { float x,y,w,h,vw,vh,_p0,_p1; };
struct VertOut { float4 position [[position]]; float2 uv [[user(locn0)]]; };
static inline float2 pxn(float2 p,float vw,float vh){
    return float2((p.x/vw)*2.0f-1.0f,-(p.y/vh)*2.0f+1.0f);
}
vertex VertOut main0(uint vid[[vertex_id]],constant RectUniform&r[[buffer(0)]]){
    float2 A=float2(r.x,r.y),B=float2(r.x+r.w,r.y);
    float2 C=float2(r.x+r.w,r.y+r.h),D=float2(r.x,r.y+r.h);
    float2 vs[6]={A,B,D,B,C,D};
    float2 us[6]={float2(0,0),float2(1,0),float2(0,1),float2(1,0),float2(1,1),float2(0,1)};
    VertOut o; o.position=float4(pxn(vs[vid],r.vw,r.vh),0,1); o.uv=us[vid]; return o;
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

    // ---- Compile shaders ------------------------------------------------

    auto make_shader = [&](const std::string& src,
                            SDL_GPUShaderStage stage,
                            Uint32 num_samplers,
                            Uint32 num_uniform_buffers) -> SDL_GPUShader*
    {
        SDL_GPUShaderCreateInfo ci{};
        ci.code                = reinterpret_cast<const Uint8*>(src.data());
        ci.code_size           = src.size();
        ci.entrypoint          = "main0";
        ci.format              = SDL_GPU_SHADERFORMAT_MSL;
        ci.stage               = stage;
        ci.num_samplers        = num_samplers;
        ci.num_uniform_buffers = num_uniform_buffers;
        ci.props               = 0;
        return SDL_CreateGPUShader(device_, &ci);
    };

    ui_rect_vert_ = make_shader(vert_src,      SDL_GPU_SHADERSTAGE_VERTEX,   0, 1);
    ui_rect_frag_ = make_shader(rect_frag_src, SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 1);
    ui_text_frag_ = make_shader(text_frag_src, SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);

    if (!ui_rect_vert_ || !ui_rect_frag_ || !ui_text_frag_) {
        std::cerr << "SDLRenderer::CreateUIPipelines - shader compile failed: "
                  << SDL_GetError() << "\n";
        if (ui_rect_vert_) { SDL_ReleaseGPUShader(device_, ui_rect_vert_); ui_rect_vert_ = nullptr; }
        if (ui_rect_frag_) { SDL_ReleaseGPUShader(device_, ui_rect_frag_); ui_rect_frag_ = nullptr; }
        if (ui_text_frag_) { SDL_ReleaseGPUShader(device_, ui_text_frag_); ui_text_frag_ = nullptr; }
        return false;
    }

    // ---- Common pipeline state ------------------------------------------

    // No vertex buffer: all geometry is generated in the vertex shader.
    SDL_GPUVertexInputState vis{};
    vis.vertex_buffer_descriptions = nullptr;
    vis.num_vertex_buffers         = 0;
    vis.vertex_attributes          = nullptr;
    vis.num_vertex_attributes      = 0;

    // Straight-alpha blending (SRC_ALPHA / ONE_MINUS_SRC_ALPHA).
    SDL_GPUColorTargetDescription colorDesc{};
    colorDesc.format                              = SDL_GetGPUSwapchainTextureFormat(device_, sdl_window_);
    colorDesc.blend_state.enable_blend            = true;
    colorDesc.blend_state.src_color_blendfactor   = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    colorDesc.blend_state.dst_color_blendfactor   = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    colorDesc.blend_state.color_blend_op          = SDL_GPU_BLENDOP_ADD;
    colorDesc.blend_state.src_alpha_blendfactor   = SDL_GPU_BLENDFACTOR_ONE;
    colorDesc.blend_state.dst_alpha_blendfactor   = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    colorDesc.blend_state.alpha_blend_op          = SDL_GPU_BLENDOP_ADD;

    SDL_GPUGraphicsPipelineTargetInfo targetInfo{};
    targetInfo.color_target_descriptions = &colorDesc;
    targetInfo.num_color_targets         = 1;
    targetInfo.has_depth_stencil_target  = false;

    // ---- ui_rect pipeline -----------------------------------------------

    SDL_GPUGraphicsPipelineCreateInfo rpci{};
    rpci.vertex_shader      = ui_rect_vert_;
    rpci.fragment_shader    = ui_rect_frag_;
    rpci.primitive_type     = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    rpci.vertex_input_state = vis;
    rpci.target_info        = targetInfo;

    ui_rect_pipeline_ = SDL_CreateGPUGraphicsPipeline(device_, &rpci);
    if (!ui_rect_pipeline_) {
        std::cerr << "SDLRenderer::CreateUIPipelines - rect pipeline failed: "
                  << SDL_GetError() << "\n";
        SDL_ReleaseGPUShader(device_, ui_rect_vert_); ui_rect_vert_ = nullptr;
        SDL_ReleaseGPUShader(device_, ui_rect_frag_); ui_rect_frag_ = nullptr;
        SDL_ReleaseGPUShader(device_, ui_text_frag_); ui_text_frag_ = nullptr;
        return false;
    }

    // ---- ui_text pipeline (same vert, sampled frag) ---------------------

    SDL_GPUGraphicsPipelineCreateInfo tpci{};
    tpci.vertex_shader      = ui_rect_vert_;   // shared vertex shader
    tpci.fragment_shader    = ui_text_frag_;
    tpci.primitive_type     = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    tpci.vertex_input_state = vis;
    tpci.target_info        = targetInfo;

    ui_text_pipeline_ = SDL_CreateGPUGraphicsPipeline(device_, &tpci);
    if (!ui_text_pipeline_) {
        std::cerr << "SDLRenderer::CreateUIPipelines - text pipeline failed: "
                  << SDL_GetError() << "\n";
        SDL_ReleaseGPUGraphicsPipeline(device_, ui_rect_pipeline_); ui_rect_pipeline_ = nullptr;
        SDL_ReleaseGPUShader(device_, ui_rect_vert_); ui_rect_vert_ = nullptr;
        SDL_ReleaseGPUShader(device_, ui_rect_frag_); ui_rect_frag_ = nullptr;
        SDL_ReleaseGPUShader(device_, ui_text_frag_); ui_text_frag_ = nullptr;
        return false;
    }

    std::cerr << "SDLRenderer::CreateUIPipelines - rect + text pipelines ready\n";
    return true;
}

void SDLRenderer::Render(double timeSeconds) {
    if (!initialized_.load() || !device_) {
        return;
    }

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    if (!cmd) {
        std::cerr << "SDLRenderer::Render - SDL_AcquireGPUCommandBuffer failed: "
                  << SDL_GetError() << "\n";
        return;
    }

    SDL_GPUTexture* swap = nullptr;
    Uint32 width = 0, height = 0;

    if (!SDL_AcquireGPUSwapchainTexture(cmd, sdl_window_, &swap, &width, &height)) {
        std::cerr << "SDLRenderer::Render - SDL_AcquireGPUSwapchainTexture error: "
                  << SDL_GetError() << "\n";
        SDL_SubmitGPUCommandBuffer(cmd);
        return;
    }

    if (!swap) {
        // Swapchain not ready (minimised / resize in progress). Submit empty cmd.
        SDL_SubmitGPUCommandBuffer(cmd);
        return;
    }

    // ---- Pass 1: copy pass — flush pending glyph textures ---------------
    if (text_renderer_ && text_renderer_->hasPendingUploads()) {
        SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
        if (cp) {
            text_renderer_->flushUploads(cp);
            SDL_EndGPUCopyPass(cp);
        }
    }

    // ---- Pass 2: render pass — wallpaper + UI overlay -------------------
    SDL_GPUColorTargetInfo ct{};
    ct.texture              = swap;
    ct.mip_level            = 0;
    ct.layer_or_depth_plane = 0;
    ct.clear_color          = {0.0f, 0.0f, 0.0f, 1.0f};
    ct.load_op              = SDL_GPU_LOADOP_CLEAR;
    ct.store_op             = SDL_GPU_STOREOP_STORE;
    ct.resolve_texture      = nullptr;
    ct.resolve_mip_level    = 0;
    ct.resolve_layer        = 0;
    ct.cycle                = true;

    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &ct, 1, nullptr);
    if (!pass) {
        std::cerr << "SDLRenderer::Render - SDL_BeginGPURenderPass failed: "
                  << SDL_GetError() << "\n";
        SDL_SubmitGPUCommandBuffer(cmd);
        return;
    }

    // ---- 2a. Wallpaper (fullscreen FBM, 3 vertices, no buffer) ----------
    SDL_BindGPUGraphicsPipeline(pass, pipeline_);

    FragmentUniform fu{};
    fu.time      = static_cast<float>(timeSeconds);
    fu.pad[0] = fu.pad[1] = fu.pad[2] = 0.0f;
    SDL_PushGPUFragmentUniformData(cmd, 0, &fu, sizeof(fu));

    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);

    // ---- 2b. UI overlay (RenderTree widgets, alpha-blended on top) ------
    if (scene_tree_ && scene_root_.valid() &&
        ui_rect_pipeline_ && ui_text_pipeline_)
    {
        RenderContext ctx;
        ctx.backend       = GPUBackend::Metal;
        ctx.device        = device_;
        ctx.cmd           = cmd;
        ctx.pass          = pass;
        ctx.viewport_w    = static_cast<float>(width);
        ctx.viewport_h    = static_cast<float>(height);
        ctx.arena         = &scene_tree_->arena();
        ctx.text_renderer = text_renderer_.get();

        ctx.pipelines["rect"] = ui_rect_pipeline_;
        ctx.pipelines["text"] = ui_text_pipeline_;

        // Root dimensions track the swapchain exactly (physical pixels,
        // Retina-correct). markLayoutDirty() propagates downward so a window
        // resize automatically re-flows the entire scene tree.
        {
            RenderNode* root = scene_tree_->node(scene_root_);
            if (root) {
                const float fw = static_cast<float>(width);
                const float fh = static_cast<float>(height);
                if (root->w != fw || root->h != fh) {
                    root->w = fw;
                    root->h = fh;
                    scene_tree_->markLayoutDirty(scene_root_);
                }
            }
        }

        scene_tree_->beginFrame();
        scene_tree_->update(scene_root_);
        scene_tree_->render(scene_root_, ctx);
    }

    SDL_EndGPURenderPass(pass);

    // Do NOT call SDL_ReleaseGPUTexture on `swap` — swapchain textures are
    // non-owning views; releasing them corrupts the Metal pending-destroy queue.
    SDL_SubmitGPUCommandBuffer(cmd);
}

bool SDLRenderer::ReloadShader(const std::string& path) {
    std::error_code ec;

    if (!fs::exists(path, ec) || ec) {
        std::cerr << "SDLRenderer::ReloadShader - shader file not found: " << path << "\n";
        return false;
    }

    const auto mtime = GetFileMTime(path);
    if (mtime == shader_mtime_) {
        return true;   // unchanged — nothing to do
    }

    const std::string source = ReadTextFile(path);
    if (source.empty()) {
        std::cerr << "SDLRenderer::ReloadShader - failed to read shader: " << path << "\n";
        return false;
    }

    std::string vertSource;
    if (!vertex_shader_path_.empty() &&
        fs::exists(vertex_shader_path_, ec) && !ec) {
        vertSource = ReadTextFile(vertex_shader_path_);
    }

    if (vertSource.empty()) {
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

} // namespace pce::sdlos
