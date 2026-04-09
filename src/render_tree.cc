#include "render_tree.h"
#include "text_renderer.h"
#include "image_cache.h"
#include "video_texture.h"

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <vector>

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

namespace pce::sdlos {


std::expected<SDL_GPUShader*, std::string>

/**
 * @brief Loads shader
 *
 * @param device               SDL3 GPU device handle
 * @param backend              Blue channel component [0, 1]
 * @param name                 Human-readable name or identifier string
 * @param stage                String tag used for lookup or categorisation
 * @param num_samplers         Numeric count
 * @param num_uniform_buffers  Contiguous memory buffer
 *
 * @return Integer result; negative values indicate an error code
 */
loadShader(SDL_GPUDevice*     device,
           GPUBackend         backend,
           std::string_view   name,
           SDL_GPUShaderStage stage,
           uint32_t           num_samplers,
           uint32_t           num_uniform_buffers)
{
    namespace fs = std::filesystem;

    const std::string_view ext =
        (backend == GPUBackend::Metal) ? ".metallib" : ".spv";

    const std::string path =
        std::format("assets/shaders/{}{}", name, ext);

    if (!fs::exists(path)) {
        return std::unexpected(
            std::format("[loadShader] file not found: {}", path));
    }

    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return std::unexpected(
            std::format("[loadShader] cannot open: {}", path));
    }

    const auto file_size = static_cast<std::size_t>(file.tellg());
    file.seekg(0);

    std::vector<Uint8> code(file_size);
    if (!file.read(reinterpret_cast<char*>(code.data()),
                   static_cast<std::streamsize>(file_size))) {
        return std::unexpected(
            std::format("[loadShader] read error: {}", path));
    }

    const SDL_GPUShaderFormat fmt =
        (backend == GPUBackend::Metal)
            ? SDL_GPU_SHADERFORMAT_MSL
            : SDL_GPU_SHADERFORMAT_SPIRV;

    SDL_GPUShaderCreateInfo info{};
    info.code                = code.data();
    info.code_size           = code.size();
    info.entrypoint          = "main0";
    info.format              = fmt;
    info.stage               = stage;
    info.num_samplers        = num_samplers;
    info.num_uniform_buffers = num_uniform_buffers;
    info.props               = 0;

    SDL_GPUShader* shader = SDL_CreateGPUShader(device, &info);
    if (!shader) {
        return std::unexpected(
            std::format("[loadShader] SDL_CreateGPUShader failed for '{}': {}",
                        path, SDL_GetError()));
    }

    std::cout << std::format("[loadShader] loaded '{}'\n", path);
    return shader;
}


/**
 * @brief Draws rect
 *
 * @param x  Horizontal coordinate in logical pixels
 * @param y  Vertical coordinate in logical pixels
 * @param w  Width in logical pixels
 * @param h  Opaque resource handle
 * @param r  Red channel component [0, 1]
 * @param g  Green channel component [0, 1]
 * @param b  Blue channel component [0, 1]
 * @param a  Alpha channel component [0, 1]
 */
void RenderContext::drawRect(float x, float y, float w, float h,
                              float r, float g, float b, float a)
{
    if (!pass || !cmd) return;

    SDL_GPUGraphicsPipeline* pipe = pipeline("rect");
    if (!pipe) return;

    SDL_BindGPUGraphicsPipeline(pass, pipe);

    // 48 bytes — matches struct RectUniform in ui_rect.vert.metal (with UV fields).
    struct alignas(4) RectUniform {
        float x, y, w, h;
        float vw, vh;
        float uv_x, uv_y;
        float uv_w, uv_h;
        float _pad0, _pad1;
    };
    static_assert(sizeof(RectUniform) == 48, "RectUniform must be 48 bytes");

    const RectUniform vu{ x, y, w, h, viewport_w, viewport_h,
                          0.f, 0.f, 1.f, 1.f, 0.f, 0.f };
    SDL_PushGPUVertexUniformData(cmd, 0, &vu, sizeof(vu));

    // Must be 16 bytes — matches struct ColorUniform in ui_rect.frag.metal.
    struct alignas(4) ColorUniform {
        float r, g, b, a;
    };
    static_assert(sizeof(ColorUniform) == 16, "ColorUniform must be 16 bytes");

    const ColorUniform fu{ r, g, b, a };
    SDL_PushGPUFragmentUniformData(cmd, 0, &fu, sizeof(fu));

    SDL_DrawGPUPrimitives(pass, 6, 1, 0, 0);  // 6 verts = 2 tris = 1 quad
}

// drawText — the glyph texture must already be GPU-resident; call
// TextRenderer::flushUploads() BEFORE SDL_BeginGPURenderPass.
//
// Push uniform layout:
//   slot 0, vertex stage   → RectUniform  (same as drawRect)
//   slot 0, fragment stage → TintUniform  {r, g, b, a}
// Sampler binding:
//   fragment sampler slot 0 → glyph texture

/**
 * @brief Draws text
 *
 * @param text  UTF-8 text content
 * @param x     Horizontal coordinate in logical pixels
 * @param y     Vertical coordinate in logical pixels
 * @param size  Capacity or number of elements
 * @param r     Red channel component [0, 1]
 * @param g     Green channel component [0, 1]
 * @param b     Blue channel component [0, 1]
 * @param a     Alpha channel component [0, 1]
 * @param rtl   Red channel component [0, 1]
 */
void RenderContext::drawText(std::string_view text,
                               float x, float y,
                               float size,
                               float r, float g, float b, float a,
                               bool rtl)
{
    if (!pass || !cmd || !text_renderer) return;
    if (!text_renderer->isReady())       return;

    SDL_GPUGraphicsPipeline* pipe = pipeline("text");
    if (!pipe) return;

    const GlyphTexture gt = text_renderer->ensureTexture(text, size, rtl);
    if (!gt.valid()) return;

    SDL_BindGPUGraphicsPipeline(pass, pipe);

    struct alignas(4) RectUniform {
        float x, y, w, h;
        float vw, vh;
        float uv_x, uv_y;
        float uv_w, uv_h;
        float _pad0, _pad1;
    };
    const RectUniform vu{
        x, y,
        static_cast<float>(gt.width),
        static_cast<float>(gt.height),
        viewport_w, viewport_h,
        0.f, 0.f, 1.f, 1.f, 0.f, 0.f
    };
    SDL_PushGPUVertexUniformData(cmd, 0, &vu, sizeof(vu));

    struct alignas(4) TintUniform { float r, g, b, a; };
    const TintUniform fu{ r, g, b, a };
    SDL_PushGPUFragmentUniformData(cmd, 0, &fu, sizeof(fu));

    SDL_GPUTextureSamplerBinding sb{};
    sb.texture = gt.texture;
    sb.sampler = text_renderer->sampler();
    SDL_BindGPUFragmentSamplers(pass, 0, &sb, 1);

    SDL_DrawGPUPrimitives(pass, 6, 1, 0, 0);
}

/**
 * @brief Pipeline
 *
 * @param name  Human-readable name or identifier string
 *
 * @return Pointer to the result, or nullptr on failure
 */
SDL_GPUGraphicsPipeline* RenderContext::pipeline(std::string_view name)
{
    // Avoid operator[] — it inserts a default entry on miss.
    // Iterate directly; the map holds ≤ 8 entries.
    for (auto& [k, v] : pipelines) {
        if (k == name) return v;
    }
    return nullptr;
}

// drawRectOutline — four axis-aligned filled rects:
//
//   ┌─────────────────────┐   ← top    (x, y,        w,     t)
//   │                     │   ← left   (x, y+t,      t, h−2t)
//   │                     │   ← right  (x+w−t, y+t,  t, h−2t)
//   └─────────────────────┘   ← bottom (x, y+h−t,    w,     t)
//
// Top/bottom claim the corners; left/right span only h−2t for clean mitre joints.
// `thickness` is clamped to ≤ min(w,h)/2 so rects never have negative extents.

// drawImage — reuses the text pipeline (textured quad + tint uniform).
// tint = {1, 1, 1, opacity} preserves the image's original RGBA colours.
//
// Push uniform layout (identical to drawText):
//   vertex  slot 0 → RectUniform  {x, y, w, h, vw, vh, _, _}
//   fragment slot 0 → TintUniform {r, g, b, a}
// Sampler binding:
//   fragment sampler slot 0 → image texture

/**
 * @brief Draws image
 *
 * @param src      Source operand or data
 * @param x        Horizontal coordinate in logical pixels
 * @param y        Vertical coordinate in logical pixels
 * @param w        Width in logical pixels
 * @param h        Opaque resource handle
 * @param opacity
 * @param fit      VisualProps
 */
void RenderContext::drawImage(std::string_view src,
                               float x, float y, float w, float h,
                               float opacity, VisualProps::ObjectFit fit)
{
    if (!image_cache || !pass || !cmd) return;

    const ImageTexture img = image_cache->ensureTexture(src);
    if (!img.valid()) return;

    SDL_GPUGraphicsPipeline* pipe = pipeline("text");
    if (!pipe) return;

    // Compute draw rect and UV crop based on fit mode
    float dx = x, dy = y, dw = w, dh = h;
    float uv_x = 0.f, uv_y = 0.f, uv_w = 1.f, uv_h = 1.f;

    using OF = VisualProps::ObjectFit;

    if (fit == OF::Contain && img.width > 0 && img.height > 0) {
        // Scale uniformly to fit inside w×h; centre in the node rect.
        const float sw = w / static_cast<float>(img.width);
        const float sh = h / static_cast<float>(img.height);
        const float s  = std::min(sw, sh);
        dw = static_cast<float>(img.width)  * s;
        dh = static_cast<float>(img.height) * s;
        dx = x + (w - dw) * 0.5f;
        dy = y + (h - dh) * 0.5f;
        // Full UV — draw rect is already shrunk.

    } else if (fit == OF::Cover && img.width > 0 && img.height > 0) {
        // Keep draw rect = w×h.  UV-crop so the image fills without distortion.
        const float img_ar  = static_cast<float>(img.width)
                            / static_cast<float>(img.height);
        const float rect_ar = (h > 0.f) ? w / h : 1.f;

        if (img_ar > rect_ar) {
            // Image is wider than the rect: fit height, crop left/right.
            uv_w = rect_ar / img_ar;
            uv_x = (1.f - uv_w) * 0.5f;
        } else {
            // Image is taller than the rect: fit width, crop top/bottom.
            uv_h = img_ar / rect_ar;
            uv_y = (1.f - uv_h) * 0.5f;
        }
        // dx/dy/dw/dh stay at the full node rect.
    }
    // ObjectFit::Fill: full rect, full UV (defaults already set).

    SDL_BindGPUGraphicsPipeline(pass, pipe);

    struct alignas(4) RectUniform {
        float x, y, w, h;
        float vw, vh;
        float uv_x, uv_y;
        float uv_w, uv_h;
        float _pad0, _pad1;
    };
    const RectUniform vu{ dx, dy, dw, dh, viewport_w, viewport_h,
                          uv_x, uv_y, uv_w, uv_h, 0.f, 0.f };
    SDL_PushGPUVertexUniformData(cmd, 0, &vu, sizeof(vu));

    struct alignas(4) TintUniform { float r, g, b, a; };
    const TintUniform fu{ 1.f, 1.f, 1.f, opacity };
    SDL_PushGPUFragmentUniformData(cmd, 0, &fu, sizeof(fu));

    SDL_GPUTextureSamplerBinding sb{};
    sb.texture = img.texture;
    sb.sampler = image_cache->sampler();
    SDL_BindGPUFragmentSamplers(pass, 0, &sb, 1);

    SDL_DrawGPUPrimitives(pass, 6, 1, 0, 0);
}

/**
 * @brief Draws image with shader
 *
 * @param src            Source operand or data
 * @param shader_name    Human-readable name or identifier string
 * @param x              Horizontal coordinate in logical pixels
 * @param y              Vertical coordinate in logical pixels
 * @param w              Width in logical pixels
 * @param h              Opaque resource handle
 * @param opacity        Iterator position
 * @param fit            Iterator position
 * @param shader_params  Opaque resource handle
 */
void RenderContext::drawImageWithShader(std::string_view src,
                                         std::string_view shader_name,
                                         float x, float y, float w, float h,
                                         float opacity,
                                         VisualProps::ObjectFit fit,
                                         const NodeShaderParams& shader_params)
{
    if (!image_cache || !pass || !cmd) return;
    if (!nodeShaderPipeline) {
        // No pipeline provider — fall back to plain image draw.
        drawImage(src, x, y, w, h, opacity, fit);
        return;
    }

    SDL_GPUGraphicsPipeline* pipe = nodeShaderPipeline(shader_name);
    if (!pipe) {
        drawImage(src, x, y, w, h, opacity, fit);
        return;
    }

    const ImageTexture img = image_cache->ensureTexture(src);
    if (!img.valid()) return;

    // Compute draw rect and UV crop (same logic as drawImage)
    float dx = x, dy = y, dw = w, dh = h;
    float uv_x = 0.f, uv_y = 0.f, uv_w = 1.f, uv_h = 1.f;

    using OF = VisualProps::ObjectFit;
    if (fit == OF::Contain && img.width > 0 && img.height > 0) {
        const float sw = w / static_cast<float>(img.width);
        const float sh = h / static_cast<float>(img.height);
        const float s  = std::min(sw, sh);
        dw = static_cast<float>(img.width)  * s;
        dh = static_cast<float>(img.height) * s;
        dx = x + (w - dw) * 0.5f;
        dy = y + (h - dh) * 0.5f;
    } else if (fit == OF::Cover && img.width > 0 && img.height > 0) {
        const float img_ar  = static_cast<float>(img.width)
                            / static_cast<float>(img.height);
        const float rect_ar = (h > 0.f) ? w / h : 1.f;
        if (img_ar > rect_ar) {
            uv_w = rect_ar / img_ar;
            uv_x = (1.f - uv_w) * 0.5f;
        } else {
            uv_h = img_ar / rect_ar;
            uv_y = (1.f - uv_h) * 0.5f;
        }
    }

    SDL_BindGPUGraphicsPipeline(pass, pipe);

    struct alignas(4) RectUniform {
        float x, y, w, h;
        float vw, vh;
        float uv_x, uv_y;
        float uv_w, uv_h;
        float _pad0, _pad1;
    };
    const RectUniform vu{ dx, dy, dw, dh, viewport_w, viewport_h,
                          uv_x, uv_y, uv_w, uv_h, 0.f, 0.f };
    SDL_PushGPUVertexUniformData(cmd, 0, &vu, sizeof(vu));

    // Fragment buffer(0) = NodeShaderParams (32 bytes)
    SDL_PushGPUFragmentUniformData(cmd, 0, &shader_params, sizeof(shader_params));

    SDL_GPUTextureSamplerBinding sb{};
    sb.texture = img.texture;
    sb.sampler = image_cache->sampler();
    SDL_BindGPUFragmentSamplers(pass, 0, &sb, 1);

    SDL_DrawGPUPrimitives(pass, 6, 1, 0, 0);
}

/**
 * @brief Draws video
 *
 * @param x        Horizontal coordinate in logical pixels
 * @param y        Vertical coordinate in logical pixels
 * @param w        Width in logical pixels
 * @param h        Opaque resource handle
 * @param opacity  Iterator position
 */
void RenderContext::drawVideo(float x, float y, float w, float h, float opacity)
{
    if (!video_texture || !pass || !cmd) return;
    SDL_GPUTexture* tex = video_texture->texture();
    SDL_GPUSampler* smp = video_texture->sampler();
    if (!tex || !smp) return;

    SDL_GPUGraphicsPipeline* pipe = pipeline("text");
    if (!pipe) return;

    SDL_BindGPUGraphicsPipeline(pass, pipe);

    struct alignas(4) RectUniform {
        float x, y, w, h;
        float vw, vh;
        float uv_x, uv_y;
        float uv_w, uv_h;
        float _pad0, _pad1;
    };
    const RectUniform vu{ x, y, w, h, viewport_w, viewport_h,
                          0.f, 0.f, 1.f, 1.f, 0.f, 0.f };
    SDL_PushGPUVertexUniformData(cmd, 0, &vu, sizeof(vu));

    struct alignas(4) TintUniform { float r, g, b, a; };
    const TintUniform fu{ 1.f, 1.f, 1.f, opacity };
    SDL_PushGPUFragmentUniformData(cmd, 0, &fu, sizeof(fu));

    SDL_GPUTextureSamplerBinding sb{};
    sb.texture = tex;
    sb.sampler = smp;
    SDL_BindGPUFragmentSamplers(pass, 0, &sb, 1);

    SDL_DrawGPUPrimitives(pass, 6, 1, 0, 0);
}

/**
 * @brief Draws video with shader
 *
 * @param shader_name    Human-readable name or identifier string
 * @param x              Horizontal coordinate in logical pixels
 * @param y              Vertical coordinate in logical pixels
 * @param w              Width in logical pixels
 * @param h              Opaque resource handle
 * @param opacity        Iterator position
 * @param shader_params  Opaque resource handle
 */
void RenderContext::drawVideoWithShader(std::string_view shader_name,
                                        float x, float y, float w, float h,
                                        float opacity,
                                        const NodeShaderParams& shader_params)
{
    if (!video_texture || !pass || !cmd) return;
    SDL_GPUTexture* tex = video_texture->texture();
    SDL_GPUSampler* smp = video_texture->sampler();

    // No frame yet — draw a dark placeholder rect so the node isn't invisible.
    if (!tex || !smp) {
        drawRect(x, y, w, h, 0.05f, 0.05f, 0.05f, opacity);
        return;
    }

    if (!nodeShaderPipeline) {
        drawVideo(x, y, w, h, opacity);
        return;
    }

    SDL_GPUGraphicsPipeline* pipe = nodeShaderPipeline(shader_name);
    if (!pipe) {
        drawVideo(x, y, w, h, opacity);
        return;
    }

    SDL_BindGPUGraphicsPipeline(pass, pipe);

    struct alignas(4) RectUniform {
        float x, y, w, h;
        float vw, vh;
        float uv_x, uv_y;
        float uv_w, uv_h;
        float _pad0, _pad1;
    };
    const RectUniform vu{ x, y, w, h, viewport_w, viewport_h,
                          0.f, 0.f, 1.f, 1.f, 0.f, 0.f };
    SDL_PushGPUVertexUniformData(cmd, 0, &vu, sizeof(vu));

    // Fragment buffer(0) = NodeShaderParams (32 bytes)
    SDL_PushGPUFragmentUniformData(cmd, 0, &shader_params, sizeof(shader_params));

    SDL_GPUTextureSamplerBinding sb{};
    sb.texture = tex;
    sb.sampler = smp;
    SDL_BindGPUFragmentSamplers(pass, 0, &sb, 1);

    SDL_DrawGPUPrimitives(pass, 6, 1, 0, 0);
}

/**
 * @brief Draws rect outline
 *
 * @param x          Horizontal coordinate in logical pixels
 * @param y          Vertical coordinate in logical pixels
 * @param w          Width in logical pixels
 * @param h          Opaque resource handle
 * @param thickness  Upper bound
 * @param r          Red channel component [0, 1]
 * @param g          Green channel component [0, 1]
 * @param b          Blue channel component [0, 1]
 * @param a          Alpha channel component [0, 1]
 */
void RenderContext::drawRectOutline(float x, float y, float w, float h,
                                     float thickness,
                                     float r, float g, float b, float a)
{
    const float t = std::min(thickness, std::min(w, h) * 0.5f);
    if (t <= 0.f || w <= 0.f || h <= 0.f) return;

    drawRect(x,         y,         w,   t,       r, g, b, a);  // top
    drawRect(x,         y + h - t, w,   t,       r, g, b, a);  // bottom
    drawRect(x,         y + t,     t,   h - 2*t, r, g, b, a);  // left
    drawRect(x + w - t, y + t,     t,   h - 2*t, r, g, b, a);  // right
}


namespace {

    // The slot_map is structurally stable for the entire layout pass (no
    // alloc/free inside update callbacks), so RenderNode* pointers remain valid.
    // collectChildren — arena-backed flat array of a node's direct children.
    // Two-pass (count then fill). Span is valid until the next arena_.reset().

std::span<RenderNode*> collectChildren(RenderNode& n, RenderTree& tree,
                                        core::frame_arena& arena)
{
    std::size_t count = 0;
    for (NodeHandle c = n.child; c.valid(); ) {
        RenderNode* cn = tree.node(c);
        if (!cn) break;
        if (!cn->hidden) ++count;
        c = cn->sibling;
    }

    auto children = arena.allocSpan<RenderNode*>(count);



    std::size_t i = 0;
    for (NodeHandle c = n.child; c.valid(); ) {
        RenderNode* cn = tree.node(c);
        if (!cn) break;
        if (!cn->hidden) children[i++] = cn;
        c = cn->sibling;
    }

    return children;
}

// flexLayout — FlexRow (is_column=false, main=X cross=Y) and
//              FlexColumn (is_column=true,  main=Y cross=X).
//
// Three-pass algorithm:
//   Pass 1 — base sizes.
//     Priority: flex_basis → width/height → intrinsic (keep existing w/h).
//     Sentinel -1 means "not specified; use next fallback".
//   Pass 2 — flex-grow distribution.
//     remaining = container_main − total_fixed − total_gap
//     Distributed proportionally to flex_grow (grow only, no shrink).
//   Pass 3 — justify-content + align-items, then position.

void flexLayout(RenderNode& n, RenderTree& tree, core::frame_arena& arena,
                bool is_column)
{
    auto children = collectChildren(n, tree, arena);
    if (children.empty()) return;

    // Reduce the available layout space by the container's inner padding.
    const auto& vp              = n.visual_props;
    const float pad_main_start  = is_column ? vp.padding_top    : vp.padding_left;
    const float pad_main_end    = is_column ? vp.padding_bottom : vp.padding_right;
    const float pad_cross_start = is_column ? vp.padding_left   : vp.padding_top;
    const float pad_cross_end   = is_column ? vp.padding_right  : vp.padding_bottom;

    const float container_main  = (is_column ? n.h : n.w) - pad_main_start - pad_main_end;
    const float container_cross = (is_column ? n.w : n.h) - pad_cross_start - pad_cross_end;
    const float gap             = n.layout_props.gap;
    const std::size_t nc        = children.size();

    // Pass 1: base sizes

    float total_fixed = 0.f;
    float total_grow  = 0.f;

    for (RenderNode* child : children) {
        const LayoutProps& lp = child->layout_props;

        float basis = lp.flex_basis;
        if (basis < 0.f)
            basis = is_column ? lp.height : lp.width;

        if (basis >= 0.f) {
            if (is_column) child->h = basis;
            else           child->w = basis;
        }
        // Still -1 (auto): keep existing w/h — intrinsic size set by a draw
        // callback or parent layout on an earlier frame.

        total_fixed += is_column ? child->h : child->w;
        total_grow  += lp.flex_grow;
    }

    // Pass 2: flex-grow distribution
    const float total_gap_px = (nc > 1) ? float(nc - 1) * gap : 0.f;
    const float remaining    = container_main - total_fixed - total_gap_px;

    if (remaining > 0.f && total_grow > 0.f) {
        for (RenderNode* child : children) {
            const float grow = child->layout_props.flex_grow;
            if (grow > 0.f) {
                const float extra = remaining * (grow / total_grow);
                if (is_column) child->h += extra;
                else           child->w += extra;
            }
        }
    }

    // Pass 3: justify-content
    // free_space clamped ≥ 0: overflow containers don't produce negative
    // offsets (shrink is a future addition).

    float total_content = 0.f;
    for (RenderNode* child : children)
        total_content += is_column ? child->h : child->w;

    const float free_space =
        std::max(0.f, container_main - total_content - total_gap_px);

    float start_offset = 0.f;
    float extra_gap    = 0.f;   // additional spacing between items beyond `gap`

    using J = LayoutProps::Justify;
    switch (n.layout_props.justify) {
        case J::Start:
            start_offset = 0.f;
            break;
        case J::Center:
            start_offset = free_space * 0.5f;
            break;
        case J::End:
            start_offset = free_space;
            break;
        case J::SpaceBetween:
            start_offset = 0.f;
            extra_gap    = (nc > 1) ? free_space / float(nc - 1) : 0.f;
            break;
        case J::SpaceAround: {
            const float per = free_space / float(nc);
            start_offset    = per * 0.5f;
            extra_gap       = per;
            break;
        }
    }

    // Pass 3 cont.: position + cross-axis align
    float main_offset = pad_main_start + start_offset;

    using A = LayoutProps::Align;
    for (RenderNode* child : children) {
        const float old_x = child->x;
        const float old_y = child->y;
        const float old_w = child->w;
        const float old_h = child->h;

        const float child_main = is_column ? child->h : child->w;

        if (is_column) child->y = main_offset;
        else           child->x = main_offset;
        main_offset += child_main + gap + extra_gap;

        switch (n.layout_props.align) {
            case A::Start:
                if (is_column) child->x = pad_cross_start;
                else           child->y = pad_cross_start;
                break;
            case A::Center:
                if (is_column)
                    child->x = pad_cross_start + (container_cross - child->w) * 0.5f;
                else
                    child->y = pad_cross_start + (container_cross - child->h) * 0.5f;
                break;
            case A::End:
                if (is_column) child->x = pad_cross_start + container_cross - child->w;
                else           child->y = pad_cross_start + container_cross - child->h;
                break;
            case A::Stretch:
                // Only override the size when the child declared it as auto (-1).
                if (is_column) {
                    child->x = pad_cross_start;
                    if (child->layout_props.width < 0.f)
                        child->w = container_cross;
                } else {
                    child->y = pad_cross_start;
                    if (child->layout_props.height < 0.f)
                        child->h = container_cross;
                }
                break;
        }

        // Cascade layout to this child's subtree ONLY if its geometry changed.
        if (child->x != old_x || child->y != old_y ||
            child->w != old_w || child->h != old_h) {
            child->dirty_layout = true;
            child->dirty_render = true;
        }
    }

    // RTL FlexRow — mirror every child's X position so the first child in
    // DOM order sits on the RIGHT edge of the container (CSS direction:rtl
    // semantics: start == right).
    //
    //   x_rtl[i] = container_w − x_ltr[i] − child_w[i]
    //
    // This naturally reverses visual order without reordering the LCRS chain.
    if (!is_column && n.visual_props.direction == VisualProps::Direction::RTL) {
        for (RenderNode* child : children) {
            child->x = n.w - child->x - child->w;
            child->dirty_render = true;
        }
    }
}

// blockLayout — vertical stack with optional gap.
// A child with layout_props.width == -1 (auto) inherits the container width.
// Child heights are taken as-is (intrinsic).
// direction:rtl — fixed-width children are right-aligned within the container.

void blockLayout(RenderNode& n, RenderTree& tree)
{
    const auto& vp      = n.visual_props;
    const float gap     = n.layout_props.gap;
    const float avail_w = n.w - vp.padding_left - vp.padding_right;
    float y_offset      = vp.padding_top;
    const bool  rtl     = (vp.direction == VisualProps::Direction::RTL);

    for (NodeHandle c = n.child; c.valid(); ) {
        RenderNode* child = tree.node(c);
        if (!child) break;

        c = child->sibling; // advance now as we may continue
        if (child->hidden) continue;

        const float old_x = child->x;
        const float old_y = child->y;
        const float old_w = child->w;
        const float old_h = child->h;

        if (child->layout_props.width < 0.f) {  // -1 = auto: fill content area
            child->w = avail_w;
            child->x = vp.padding_left;
        } else {
            // Fixed-width: left-align for LTR, right-align for RTL.
            child->x = rtl
                       ? (n.w - vp.padding_right - child->w)
                       : vp.padding_left;
        }
        child->y = y_offset;
        y_offset += child->h + gap;

        // Cascade layout to this child's subtree ONLY if its geometry changed.
        if (child->x != old_x || child->y != old_y ||
            child->w != old_w || child->h != old_h) {
            child->dirty_layout = true;
            child->dirty_render = true;
        }
    }
}

} // anonymous namespace


/**
 * @brief Renders tree
 *
 * @param arena_size  Frame-scoped memory arena
 */
RenderTree::RenderTree(std::size_t arena_size)
    : arena_(arena_size)
{
}


/**
 * @brief Alloc
 *
 * @return Handle to the node, or k_null_handle on failure
 */
NodeHandle RenderTree::alloc()
{
    return nodes_.insert(RenderNode{});
}

/**
 * @brief Free
 *
 * @param handle  Opaque resource handle
 */
void RenderTree::free(NodeHandle handle)
{
    if (!handle.valid()) return;

    // Detach first so no dangling LCRS links remain.
    detach(handle);

    // BFS subtree collection via vector growth — index-based loop avoids
    // iterator invalidation when the vector reallocates.
    std::vector<NodeHandle> to_free;
    to_free.reserve(16);
    to_free.push_back(handle);

    for (std::size_t i = 0; i < to_free.size(); ++i) {
        const NodeHandle h = to_free[i];
        const RenderNode* n = nodes_.get(h);
        if (!n) continue;

        for (NodeHandle c = n->child; c.valid(); ) {
            const RenderNode* cn = nodes_.get(c);
            const NodeHandle  next = cn ? cn->sibling : k_null_handle;
            to_free.push_back(c);
            c = next;
        }
    }

    // Erasing increments each slot's generation — surviving handles to these
    // nodes become stale immediately.
    for (const NodeHandle h : to_free) {
        nodes_.erase(h);
    }
}


/**
 * @brief Node
 *
 * @param handle  Opaque resource handle
 *
 * @return Pointer to the result, or nullptr on failure
 */
RenderNode* RenderTree::node(NodeHandle handle)
{
    return nodes_.get(handle);
}

/**
 * @brief Node
 *
 * @param handle  Opaque resource handle
 *
 * @return Pointer to the result, or nullptr on failure
 */
const RenderNode* RenderTree::node(NodeHandle handle) const
{
    return nodes_.get(handle);
}


/**
 * @brief Appends child
 *
 * @param parent  Red channel component [0, 1]
 * @param child   Upper bound
 */
void RenderTree::appendChild(NodeHandle parent, NodeHandle child)
{
    RenderNode* p = nodes_.get(parent);
    RenderNode* c = nodes_.get(child);
    assert(p && "appendChild — invalid parent handle");
    assert(c && "appendChild — invalid child handle");

    if (c->parent.valid()) {
        detach(child);
        c = nodes_.get(child);   // re-fetch: detach does not invalidate
    }

    c->parent  = parent;
    c->sibling = k_null_handle;

    if (!p->child.valid()) {
        p->child = child;
        return;
    }

    NodeHandle last = p->child;
    for (;;) {
        RenderNode* ln = nodes_.get(last);
        assert(ln && "appendChild — corrupt sibling chain");
        if (!ln->sibling.valid()) {
            ln->sibling = child;
            break;
        }
        last = ln->sibling;
    }
}

/**
 * @brief Appends children
 *
 * @param parent    Red channel component [0, 1]
 * @param children  Upper bound
 */
void RenderTree::appendChildren(NodeHandle                  parent,
                                 std::span<const NodeHandle> children)
{
    for (const NodeHandle child : children) {
        appendChild(parent, child);
    }
}

/**
 * @brief Detaches
 *
 * @param child  Upper bound
 */
void RenderTree::detach(NodeHandle child)
{
    RenderNode* c = nodes_.get(child);
    if (!c || !c->parent.valid()) return;

    RenderNode* p = nodes_.get(c->parent);
    if (!p) {
        // Parent is stale — clear the child's link.
        c->parent  = k_null_handle;
        c->sibling = k_null_handle;
        return;
    }

    if (p->child == child) {
        p->child = c->sibling;
    } else {
        NodeHandle prev = p->child;
        while (prev.valid()) {
            RenderNode* pn = nodes_.get(prev);
            if (!pn) break;
            if (pn->sibling == child) {
                pn->sibling = c->sibling;
                break;
            }
            prev = pn->sibling;
        }
    }

    c->parent  = k_null_handle;
    c->sibling = k_null_handle;
}


/**
 * @brief Mark dirty
 *
 * @param handle  Opaque resource handle
 */
void RenderTree::markDirty(NodeHandle handle)
{
    for (NodeHandle h = handle; h.valid(); ) {
        RenderNode* n = nodes_.get(h);
        if (!n) break;
        n->dirty_render = true;
        h = n->parent;
    }
}

/**
 * @brief Mark layout dirty
 *
 * @param handle  Opaque resource handle
 */
void RenderTree::markLayoutDirty(NodeHandle handle)
{
    for (NodeHandle h = handle; h.valid(); ) {
        RenderNode* n = nodes_.get(h);
        if (!n) break;
        if (n->dirty_layout) break; // Optimization: parent is already dirty, stop propagation.
        n->dirty_layout = true;
        n->dirty_render = true;
        h = n->parent;
    }
}


/**
 * @brief Any dirty
 *
 * @param start  Red channel component [0, 1]
 *
 * @return true on success, false on failure
 */
bool RenderTree::anyDirty(NodeHandle start) const noexcept
{
    if (!start.valid()) return false;

    // Iterative DFS — avoids recursion overhead for deep trees.
    // Using a small stack allocated on the C++ stack where possible;
    // falls back to heap via std::vector for large scenes.
    struct Frame { NodeHandle h; };
    std::vector<Frame> stack;
    stack.reserve(32);
    stack.push_back({start});

    while (!stack.empty()) {
        const NodeHandle h = stack.back().h;
        stack.pop_back();

        const RenderNode* n = nodes_.get(h);
        if (!n) continue;

        if (n->dirty_render) return true;   // early-out on first hit

        for (NodeHandle c = n->child; c.valid(); ) {
            const RenderNode* cn = nodes_.get(c);
            if (!cn) break;
            stack.push_back({c});
            c = cn->sibling;
        }
    }
    return false;
}

/**
 * @brief Force all dirty
 *
 * @param start  Red channel component [0, 1]
 */
void RenderTree::forceAllDirty(NodeHandle start) noexcept
{
    if (!start.valid()) return;

    std::vector<NodeHandle> stack;
    stack.reserve(32);
    stack.push_back(start);

    while (!stack.empty()) {
        const NodeHandle h = stack.back();
        stack.pop_back();

        RenderNode* n = nodes_.get(h);
        if (!n) continue;

        n->dirty_render = true;

        for (NodeHandle c = n->child; c.valid(); ) {
            const RenderNode* cn = nodes_.get(c);
            if (!cn) break;
            stack.push_back(c);
            c = cn->sibling;
        }
    }
}


/**
 * @brief Searches for and returns by id
 *
 * @param start  Red channel component [0, 1]
 * @param id     Unique object identifier
 *
 * @return Handle to the node, or k_null_handle on failure
 */
NodeHandle RenderTree::findById(NodeHandle start, std::string_view id) const
{
    if (!start.valid()) return k_null_handle;
    const RenderNode* n = nodes_.get(start);
    if (!n) return k_null_handle;
    if (n->style("id") == id) return start;
    for (NodeHandle c = n->child; c.valid(); ) {
        const RenderNode* cn = nodes_.get(c);
        if (!cn) break;
        const NodeHandle found = findById(c, id);
        if (found.valid()) return found;
        c = cn->sibling;
    }
    return k_null_handle;
}

/**
 * @brief Searches for and returns by class
 *
 * @param start  Red channel component [0, 1]
 * @param cls    Signed 32-bit integer
 *
 * @return Integer result; negative values indicate an error code
 */
std::vector<NodeHandle> RenderTree::findByClass(NodeHandle start,
                                                std::string_view cls) const
{
    // Returns true when the space-separated token list contains `tok` exactly.
    const auto hasToken = [](std::string_view list, std::string_view tok) noexcept {
        std::size_t pos = 0;
        while (pos <= list.size()) {
            const std::size_t end  = list.find(' ', pos);
            const std::size_t len  = (end == std::string_view::npos)
                                     ? list.size() - pos
                                     : end - pos;
            if (list.substr(pos, len) == tok) return true;
            if (end == std::string_view::npos) break;
            pos = end + 1;
        }
        return false;
    };

    std::vector<NodeHandle> result;
    std::vector<NodeHandle> stack;
    if (start.valid()) stack.push_back(start);

    while (!stack.empty()) {
        const NodeHandle  h = stack.back();
        stack.pop_back();
        const RenderNode* n = nodes_.get(h);
        if (!n) continue;

        if (hasToken(n->style("class"), cls))
            result.push_back(h);

        // Push children right-to-left so the leftmost child is visited first.
        std::vector<NodeHandle> kids;
        for (NodeHandle c = n->child; c.valid(); ) {
            const RenderNode* cn = nodes_.get(c);
            if (!cn) break;
            kids.push_back(c);
            c = cn->sibling;
        }
        for (auto it = kids.rbegin(); it != kids.rend(); ++it)
            stack.push_back(*it);
    }
    return result;
}

// traverse — iterative preorder DFS (parent before children, left-to-right).
//
// Children are pushed left→right onto the stack then the newly pushed segment
// is reversed in-place so the leftmost child sits on top — allocation-free
// ordering trick, no temporary container needed.
// Stale handles are silently skipped via the nullptr check on nodes_.get().
//
// Mutation constraint: fn must not call alloc() or free() during traversal.
// slot_map::insert() may reallocate its backing vector, invalidating any
// RenderNode* held by fn. Queue structural mutations and apply after render.
/**
 * @brief Traverse
 */
/**
 * @brief Traverse
 */
template<std::invocable<NodeHandle, RenderNode&> Fn>
void RenderTree::traverse(NodeHandle start, Fn&& fn)
{
    if (!start.valid()) return;

    std::vector<NodeHandle> stack;
    stack.reserve(64);
    stack.push_back(start);

    while (!stack.empty()) {
        const NodeHandle h = stack.back();
        stack.pop_back();

        RenderNode* n = nodes_.get(h);
        if (!n) continue;   // stale handle — skip gracefully

        fn(h, *n);

        const auto base = static_cast<std::ptrdiff_t>(stack.size());

        for (NodeHandle c = n->child; c.valid(); ) {
            const RenderNode* cn = nodes_.get(c);
            const NodeHandle  next = cn ? cn->sibling : k_null_handle;
            stack.push_back(c);
            c = next;
        }

        std::reverse(stack.begin() + base, stack.end());
    }
}


/**
 * @brief Begins frame
 */
void RenderTree::beginFrame()
{
    // O(1) bump-allocator reset: reclaims all arena memory from the previous
    // frame in a single instruction — no individual frees, no fragmentation.
    arena_.reset();
}

/**
 * @brief Updates
 *
 * @param root  Red channel component [0, 1]
 */
void RenderTree::update(NodeHandle root)
{
    const NodeHandle start = root.valid() ? root : root_;
    if (!start.valid()) return;

    // Because traversal is preorder, resolveLayout on a dirty_layout node
    // writes geometry onto direct children and marks them dirty_layout too —
    // their own layouts then run when they are visited, cascading down the
    // tree in one pass without a separate fixpoint loop.
    traverse(start, [this](NodeHandle h, RenderNode& n) {
        if (n.dirty_layout) {
            resolveLayout(h, n);
            n.dirty_layout = false;
        }
        if (n.update) n.update();
    });
}

/**
 * @brief Resolves layout
 *
 * @param param0  Red channel component [0, 1]
 * @param n       RenderNode & value
 */
void RenderTree::resolveLayout(NodeHandle /*h*/, RenderNode& n)
{
    switch (n.layout_kind) {
        case LayoutKind::FlexRow:
            flexLayout(n, *this, arena_, false);
            break;
        case LayoutKind::FlexColumn:
            flexLayout(n, *this, arena_, true);
            break;
        case LayoutKind::Block:
            blockLayout(n, *this);
            break;
        case LayoutKind::None:
        case LayoutKind::Grid:
            // Absolute / caller-managed. Grid reserved for Phase 3.
            break;
    }
}

/**
 * @brief Renders
 *
 * @param root  Red channel component [0, 1]
 * @param ctx   Execution or rendering context
 */
void RenderTree::render(NodeHandle root, RenderContext& ctx)
{
    if (!root.valid()) return;

    // Iterative pre/post ("envelope") traversal.
    //
    // Each entry carries a "post" flag:
    //   post=false  →  draw this node, push children, push post-sentinel if needed
    //   post=true   →  all children done; call after_draw (used for scissor restore,
    //                  mask pop, etc.)
    //
    // Children are pushed right-to-left so the leftmost child is processed first.

    struct Entry { NodeHandle h; bool post; };
    std::vector<Entry> stack;
    stack.reserve(64);
    stack.push_back({root, false});

    while (!stack.empty()) {
        const auto [h, post] = stack.back();
        stack.pop_back();

        RenderNode* n = nodes_.get(h);
        if (!n) continue;

        if (post) {
            // Post-children hook — restore GPU state set in draw().
            if (!n->hidden && n->after_draw) n->after_draw(ctx);
            continue;
        }

        // Skip hidden nodes and their subtrees entirely.
        if (n->hidden) continue;

        // Draw this node.
        if (n->draw) {
            n->draw(ctx);
            n->dirty_render = false;
        }

        // If this node needs a post-children call, push the sentinel now
        // so it fires after all descendants have been processed.
        if (n->after_draw)
            stack.push_back({h, true});

        // Push children right-to-left so the leftmost is on top of the stack.
        std::vector<NodeHandle> kids;
        for (NodeHandle c = n->child; c.valid(); ) {
            const RenderNode* cn = nodes_.get(c);
            if (!cn) break;
            if (!cn->hidden) kids.push_back(c);
            c = cn->sibling;
        }
        for (auto it = kids.rbegin(); it != kids.rend(); ++it)
            stack.push_back({*it, false});
    }
}

} // namespace pce::sdlos
