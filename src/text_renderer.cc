#include "text_renderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>

namespace pce::sdlos {

/**
 * @brief Initialises
 *
 * @param device        SDL3 GPU device handle
 * @param default_size  Capacity or number of elements
 *
 * @return true on success, false on failure
 */
bool TextRenderer::init(SDL_GPUDevice *device, float default_size) {
    if (!device) {
        std::cerr << "[TextRenderer] init: null device\n";
        return false;
    }

    device_       = device;
    default_size_ = default_size;

#ifdef SDL_TTF_AVAILABLE
    if (!TTF_Init()) {
        std::cerr << "[TextRenderer] TTF_Init failed: " << SDL_GetError() << "\n";
        return false;
    }
    ttf_inited_ = true;
#else
    std::cerr << "[TextRenderer] SDL_ttf not compiled in — text rendering disabled\n";
    return false;
#endif

    sampler_ = createSampler();
    if (!sampler_) {
        std::cerr << "[TextRenderer] failed to create sampler\n";
#ifdef SDL_TTF_AVAILABLE
        TTF_Quit();
        ttf_inited_ = false;
#endif
        return false;
    }

    // Font is loaded separately — isReady() returns false until loadFont()
    // or loadFirstAvailable() succeeds.
    std::cout << "[TextRenderer] initialised (no font loaded yet)\n";
    return true;
}

/**
 * @brief Shuts down
 */
void TextRenderer::shutdown() {
    // Drain pending uploads: textures are owned by cache_; only free the
    // leftover CPU surfaces here.
    for (auto &pu : pending_uploads_) {
        if (pu.surface) {
            SDL_DestroySurface(pu.surface);
            pu.surface = nullptr;
        }
        // pu.texture is owned by cache_ — do not release here.
    }
    pending_uploads_.clear();

    clearCache();

    if (sampler_) {
        SDL_ReleaseGPUSampler(device_, sampler_);
        sampler_ = nullptr;
    }

#ifdef SDL_TTF_AVAILABLE
    if (font_) {
        TTF_CloseFont(font_);
        font_ = nullptr;
    }
    if (ttf_inited_) {
        TTF_Quit();
        ttf_inited_ = false;
    }
#endif

    device_ = nullptr;
    ready_  = false;
}

/**
 * @brief Loads font
 *
 * @param path     Filesystem path
 * @param pt_size  Capacity or number of elements
 *
 * @return true on success, false on failure
 */
bool TextRenderer::loadFont(const std::string &path, float pt_size) {
#ifdef SDL_TTF_AVAILABLE
    return openFont(path, pt_size);
#else
    (void)path;
    (void)pt_size;
    return false;
#endif
}

bool TextRenderer::loadFirstAvailable(
    std::initializer_list<std::string_view> candidates,
    float pt_size) {
#ifdef SDL_TTF_AVAILABLE
    for (std::string_view path : candidates) {
        if (loadFont(std::string(path), pt_size)) {
            std::cout << "[TextRenderer] loaded font: " << path << "\n";
            return true;
        }
    }
    return false;
#else
    (void)candidates;
    (void)pt_size;
    return false;
#endif
}

/**
 * @brief Try load system font
 *
 * @param pt_size  Capacity or number of elements
 *
 * @return true on success, false on failure
 */
bool TextRenderer::tryLoadSystemFont(float pt_size) {
#ifdef SDL_TTF_AVAILABLE
    // Explicit opt-in only — never called automatically.
    static constexpr const char *kSystemPaths[] = {
        // macOS / iOS — Latin-first.  HelveticaNeue is present on every macOS
        // version (10.9+) and has excellent Latin/Greek/Cyrillic coverage.
        "/System/Library/Fonts/HelveticaNeue.ttc",
        "/System/Library/Fonts/Helvetica.ttc",
        // San Francisco (present as SFNS on older macOS, split files on newer)
        "/System/Library/Fonts/SFNS.ttf",
        "/System/Library/Fonts/SFNSText.ttf",
        "/System/Library/Fonts/Geneva.ttf",
        // GeezaPro last on macOS — Arabic + Latin fallback only.
        // It is tried last so a Latin-capable font wins for the primary slot.
        // For genuine Arabic rendering, supply a bundled font in assets/fonts/.
        "/System/Library/Fonts/GeezaPro.ttc",
        // Linux — DejaVu Sans has broad Latin/Greek/Cyrillic/Arabic subset coverage.
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/freefont/FreeSans.ttf",
        // Linux — Noto Arabic (broad Arabic coverage, tried after Latin fonts)
        "/usr/share/fonts/truetype/noto/NotoSansArabic-Regular.ttf",
        "/usr/share/fonts/opentype/noto/NotoSansArabic-Regular.ttf",
        "/usr/share/fonts/noto/NotoSansArabic-Regular.ttf",
        nullptr};
    for (const char *path : kSystemPaths) {
        if (!path)
            break;
        if (loadFont(path, pt_size)) {
            std::cout << "[TextRenderer] loaded system font: " << path << "\n";
            return true;
        }
    }
    return false;
#else
    (void)pt_size;
    return false;
#endif
}

#ifdef SDL_TTF_AVAILABLE
bool TextRenderer::openFont(const std::string &path, float pt) {
    TTF_Font *f = TTF_OpenFont(path.c_str(), pt);
    if (!f)
        return false;
    if (font_)
        TTF_CloseFont(font_);
    font_ = f;
    // ready_ flips here when a font is loaded after init() (sampler_ already set).
    ready_ = (sampler_ != nullptr);
    return true;
}
#endif

// ---- Cache interface ----

/**
 * @brief Ensure texture
 *
 * @param text  UTF-8 text content
 * @param size  Capacity or number of elements
 * @param rtl   Red channel component [0, 1]
 *
 * @return GlyphTexture result
 */
GlyphTexture TextRenderer::ensureTexture(std::string_view text, float size, bool rtl) {
    if (!ready_ || text.empty())
        return {};

    const float ns = normaliseSize(size > 0.f ? size : default_size_);
    const Key key{std::string(text), ns, rtl};

    // ── Cache hit ──────────────────────────────────────────────────────────
    if (auto it = cache_.find(key); it != cache_.end()) {
        return it->second;
    }

    // ── Cache miss: render CPU-side ────────────────────────────────────────
#ifdef SDL_TTF_AVAILABLE
    if (!font_)
        return {};

    // TODO: maintain a TTF_Font* cache keyed by size for multi-size support.
    // Currently we reuse font_ opened at default_size_.

    // Set HarfBuzz text direction — RTL enables Arabic/Hebrew contextual
    // shaping (ligatures, contextual forms, mirrored brackets, etc.).
    // TTF_DIRECTION_LTR = 4, TTF_DIRECTION_RTL = 5 in SDL3_ttf.
    TTF_SetFontDirection(font_, rtl ? TTF_DIRECTION_RTL : TTF_DIRECTION_LTR);

    const SDL_Color white = {255, 255, 255, 255};
    SDL_Surface *raw      = TTF_RenderText_Blended(
        font_,
        std::string(text).c_str(),
        0,  // length = 0 → use strlen
        white);
    if (!raw) {
        std::cerr << "[TextRenderer] TTF_RenderText_Blended failed: " << SDL_GetError() << "\n";
        return {};
    }

    // Convert to RGBA32 so byte order matches SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM.
    SDL_Surface *surf = SDL_ConvertSurface(raw, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(raw);
    if (!surf) {
        std::cerr << "[TextRenderer] SDL_ConvertSurface failed: " << SDL_GetError() << "\n";
        return {};
    }

    // Pre-allocate the GPU texture (no pixel data yet — filled in flushUploads).
    SDL_GPUTexture *tex = createTexture(surf->w, surf->h);
    if (!tex) {
        SDL_DestroySurface(surf);
        return {};
    }

    // Register in cache now so repeated calls within the same frame are hits.
    GlyphTexture gt{tex, surf->w, surf->h};
    cache_.emplace(key, gt);

    pending_uploads_.push_back({key, surf, tex});

    return gt;

#else
    (void)text;
    (void)size;
    return {};
#endif
}

/**
 * @brief Flushes uploads
 *
 * @param copy_pass  Vertical coordinate in logical pixels
 */
void TextRenderer::flushUploads(SDL_GPUCopyPass *copy_pass) {
    if (!device_ || !copy_pass || pending_uploads_.empty())
        return;

    for (auto &pu : pending_uploads_) {
        if (!pu.surface || !pu.texture)
            continue;

        const int w             = pu.surface->w;
        const int h             = pu.surface->h;
        const Uint32 byte_count = static_cast<Uint32>(w * h * 4);

        // ── Create a one-shot transfer buffer ─────────────────────────────
        SDL_GPUTransferBufferCreateInfo tbci{};
        tbci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        tbci.size  = byte_count;
        tbci.props = 0;

        SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(device_, &tbci);
        if (!tb) {
            std::cerr << "[TextRenderer] SDL_CreateGPUTransferBuffer failed: " << SDL_GetError()
                      << "\n";
            SDL_DestroySurface(pu.surface);
            pu.surface = nullptr;
            continue;
        }

        // ── Copy surface pixels into the transfer buffer ──────────────────
        void *ptr = SDL_MapGPUTransferBuffer(device_, tb, /*cycle=*/false);
        if (!ptr) {
            std::cerr << "[TextRenderer] SDL_MapGPUTransferBuffer failed: " << SDL_GetError()
                      << "\n";
            SDL_ReleaseGPUTransferBuffer(device_, tb);
            SDL_DestroySurface(pu.surface);
            pu.surface = nullptr;
            continue;
        }

        SDL_LockSurface(pu.surface);
        std::memcpy(ptr, pu.surface->pixels, byte_count);
        SDL_UnlockSurface(pu.surface);

        SDL_UnmapGPUTransferBuffer(device_, tb);

        // ── Upload via the copy pass ──────────────────────────────────────
        SDL_GPUTextureTransferInfo tfi{};
        tfi.transfer_buffer = tb;
        tfi.offset          = 0;
        tfi.pixels_per_row  = static_cast<Uint32>(w);
        tfi.rows_per_layer  = static_cast<Uint32>(h);

        SDL_GPUTextureRegion tr{};
        tr.texture   = pu.texture;
        tr.mip_level = 0;
        tr.layer     = 0;
        tr.x = tr.y = tr.z = 0;
        tr.w               = static_cast<Uint32>(w);
        tr.h               = static_cast<Uint32>(h);
        tr.d               = 1;

        SDL_UploadToGPUTexture(copy_pass, &tfi, &tr, /*cycle=*/false);

        // Release the transfer buffer after the copy command is recorded.
        // SDL3 GPU holds an internal reference until the GPU finishes, so
        // this is safe — we do not need to keep the buffer alive ourselves.
        SDL_ReleaseGPUTransferBuffer(device_, tb);
        SDL_DestroySurface(pu.surface);
        pu.surface = nullptr;
    }

    pending_uploads_.clear();
}

/**
 * @brief Measure text
 *
 * @param text    UTF-8 text content
 * @param param1  Red channel component [0, 1]
 * @param rtl     Red channel component [0, 1]
 *
 * @return Integer result; negative values indicate an error code
 */
std::pair<int, int> TextRenderer::measureText(std::string_view text, float /*size*/, bool rtl) {
    if (!ready_ || text.empty())
        return {0, 0};

#ifdef SDL_TTF_AVAILABLE
    if (!font_)
        return {0, 0};

    // Match the direction used by ensureTexture so measurements are consistent
    // with the actual rendered glyph dimensions.
    TTF_SetFontDirection(font_, rtl ? TTF_DIRECTION_RTL : TTF_DIRECTION_LTR);

    int w = 0, h = 0;
    // TTF_GetStringSize measures without allocating a GPU texture.
    // `size` is accepted for API symmetry but currently ignored — the font is
    // loaded at a fixed point size and all text (render + measure) uses it.
    // Multi-size font caching is a future TODO.
    if (!TTF_GetStringSize(font_, text.data(), text.size(), &w, &h)) {
        // Measurement can fail for empty strings or if the font is broken;
        // return {0, 0} so callers fall back gracefully.
        return {0, 0};
    }
    return {w, h};
#else
    return {0, 0};
#endif
}

/**
 * @brief Clears cache
 */
void TextRenderer::clearCache() {
    if (device_) {
        for (auto &[key, gt] : cache_) {
            if (gt.texture) {
                SDL_ReleaseGPUTexture(device_, gt.texture);
            }
        }
    }
    cache_.clear();
}

/**
 * @brief Normalise size
 *
 * @param s  32-bit floating-point scalar
 *
 * @return Computed floating-point value
 *
 * @warning Factory function 'createTexture' returns a non-const raw pointer — Raw
 *          pointer parameter — ownership is ambiguous; consider std::span (non-owning
 *          view), std::unique_ptr (transfer), or const T* (borrow)
 */
float TextRenderer::normaliseSize(float s) noexcept {
    // Round to nearest 0.5pt so near-duplicate sizes don't pollute the cache.
    s = std::clamp(s, 6.f, 256.f);
    return std::round(s * 2.f) / 2.f;
}

/**
 * @brief Creates and returns texture
 *
 * @param w  Width in logical pixels
 * @param h  Opaque resource handle
 *
 * @return Pointer to the result, or nullptr on failure
 *
 * @warning Factory function 'createTexture' returns a non-const raw pointer — Raw
 *          pointer parameter — ownership is ambiguous; consider std::span (non-owning
 *          view), std::unique_ptr (transfer), or const T* (borrow)
 */
SDL_GPUTexture *TextRenderer::createTexture(int w, int h) {
    if (!device_ || w <= 0 || h <= 0)
        return nullptr;

    SDL_GPUTextureCreateInfo tci{};
    tci.type                 = SDL_GPU_TEXTURETYPE_2D;
    tci.format               = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    tci.usage                = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    tci.width                = static_cast<Uint32>(w);
    tci.height               = static_cast<Uint32>(h);
    tci.layer_count_or_depth = 1;
    tci.num_levels           = 1;
    tci.sample_count         = SDL_GPU_SAMPLECOUNT_1;
    tci.props                = 0;

    SDL_GPUTexture *tex = SDL_CreateGPUTexture(device_, &tci);
    if (!tex) {
        std::cerr << "[TextRenderer] SDL_CreateGPUTexture failed: " << SDL_GetError() << "\n";
    }
    return tex;
}

/**
 * @brief Creates and returns sampler
 *
 * @return Pointer to the result, or nullptr on failure
 *
 * @warning Factory function 'createSampler' returns a non-const raw pointer — Raw
 *          pointer parameter — ownership is ambiguous; consider std::span (non-owning
 *          view), std::unique_ptr (transfer), or const T* (borrow)
 */
SDL_GPUSampler *TextRenderer::createSampler() {
    if (!device_)
        return nullptr;

    SDL_GPUSamplerCreateInfo sci{};
    sci.min_filter     = SDL_GPU_FILTER_LINEAR;
    sci.mag_filter     = SDL_GPU_FILTER_LINEAR;
    sci.mipmap_mode    = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    sci.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sci.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sci.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sci.props          = 0;

    SDL_GPUSampler *s = SDL_CreateGPUSampler(device_, &sci);
    if (!s) {
        std::cerr << "[TextRenderer] SDL_CreateGPUSampler failed: " << SDL_GetError() << "\n";
    }
    return s;
}

}  // namespace pce::sdlos
