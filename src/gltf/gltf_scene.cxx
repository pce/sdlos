#include "gltf_scene.hh"
#include "math3d.hh"
#include "../css_loader.hh"

#include <fastgltf/core.hpp>
#include <fastgltf/types.hpp>
#include <fastgltf/tools.hpp>

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include <algorithm>
#include <cassert>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace pce::sdlos::gltf {

using namespace math3d;


GltfCamera::GltfCamera() noexcept
{
    const Mat4 id = Mat4::identity();
    std::memcpy(view, id.data.data(), sizeof(view));
    std::memcpy(proj, id.data.data(), sizeof(proj));
    pos[0] = 0.f;
    pos[1] = 2.f;
    pos[2] = 8.f;
}

void GltfCamera::lookAt(float ex, float ey, float ez,
                         float cx, float cy, float cz) noexcept
{
    pos[0] = ex;
    pos[1] = ey;
    pos[2] = ez;
    const Mat4 m = math3d::lookAt({ex, ey, ez}, {cx, cy, cz});
    std::memcpy(view, m.data.data(), sizeof(view));
}

void GltfCamera::perspective(float fov_y_deg, float aspect,
                              float near_z, float far_z) noexcept
{
    constexpr float kDeg2Rad = 3.14159265358979323846f / 180.f;
    const Mat4 m = math3d::perspective(fov_y_deg * kDeg2Rad, aspect,
                                        near_z, far_z);
    std::memcpy(proj, m.data.data(), sizeof(proj));
}

void GltfCamera::setViewport(float w, float h) noexcept
{
    vw = w;
    vh = h;
}


/*static*/
void GltfScene::parseHexColor(std::string_view hex, float (&out)[4]) noexcept
{
    out[0] = out[1] = out[2] = out[3] = 1.f;
    if (hex.empty()) return;

    if (hex.front() == '#') hex.remove_prefix(1);
    if (hex.size() < 6) return;

    auto nibble = [](char c) noexcept -> unsigned {
        if (c >= '0' && c <= '9') return static_cast<unsigned>(c - '0');
        if (c >= 'a' && c <= 'f') return static_cast<unsigned>(c - 'a') + 10u;
        if (c >= 'A' && c <= 'F') return static_cast<unsigned>(c - 'A') + 10u;
        return 0u;
    };

    auto byte = [&](std::size_t i) noexcept -> float {
        return static_cast<float>((nibble(hex[i]) << 4u) | nibble(hex[i + 1]))
               / 255.f;
    };

    out[0] = byte(0);
    out[1] = byte(2);
    out[2] = byte(4);
    out[3] = (hex.size() >= 8) ? byte(6) : 1.f;
}

/*static*/
float GltfScene::parseFloat(std::string_view s, float fallback) noexcept
{
    if (s.empty()) return fallback;
    float value = fallback;
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
    return (ec == std::errc{}) ? value : fallback;
}

/*static*/
std::string GltfScene::normalizeId(std::string_view s) noexcept
{
    std::string out;
    out.reserve(s.size());
    for (const unsigned char c : s) {
        if (std::isalnum(static_cast<int>(c))) {
            out += static_cast<char>(std::tolower(static_cast<int>(c)));
        } else if (c == ' ' || c == '_' || c == '-') {
            out += '-';
        }
    }
    return out;
}


bool GltfScene::init(SDL_GPUDevice*       device,
                     SDL_GPUShaderFormat  fmt,
                     const std::string&   base_path,
                     SDL_GPUTextureFormat swapchain_fmt) noexcept
{
    device_ = device;
    fmt_    = fmt;
    base_   = base_path;

    if (!createPipeline(swapchain_fmt)) {
        std::cerr << "[GltfScene] init: pipeline creation failed\n";
        device_ = nullptr;
        return false;
    }

    // Establish a sensible default camera; the caller can override.
    camera_.setViewport(1.f, 1.f);
    camera_.lookAt(0.f, 2.f, 8.f, 0.f, 0.f, 0.f);
    camera_.perspective(45.f, 1.f);

    std::cerr << "[GltfScene] init OK\n";
    return true;
}


int GltfScene::attach(RenderTree&        tree,
                      NodeHandle         tree_root,
                      const std::string& base_path)
{
    tree_ref_ = &tree;

    // ── DFS: collect every node whose tag == "scene3d" ───────────────────
    //
    // We collect all handles first, then load files, so that the loadFile()
    // calls (which append children to those nodes) do not invalidate the DFS.

    std::vector<NodeHandle> scene3d_nodes;

    if (tree_root != k_null_handle) {
        std::vector<NodeHandle> stack;
        stack.push_back(tree_root);

        while (!stack.empty()) {
            const NodeHandle h = stack.back();
            stack.pop_back();

            const RenderNode* n = tree.node(h);
            if (!n) continue;

            if (n->style("tag") == "scene3d")
                scene3d_nodes.push_back(h);

            // Walk the LCRS sibling chain from the first child.
            NodeHandle c = n->child;
            while (c != k_null_handle) {
                const RenderNode* cn = tree.node(c);
                if (!cn) break;
                stack.push_back(c);
                c = cn->sibling;
            }
        }
    }

    // ── Load each scene3d node's src= glTF file ───────────────────────────

    const std::size_t before = entries_.size();

    for (const NodeHandle sh : scene3d_nodes) {
        const RenderNode* sn = tree.node(sh);
        if (!sn) continue;

        const auto src_sv = sn->style("src");
        if (src_sv.empty()) {
            std::cerr << "[GltfScene] scene3d node missing src= attribute\n";
            continue;
        }

        const std::filesystem::path gltf_path =
            std::filesystem::path(base_path) / std::string(src_sv);

        if (!std::filesystem::exists(gltf_path)) {
            std::cerr << "[GltfScene] glTF file not found: "
                      << gltf_path << "\n";
            continue;
        }

        loadFile(gltf_path, tree, sh);
    }

    const int loaded = static_cast<int>(entries_.size() - before);
    std::cerr << "[GltfScene] attach: " << loaded
              << " primitive(s) from " << scene3d_nodes.size()
              << " scene3d node(s)\n";
    return loaded;
}


void GltfScene::applyCSS(RenderTree& tree) noexcept
{
    // StyleMap values are read live from each proxy node every frame inside
    // drawEntry(), so there is no separate uniform cache to flush.
    //
    // This call is the hook the app invokes after css.applyTo() has run so
    // that any CSS rules which landed on 3D proxy nodes are picked up on the
    // very next render.  We just mark every proxy dirty to guarantee that.
    for (const auto& e : entries_) {
        RenderNode* n = tree.node(e.proxy_handle);
        if (n) n->dirty_render = true;
    }
}


// Build the model matrix for an entry from style-driven transform properties.
// When any of --scale[-x/y/z], --translate-x/y/z, --rotation-x/y/z are set
// on the proxy node they fully replace the glTF world_mat.  This lets a CSS
// "map file" loaded with applyMapCSS() act as the scene layout descriptor:
//   --scale          uniform scale (all axes)
//   --scale-x/y/z    per-axis scale; each falls back to --scale when absent
//   --translate-x/y/z world-space offset (pixels in ortho, metres in persp)
//   --rotation-x/y/z Euler degrees, applied X → Y → Z
// When none are set, world_mat (from the glTF node hierarchy) is used as-is.
// TRS order: T * Rz * Ry * Rx * S.
/*static*/ Mat4 GltfScene::buildModelMatrix(const MeshEntry& e,
                                             RenderTree&      tree) noexcept
{
    const RenderNode* n = tree.node(e.proxy_handle);
    if (!n) {
        Mat4 m{};
        std::memcpy(m.data.data(), e.world_mat, sizeof(e.world_mat));
        return m;
    }

    const float su = parseFloat(n->style("--scale"),        0.f);
    const float sx = parseFloat(n->style("--scale-x"),      0.f);
    const float sy = parseFloat(n->style("--scale-y"),      0.f);
    const float sz = parseFloat(n->style("--scale-z"),      0.f);
    const float tx = parseFloat(n->style("--translate-x"),  0.f);
    const float ty = parseFloat(n->style("--translate-y"),  0.f);
    const float tz = parseFloat(n->style("--translate-z"),  0.f);
    const float rx = parseFloat(n->style("--rotation-x"),   0.f);
    const float ry = parseFloat(n->style("--rotation-y"),   0.f);
    const float rz = parseFloat(n->style("--rotation-z"),   0.f);

    const bool has_style =
        (su != 0.f || sx != 0.f || sy != 0.f || sz != 0.f ||
         tx != 0.f || ty != 0.f || tz != 0.f ||
         rx != 0.f || ry != 0.f || rz != 0.f);

    if (!has_style) {
        Mat4 m{};
        std::memcpy(m.data.data(), e.world_mat, sizeof(e.world_mat));
        return m;
    }

    constexpr float kD2R = 3.14159265f / 180.f;
    const float fsx = (sx != 0.f) ? sx : (su != 0.f ? su : 1.f);
    const float fsy = (sy != 0.f) ? sy : (su != 0.f ? su : 1.f);
    const float fsz = (sz != 0.f) ? sz : (su != 0.f ? su : 1.f);

    // CSS TRS is an outer transform on top of world_mat, not a replacement.
    // --scale: 1 = natural glTF size; --translate-x: 3 = 3 units from glTF origin.
    Mat4 world_m{};
    std::memcpy(world_m.data.data(), e.world_mat, sizeof(e.world_mat));

    return translate({tx, ty, tz})
         * rotateZ(rz * kD2R)
         * rotateY(ry * kD2R)
         * rotateX(rx * kD2R)
         * scale({fsx, fsy, fsz})
         * world_m;
}

bool GltfScene::applyMapCSS(RenderTree&        tree,
                             NodeHandle         root,
                             const std::string& path) noexcept
{
    auto css = pce::sdlos::css::load(path);
    if (css.empty()) {
        std::cerr << "[GltfScene] applyMapCSS: no rules loaded from " << path << "\n";
        return false;
    }
    css.applyTo(tree, root);
    std::cerr << "[GltfScene] applyMapCSS: " << css.size() << " rules from "
              << std::filesystem::path(path).filename().string() << "\n";
    return true;
}

void GltfScene::tick(RenderTree& tree, float vw, float vh) noexcept
{
    camera_.setViewport(vw, vh);

    // Rebuild view / proj matrices from the camera's flat float[16] arrays.
    Mat4 view_mat{}, proj_mat{};
    std::memcpy(view_mat.data.data(), camera_.view, sizeof(camera_.view));
    std::memcpy(proj_mat.data.data(), camera_.proj, sizeof(camera_.proj));

    for (auto& e : entries_) {
        // Skip entries whose parent scene3d container is hidden.
        if (e.scene3d_handle.valid()) {
            const RenderNode* sn = tree.node(e.scene3d_handle);
            if (sn && sn->style("display") == "none") continue;
        }

        RenderNode* n = tree.node(e.proxy_handle);
        if (!n) continue;

        // MVP = proj × view × model (style transform or glTF world_mat)
        const Mat4 model_mat = buildModelMatrix(e, tree);
        const Mat4 mvp = proj_mat * view_mat * model_mat;

        // Project all 8 corners of the model-space AABB to screen space.
        const float* mn = e.gpu.aabb_min;
        const float* mx = e.gpu.aabb_max;

        const float corners[8][3] = {
            {mn[0], mn[1], mn[2]},
            {mx[0], mn[1], mn[2]},
            {mn[0], mx[1], mn[2]},
            {mx[0], mx[1], mn[2]},
            {mn[0], mn[1], mx[2]},
            {mx[0], mn[1], mx[2]},
            {mn[0], mx[1], mx[2]},
            {mx[0], mx[1], mx[2]},
        };

        constexpr float kFMax = std::numeric_limits<float>::max();
        constexpr float kFMin = std::numeric_limits<float>::lowest();

        float sx_min = kFMax, sy_min = kFMax;
        float sx_max = kFMin, sy_max = kFMin;
        bool  any_visible = false;

        for (const auto& c : corners) {
            const Vec4 clip = mvp * Vec4{c[0], c[1], c[2], 1.f};

            // w ≤ 0 means the corner is at or behind the near plane.
            if (clip.w <= 0.f) continue;

            const float inv_w = 1.f / clip.w;
            const float ndc_x =  clip.x * inv_w;
            const float ndc_y =  clip.y * inv_w;

            // NDC → screen pixels.
            // Metal NDC: +Y up.  Screen: +Y down → flip.
            const float sx = ( ndc_x + 1.f) * 0.5f * vw;
            const float sy = (-ndc_y + 1.f) * 0.5f * vh;

            sx_min = std::min(sx_min, sx);
            sy_min = std::min(sy_min, sy);
            sx_max = std::max(sx_max, sx);
            sy_max = std::max(sy_max, sy);
            any_visible = true;
        }

        // If all corners were behind the camera, leave the bounds unchanged.
        if (!any_visible) continue;

        const float new_x = sx_min;
        const float new_y = sy_min;
        const float new_w = sx_max - sx_min;
        const float new_h = sy_max - sy_min;

        if (n->x != new_x || n->y != new_y ||
            n->w != new_w || n->h != new_h)
        {
            n->x = new_x;
            n->y = new_y;
            n->w = new_w;
            n->h = new_h;
            n->dirty_render = true;
        }
    }
}


void GltfScene::render(SDL_GPUCommandBuffer* cmd,
                       SDL_GPUTexture*       color_target,
                       float vw, float vh) noexcept
{
    if (!pipeline_ || entries_.empty() || !color_target || !cmd) return;

    const Uint32 w = std::max(1u, static_cast<Uint32>(vw));
    const Uint32 h = std::max(1u, static_cast<Uint32>(vh));

    if (!createOrResizeDepth(w, h)) return;

    // ── Color target: LOAD (preserve wallpaper already drawn), then STORE ─

    SDL_GPUColorTargetInfo ct{};
    ct.texture  = color_target;
    ct.load_op  = SDL_GPU_LOADOP_LOAD;
    ct.store_op = SDL_GPU_STOREOP_STORE;
    ct.cycle    = false;

    // ── Depth target: CLEAR to 1.0 at start of the 3D pre-pass ───────────

    SDL_GPUDepthStencilTargetInfo dt{};
    dt.texture          = depth_;
    dt.load_op          = SDL_GPU_LOADOP_CLEAR;
    dt.store_op         = SDL_GPU_STOREOP_DONT_CARE;
    dt.stencil_load_op  = SDL_GPU_LOADOP_DONT_CARE;
    dt.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    dt.clear_depth      = 1.f;
    dt.clear_stencil    = 0;
    dt.cycle            = false;

    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &ct, 1, &dt);
    if (!pass) {
        std::cerr << "[GltfScene] SDL_BeginGPURenderPass failed: "
                  << SDL_GetError() << "\n";
        return;
    }

    SDL_BindGPUGraphicsPipeline(pass, pipeline_);

    SDL_GPUViewport viewport{};
    viewport.x         = 0.f;
    viewport.y         = 0.f;
    viewport.w         = vw;
    viewport.h         = vh;
    viewport.min_depth = 0.f;
    viewport.max_depth = 1.f;
    SDL_SetGPUViewport(pass, &viewport);

    if (tree_ref_) {
        for (const auto& e : entries_) {
            if (!e.gpu.valid()) continue;
            drawEntry(pass, cmd, e, *tree_ref_, vw, vh);
        }
    }

    SDL_EndGPURenderPass(pass);
}


void GltfScene::drawEntry(SDL_GPURenderPass*    pass,
                           SDL_GPUCommandBuffer* cmd,
                           const MeshEntry&      e,
                           RenderTree&           tree,
                           float /*vw*/, float /*vh*/) noexcept
{
    const RenderNode* n = tree.node(e.proxy_handle);
    if (!n) return;

    // Skip if the parent scene3d container is hidden (display: none).
    if (e.scene3d_handle.valid()) {
        const RenderNode* sn = tree.node(e.scene3d_handle);
        if (sn && sn->style("display") == "none") return;
    }

    // ── Read live CSS values from the proxy node's StyleMap ───────────────
    //
    // Values are read every frame so CSS animation / hover transitions
    // (set by tickHover) are picked up without any additional callbacks.

    // 'color' → base_color rgba (default opaque white)
    float base_color[4] = {1.f, 1.f, 1.f, 1.f};
    {
        const auto cv = n->style("color");
        if (!cv.empty()) parseHexColor(cv, base_color);
    }

    // '--roughness' (default 0.5)
    const float roughness = parseFloat(n->style("--roughness"), 0.5f);

    // '--metallic' (default 0.0)
    const float metallic  = parseFloat(n->style("--metallic"),  0.0f);

    // '--emissive' hex rgb + '--emissive-intensity' scalar (default 1.0)
    float emissive[4] = {0.f, 0.f, 0.f, 1.f};
    {
        const auto ev = n->style("--emissive");
        if (!ev.empty()) {
            float tmp[4] = {0.f, 0.f, 0.f, 1.f};
            parseHexColor(ev, tmp);
            emissive[0] = tmp[0];
            emissive[1] = tmp[1];
            emissive[2] = tmp[2];
        }
        emissive[3] = parseFloat(n->style("--emissive-intensity"), 1.f);
    }

    // 'border-width' > 0 signals hover (set by css :hover rule)
    const float hover_t =
        (parseFloat(n->style("border-width"), 0.f) > 0.f) ? 1.f : 0.f;

    // 'opacity' (default 1.0)
    const float opacity = parseFloat(n->style("opacity"), 1.f);

    // ── Build VertPush ────────────────────────────────────────────────────

    Mat4 view_mat{}, proj_mat{};
    std::memcpy(view_mat.data.data(), camera_.view, sizeof(camera_.view));
    std::memcpy(proj_mat.data.data(), camera_.proj, sizeof(camera_.proj));

    // Style transform (--scale, --translate-x/y/z, --rotation-x/y/z) or
    // glTF world_mat fallback — built once here, shared by VertPush and MVP.
    const Mat4 model_mat = buildModelMatrix(e, tree);
    const Mat4 mvp = proj_mat * view_mat * model_mat;

    VertPush vp{};
    std::memcpy(vp.mvp,   mvp.data.data(),       sizeof(vp.mvp));
    std::memcpy(vp.model, model_mat.data.data(),  sizeof(vp.model));

    // ── Build FragPush ────────────────────────────────────────────────────

    FragPush fp{};
    std::memcpy(fp.base_color, base_color, sizeof(fp.base_color));
    std::memcpy(fp.emissive,   emissive,   sizeof(fp.emissive));

    fp.light_dir_i[0] = light_.dir[0];
    fp.light_dir_i[1] = light_.dir[1];
    fp.light_dir_i[2] = light_.dir[2];
    fp.light_dir_i[3] = light_.intensity;

    fp.light_color[0] = light_.color[0];
    fp.light_color[1] = light_.color[1];
    fp.light_color[2] = light_.color[2];
    fp.light_color[3] = 0.f;

    fp.cam_pos[0] = camera_.pos[0];
    fp.cam_pos[1] = camera_.pos[1];
    fp.cam_pos[2] = camera_.pos[2];
    fp.cam_pos[3] = 0.f;

    fp.roughness = roughness;
    fp.metallic  = metallic;
    fp.hover_t   = hover_t;
    fp.opacity   = opacity;

    // ── Bind buffers, push uniforms, draw ────────────────────────────────

    SDL_GPUBufferBinding vbb{};
    vbb.buffer = e.gpu.vertex_buf;
    vbb.offset = 0;
    SDL_BindGPUVertexBuffers(pass, 0, &vbb, 1);

    SDL_GPUBufferBinding ibb{};
    ibb.buffer = e.gpu.index_buf;
    ibb.offset = 0;
    SDL_BindGPUIndexBuffer(pass, &ibb, SDL_GPU_INDEXELEMENTSIZE_32BIT);

    SDL_PushGPUVertexUniformData(cmd,  0, &vp, static_cast<Uint32>(sizeof(vp)));
    SDL_PushGPUFragmentUniformData(cmd, 0, &fp, static_cast<Uint32>(sizeof(fp)));

    SDL_DrawGPUIndexedPrimitives(pass,
                                  e.gpu.index_count,  // num_indices
                                  1,                   // num_instances
                                  0,                   // first_index
                                  0,                   // vertex_offset
                                  0);                  // first_instance
}


bool GltfScene::createPipeline(SDL_GPUTextureFormat swapchain_fmt) noexcept
{
    // ── Choose shader file paths from the backend format ──────────────────

    std::string vert_path, frag_path;
    SDL_GPUShaderFormat shader_fmt = SDL_GPU_SHADERFORMAT_INVALID;

    if (fmt_ & SDL_GPU_SHADERFORMAT_MSL) {
        vert_path  = base_ + "data/shaders/msl/pbr_mesh.vert.metal";
        frag_path  = base_ + "data/shaders/msl/pbr_mesh.frag.metal";
        shader_fmt = SDL_GPU_SHADERFORMAT_MSL;
    } else if (fmt_ & SDL_GPU_SHADERFORMAT_SPIRV) {
        vert_path  = base_ + "data/shaders/spirv/pbr_mesh.vert.spv";
        frag_path  = base_ + "data/shaders/spirv/pbr_mesh.frag.spv";
        shader_fmt = SDL_GPU_SHADERFORMAT_SPIRV;
    } else {
        std::cerr << "[GltfScene] createPipeline: unsupported shader format "
                  << static_cast<int>(fmt_) << "\n";
        return false;
    }

    // ── Load shader source / bytecode from disk ───────────────────────────

    auto loadBytes = [](const std::string& path) -> std::vector<Uint8> {
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f) return {};
        const auto sz = f.tellg();
        if (sz <= 0) return {};
        f.seekg(0);
        std::vector<Uint8> buf(static_cast<std::size_t>(sz));
        if (!f.read(reinterpret_cast<char*>(buf.data()),
                    static_cast<std::streamsize>(sz)))
            return {};
        return buf;
    };

    const auto vert_code = loadBytes(vert_path);
    if (vert_code.empty()) {
        std::cerr << "[GltfScene] vertex shader not found: " << vert_path << "\n";
        return false;
    }

    const auto frag_code = loadBytes(frag_path);
    if (frag_code.empty()) {
        std::cerr << "[GltfScene] fragment shader not found: " << frag_path << "\n";
        return false;
    }

    // ── Vertex shader (slot 0 = VertPush, 128 bytes) ──────────────────────

    SDL_GPUShaderCreateInfo vci{};
    vci.code                = vert_code.data();
    vci.code_size           = vert_code.size();
    vci.entrypoint          = "main0";
    vci.format              = shader_fmt;
    vci.stage               = SDL_GPU_SHADERSTAGE_VERTEX;
    vci.num_samplers        = 0;
    vci.num_uniform_buffers = 1;
    vci.props               = 0;

    vert_ = SDL_CreateGPUShader(device_, &vci);
    if (!vert_) {
        std::cerr << "[GltfScene] vertex shader compile failed: "
                  << SDL_GetError() << "\n";
        return false;
    }

    // ── Fragment shader (slot 0 = FragPush, 96 bytes) ─────────────────────

    SDL_GPUShaderCreateInfo fci{};
    fci.code                = frag_code.data();
    fci.code_size           = frag_code.size();
    fci.entrypoint          = "main0";
    fci.format              = shader_fmt;
    fci.stage               = SDL_GPU_SHADERSTAGE_FRAGMENT;
    fci.num_samplers        = 0;
    fci.num_uniform_buffers = 1;
    fci.props               = 0;

    frag_ = SDL_CreateGPUShader(device_, &fci);
    if (!frag_) {
        std::cerr << "[GltfScene] fragment shader compile failed: "
                  << SDL_GetError() << "\n";
        SDL_ReleaseGPUShader(device_, vert_);
        vert_ = nullptr;
        return false;
    }

    // ── Vertex input layout ───────────────────────────────────────────────
    //
    //   Buffer 0, stride = 32 (sizeof GpuVertex):
    //     location 0 — FLOAT3 @ offset  0  (position)
    //     location 1 — FLOAT3 @ offset 12  (normal)
    //     location 2 — FLOAT2 @ offset 24  (texcoord_0)

    SDL_GPUVertexBufferDescription vbd{};
    vbd.slot               = 0;
    vbd.pitch              = 32;
    vbd.input_rate         = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    vbd.instance_step_rate = 0;

    SDL_GPUVertexAttribute attrs[3]{};

    attrs[0].location    = 0;
    attrs[0].buffer_slot = 0;
    attrs[0].format      = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
    attrs[0].offset      = 0;

    attrs[1].location    = 1;
    attrs[1].buffer_slot = 0;
    attrs[1].format      = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
    attrs[1].offset      = 12;

    attrs[2].location    = 2;
    attrs[2].buffer_slot = 0;
    attrs[2].format      = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
    attrs[2].offset      = 24;

    SDL_GPUVertexInputState vis{};
    vis.vertex_buffer_descriptions = &vbd;
    vis.num_vertex_buffers         = 1;
    vis.vertex_attributes          = attrs;
    vis.num_vertex_attributes      = 3;

    // ── Color target: swapchain format, straight-alpha blend ──────────────

    SDL_GPUColorTargetDescription color_desc{};
    color_desc.format = swapchain_fmt;
    color_desc.blend_state.enable_blend          = true;
    color_desc.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    color_desc.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    color_desc.blend_state.color_blend_op        = SDL_GPU_BLENDOP_ADD;
    color_desc.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    color_desc.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    color_desc.blend_state.alpha_blend_op        = SDL_GPU_BLENDOP_ADD;

    SDL_GPUGraphicsPipelineTargetInfo target_info{};
    target_info.color_target_descriptions = &color_desc;
    target_info.num_color_targets         = 1;
    target_info.has_depth_stencil_target  = true;
    target_info.depth_stencil_format      = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;

    // ── Depth-stencil: LESS, write enabled ───────────────────────────────

    SDL_GPUDepthStencilState ds{};
    ds.enable_depth_test   = true;
    ds.enable_depth_write  = true;
    ds.compare_op          = SDL_GPU_COMPAREOP_LESS;
    ds.enable_stencil_test = false;

    // ── Rasterizer: back-face cull, CCW front-face ────────────────────────

    SDL_GPURasterizerState ras{};
    ras.cull_mode  = SDL_GPU_CULLMODE_BACK;
    ras.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    ras.fill_mode  = SDL_GPU_FILLMODE_FILL;

    // ── Assemble and create the graphics pipeline ─────────────────────────

    SDL_GPUGraphicsPipelineCreateInfo pci{};
    pci.vertex_shader       = vert_;
    pci.fragment_shader     = frag_;
    pci.vertex_input_state  = vis;
    pci.primitive_type      = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pci.rasterizer_state    = ras;
    pci.depth_stencil_state = ds;
    pci.target_info         = target_info;
    pci.props               = 0;

    pipeline_ = SDL_CreateGPUGraphicsPipeline(device_, &pci);
    if (!pipeline_) {
        std::cerr << "[GltfScene] SDL_CreateGPUGraphicsPipeline failed: "
                  << SDL_GetError() << "\n";
        SDL_ReleaseGPUShader(device_, vert_); vert_ = nullptr;
        SDL_ReleaseGPUShader(device_, frag_); frag_ = nullptr;
        return false;
    }

    std::cerr << "[GltfScene] PBR pipeline created ("
              << ((shader_fmt == SDL_GPU_SHADERFORMAT_MSL) ? "MSL" : "SPIRV")
              << ")\n";
    return true;
}


bool GltfScene::createOrResizeDepth(Uint32 w, Uint32 h) noexcept
{
    if (depth_ && depth_w_ == w && depth_h_ == h) return true;

    // SDL3 defers the actual GPU-side release until all in-flight commands
    // referencing the old texture have completed — safe to call immediately.
    if (depth_) {
        SDL_ReleaseGPUTexture(device_, depth_);
        depth_   = nullptr;
        depth_w_ = 0;
        depth_h_ = 0;
    }

    SDL_GPUTextureCreateInfo tci{};
    tci.type                 = SDL_GPU_TEXTURETYPE_2D;
    tci.format               = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    tci.usage                = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
    tci.width                = w;
    tci.height               = h;
    tci.layer_count_or_depth = 1;
    tci.num_levels           = 1;
    tci.sample_count         = SDL_GPU_SAMPLECOUNT_1;
    tci.props                = 0;

    depth_ = SDL_CreateGPUTexture(device_, &tci);
    if (!depth_) {
        std::cerr << "[GltfScene] depth texture alloc failed ("
                  << w << "x" << h << "): " << SDL_GetError() << "\n";
        return false;
    }

    depth_w_ = w;
    depth_h_ = h;
    return true;
}


bool GltfScene::loadFile(const std::filesystem::path& path,
                         RenderTree&                  tree,
                         NodeHandle                   parent_handle)
{
    namespace fg = fastgltf;

    // Verify that fastgltf's column-major fmat4x4 is blittable to float[16].
    // If this fires, replace the memcpy below with explicit column access:
    //   for(int c=0;c<4;++c) { world_mat[c*4+r] = world_mat_fg[c][r]; }
    static_assert(sizeof(fg::math::fmat4x4) == 16 * sizeof(float),
                  "fastgltf::math::fmat4x4 must be a standard-layout 64-byte type");

    // ── Map the glTF file into memory ─────────────────────────────────────

    auto mapped = fg::MappedGltfFile::FromPath(path);
    if (!mapped) {
        std::cerr << "[GltfScene] MappedGltfFile::FromPath failed: "
                  << path << "\n";
        return false;
    }

    // ── Parse ─────────────────────────────────────────────────────────────

    fg::Parser parser;

    constexpr auto opts = fg::Options::LoadExternalBuffers
                        | fg::Options::GenerateMeshIndices;

    auto result = parser.loadGltf(mapped.get(), path.parent_path(), opts);
    if (result.error() != fg::Error::None) {
        std::cerr << "[GltfScene] loadGltf error "
                  << static_cast<int>(result.error())
                  << " for: " << path << "\n";
        return false;
    }

    fg::Asset& asset = result.get();

    // ── Select default scene ──────────────────────────────────────────────

    const std::size_t scene_idx =
        asset.defaultScene.has_value() ? asset.defaultScene.value() : 0u;

    if (scene_idx >= asset.scenes.size()) {
        std::cerr << "[GltfScene] no scenes in: " << path << "\n";
        return false;
    }

    bool loaded_any = false;

    // mesh-id attr on the parent scene3d RenderNode overrides the proxy id
    // for the first primitive loaded from this file.  This lets the source
    // syntax say  scene3d(src="Ball.glb" mesh-id="ball")  and have the proxy
    // node carry id="ball" regardless of the internal glTF node name.
    // Subsequent primitives in a multi-mesh file get "ball-1", "ball-2", …
    const std::string mesh_id_override = [&]() -> std::string {
        const RenderNode* sn = tree.node(parent_handle);
        return sn ? std::string(sn->style("mesh-id")) : "";
    }();
    int proxy_count_for_scene = 0;

    // ── Traverse scene graph with accumulated world transforms ────────────
    //
    // fg::iterateSceneNodes calls the lambda for every node in the scene,
    // providing the fully-accumulated world-space transform for each one.

    fg::iterateSceneNodes(
        asset, scene_idx,
        fg::math::fmat4x4(1.f),   // identity: no extra parent transform
        [&](fg::Node& node, fg::math::fmat4x4 world_mat_fg)
        {
            // Only process nodes that own at least one mesh.
            if (!node.meshIndex.has_value()) return;

            const std::size_t mesh_idx = node.meshIndex.value();
            const fg::Mesh&   mesh     = asset.meshes[mesh_idx];

            // Copy the world-space transform to a flat column-major array.
            float world_mat[16];
            std::memcpy(world_mat, &world_mat_fg, sizeof(world_mat));

            // Stable proxy id: prefer node name, fall back to mesh name.
            const std::string node_id = normalizeId(
                node.name.empty() ? mesh.name : node.name);

            // ── Process each primitive in this mesh ───────────────────────

            for (std::size_t prim_idx = 0;
                 prim_idx < mesh.primitives.size();
                 ++prim_idx)
            {
                const fg::Primitive& prim = mesh.primitives[prim_idx];

                // POSITION is mandatory.
                auto pos_it = prim.findAttribute("POSITION");
                if (pos_it == prim.attributes.end()) continue;

                // Indices are mandatory (GenerateMeshIndices guarantees them).
                if (!prim.indicesAccessor.has_value()) continue;

                // ── Build interleaved vertex array ────────────────────────

                const fg::Accessor& pos_acc =
                    asset.accessors[pos_it->accessorIndex];
                const std::size_t vert_count = pos_acc.count;

                std::vector<GpuVertex> verts(vert_count);

                // Accumulate model-space AABB while copying positions.
                constexpr float kFMax = std::numeric_limits<float>::max();
                constexpr float kFMin = std::numeric_limits<float>::lowest();
                float aabb_min[3] = {kFMax, kFMax, kFMax};
                float aabb_max[3] = {kFMin, kFMin, kFMin};

                fg::iterateAccessorWithIndex<fg::math::fvec3>(
                    asset, pos_acc,
                    [&](fg::math::fvec3 v, std::size_t i) {
                        verts[i].px = v.x();
                        verts[i].py = v.y();
                        verts[i].pz = v.z();
                        aabb_min[0] = std::min(aabb_min[0], v.x());
                        aabb_min[1] = std::min(aabb_min[1], v.y());
                        aabb_min[2] = std::min(aabb_min[2], v.z());
                        aabb_max[0] = std::max(aabb_max[0], v.x());
                        aabb_max[1] = std::max(aabb_max[1], v.y());
                        aabb_max[2] = std::max(aabb_max[2], v.z());
                    });

                // Normals (optional — vertex struct is zero-initialised).
                auto norm_it = prim.findAttribute("NORMAL");
                if (norm_it != prim.attributes.end()) {
                    const fg::Accessor& norm_acc =
                        asset.accessors[norm_it->accessorIndex];
                    fg::iterateAccessorWithIndex<fg::math::fvec3>(
                        asset, norm_acc,
                        [&](fg::math::fvec3 v, std::size_t i) {
                            verts[i].nx = v.x();
                            verts[i].ny = v.y();
                            verts[i].nz = v.z();
                        });
                }

                // Texture coordinates (optional).
                auto uv_it = prim.findAttribute("TEXCOORD_0");
                if (uv_it != prim.attributes.end()) {
                    const fg::Accessor& uv_acc =
                        asset.accessors[uv_it->accessorIndex];
                    fg::iterateAccessorWithIndex<fg::math::fvec2>(
                        asset, uv_acc,
                        [&](fg::math::fvec2 v, std::size_t i) {
                            verts[i].u = v.x();
                            verts[i].v = v.y();
                        });
                }

                // ── Copy indices ──────────────────────────────────────────

                const fg::Accessor& idx_acc =
                    asset.accessors[prim.indicesAccessor.value()];
                const std::size_t   idx_count = idx_acc.count;

                std::vector<std::uint32_t> indices(idx_count);
                fg::copyFromAccessor<std::uint32_t>(asset, idx_acc,
                                                    indices.data());

                // ── Read material defaults ────────────────────────────────

                std::string mat_class;
                float       mat_color[4] = {1.f, 1.f, 1.f, 1.f};
                float       mat_rough    = 0.5f;
                float       mat_metal    = 0.0f;

                if (prim.materialIndex.has_value()) {
                    const auto& mat =
                        asset.materials[prim.materialIndex.value()];
                    mat_class    = normalizeId(mat.name);
                    mat_color[0] = mat.pbrData.baseColorFactor.x();
                    mat_color[1] = mat.pbrData.baseColorFactor.y();
                    mat_color[2] = mat.pbrData.baseColorFactor.z();
                    mat_color[3] = mat.pbrData.baseColorFactor.w();
                    mat_rough    = mat.pbrData.roughnessFactor;
                    mat_metal    = mat.pbrData.metallicFactor;
                }

                // ── GPU upload ────────────────────────────────────────────
                //
                // One transfer buffer per primitive (acceptable at load time).
                // Layout in the transfer buffer: [vertices | indices].

                const Uint32 vbytes =
                    static_cast<Uint32>(verts.size()   * sizeof(GpuVertex));
                const Uint32 ibytes =
                    static_cast<Uint32>(indices.size() * sizeof(std::uint32_t));

                // 1. Create transfer (staging) buffer.
                SDL_GPUTransferBufferCreateInfo tci{};
                tci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
                tci.size  = vbytes + ibytes;
                tci.props = 0;

                SDL_GPUTransferBuffer* tb =
                    SDL_CreateGPUTransferBuffer(device_, &tci);
                if (!tb) {
                    std::cerr << "[GltfScene] transfer buffer alloc failed\n";
                    continue;
                }

                // 2. Map → fill → unmap.
                void* ptr = SDL_MapGPUTransferBuffer(device_, tb, false);
                std::memcpy(ptr, verts.data(), vbytes);
                std::memcpy(static_cast<char*>(ptr) + vbytes,
                            indices.data(), ibytes);
                SDL_UnmapGPUTransferBuffer(device_, tb);

                // 3. Allocate persistent GPU buffers.
                GpuMesh gpu{};

                SDL_GPUBufferCreateInfo vbc{};
                vbc.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
                vbc.size  = vbytes;
                vbc.props = 0;
                gpu.vertex_buf = SDL_CreateGPUBuffer(device_, &vbc);

                SDL_GPUBufferCreateInfo ibc{};
                ibc.usage = SDL_GPU_BUFFERUSAGE_INDEX;
                ibc.size  = ibytes;
                ibc.props = 0;
                gpu.index_buf = SDL_CreateGPUBuffer(device_, &ibc);

                if (!gpu.vertex_buf || !gpu.index_buf) {
                    std::cerr << "[GltfScene] GPU buffer alloc failed\n";
                    SDL_ReleaseGPUTransferBuffer(device_, tb);
                    if (gpu.vertex_buf)
                        SDL_ReleaseGPUBuffer(device_, gpu.vertex_buf);
                    if (gpu.index_buf)
                        SDL_ReleaseGPUBuffer(device_, gpu.index_buf);
                    continue;
                }

                // 4. Record and submit a copy pass.
                SDL_GPUCommandBuffer* upload_cmd =
                    SDL_AcquireGPUCommandBuffer(device_);
                SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(upload_cmd);

                SDL_GPUTransferBufferLocation src{};
                src.transfer_buffer = tb;

                SDL_GPUBufferRegion dst{};

                // Vertex upload
                src.offset  = 0;
                dst.buffer  = gpu.vertex_buf;
                dst.offset  = 0;
                dst.size    = vbytes;
                SDL_UploadToGPUBuffer(cp, &src, &dst, false);

                // Index upload (immediately after vertex data in the TB)
                src.offset  = vbytes;
                dst.buffer  = gpu.index_buf;
                dst.offset  = 0;
                dst.size    = ibytes;
                SDL_UploadToGPUBuffer(cp, &src, &dst, false);

                SDL_EndGPUCopyPass(cp);
                SDL_SubmitGPUCommandBuffer(upload_cmd);
                SDL_ReleaseGPUTransferBuffer(device_, tb);

                // 5. Finish populating the GpuMesh descriptor.
                gpu.index_count = static_cast<Uint32>(idx_count);
                std::memcpy(gpu.aabb_min, aabb_min, sizeof(aabb_min));
                std::memcpy(gpu.aabb_max, aabb_max, sizeof(aabb_max));

                // ── Create jade proxy node ────────────────────────────────
                //
                //   • LayoutKind::None — excluded from flex / block layout.
                //   • draw   = nullptr — GltfScene::render() handles 3D draw.
                //   • update = nullptr — GltfScene::tick()  handles AABB projection.
                //   • dirty_render = false — 3D proxies never participate in
                //     the 2D offscreen-texture redraw.
                //
                //   x / y / w / h are written every frame by tick() to the
                //   projected screen-space AABB so css::StyleSheet::tickHover()
                //   works without any 3D-specific logic.

                const NodeHandle proxy_h = tree.alloc();
                RenderNode*      proxy   = tree.node(proxy_h);

                proxy->layout_kind  = LayoutKind::None;
                proxy->draw         = nullptr;
                proxy->update       = nullptr;
                proxy->dirty_render = false;

                proxy->setStyle("tag", "object3d");
                // Proxy id: mesh-id override (first prim gets bare override;
                // subsequent get "override-N") or normalised glTF node name.
                {
                    std::string eff_id;
                    if (!mesh_id_override.empty()) {
                        eff_id = (proxy_count_for_scene == 0)
                            ? mesh_id_override
                            : mesh_id_override + "-"
                              + std::to_string(proxy_count_for_scene);
                    } else {
                        eff_id = node_id;
                    }
                    if (!eff_id.empty())
                        proxy->setStyle("id", eff_id);
                }
                if (!mat_class.empty())
                    proxy->setStyle("class", mat_class);

                // Seed colour and PBR defaults from the glTF material.
                // css::StyleSheet::applyTo() will overwrite with CSS rules.
                {
                    char buf[32];
                    const auto byt = [](float f) -> unsigned {
                        return static_cast<unsigned>(
                            std::clamp(f, 0.f, 1.f) * 255.f + 0.5f);
                    };
                    std::snprintf(buf, sizeof(buf), "#%02x%02x%02x%02x",
                                  byt(mat_color[0]), byt(mat_color[1]),
                                  byt(mat_color[2]), byt(mat_color[3]));
                    proxy->setStyle("color", std::string(buf));

                    std::snprintf(buf, sizeof(buf), "%.6f", mat_rough);
                    proxy->setStyle("--roughness", std::string(buf));

                    std::snprintf(buf, sizeof(buf), "%.6f", mat_metal);
                    proxy->setStyle("--metallic", std::string(buf));
                }

                tree.appendChild(parent_handle, proxy_h);

                // ── Register MeshEntry ────────────────────────────────────

                MeshEntry entry{};
                entry.gpu            = gpu;
                entry.proxy_handle   = proxy_h;
                entry.scene3d_handle = parent_handle;
                std::memcpy(entry.world_mat, world_mat, sizeof(entry.world_mat));
                entries_.push_back(std::move(entry));

                ++proxy_count_for_scene;
                loaded_any = true;
            }
        }
    ); // end fg::iterateSceneNodes

    if (loaded_any) {
        std::cerr << "[GltfScene] loaded '" << path.filename().string()
                  << "' (" << entries_.size() << " total primitive(s))\n";
    } else {
        std::cerr << "[GltfScene] no mesh primitives found in: " << path << "\n";
    }

    return loaded_any;
}


void GltfScene::shutdown() noexcept
{
    if (!device_) return;

    // Release mesh GPU buffers.
    // SDL3 defers the actual GPU-side free until all commands referencing the
    // buffer have completed — no explicit SDL_WaitForGPUIdle needed.
    for (auto& e : entries_) {
        if (e.gpu.vertex_buf) {
            SDL_ReleaseGPUBuffer(device_, e.gpu.vertex_buf);
            e.gpu.vertex_buf = nullptr;
        }
        if (e.gpu.index_buf) {
            SDL_ReleaseGPUBuffer(device_, e.gpu.index_buf);
            e.gpu.index_buf = nullptr;
        }
    }
    entries_.clear();

    // Release depth texture.
    if (depth_) {
        SDL_ReleaseGPUTexture(device_, depth_);
        depth_   = nullptr;
        depth_w_ = 0;
        depth_h_ = 0;
    }

    // Release pipeline (shaders are released separately; the pipeline holds
    // its own internal references after creation).
    if (pipeline_) {
        SDL_ReleaseGPUGraphicsPipeline(device_, pipeline_);
        pipeline_ = nullptr;
    }
    if (vert_) {
        SDL_ReleaseGPUShader(device_, vert_);
        vert_ = nullptr;
    }
    if (frag_) {
        SDL_ReleaseGPUShader(device_, frag_);
        frag_ = nullptr;
    }

    tree_ref_ = nullptr;
    device_   = nullptr;
}

} // namespace pce::sdlos::gltf
