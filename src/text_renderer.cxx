#include "text_renderer.hh"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>

namespace pce::sdlos {

// ---- Lifecycle ----

bool TextRenderer::init(SDL_GPUDevice* device, float default_size)
{
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

void TextRenderer::shutdown()
{
    // Drain pending uploads: textures are owned by cache_; only free the
    // leftover CPU surfaces here.
    for (auto& pu : pending_uploads_) {
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

// ---- Font loading ----

bool TextRenderer::loadFont(const std::string& path, float pt_size)
{
#ifdef SDL_TTF_AVAILABLE
    return openFont(path, pt_size);
#else
    (void)path; (void)pt_size;
    return false;
#endif
}

bool TextRenderer::loadFirstAvailable(std::initializer_list<std::string_view> candidates,
                                       float pt_size)
{
#ifdef SDL_TTF_AVAILABLE
    for (std::string_view path : candidates) {
        if (loadFont(std::string(path), pt_size)) {
            std::cout << "[TextRenderer] loaded font: " << path << "\n";
            return true;
        }
    }
    return false;
#else
    (void)candidates; (void)pt_size;
    return false;
#endif
}

bool TextRenderer::tryLoadSystemFont(float pt_size)
{
#ifdef SDL_TTF_AVAILABLE
    // Explicit opt-in only — never called automatically.
    static constexpr const char* kSystemPaths[] = {
        // macOS / iOS
        "/System/Library/Fonts/HelveticaNeue.ttc",
        "/System/Library/Fonts/Helvetica.ttc",
        "/System/Library/Fonts/SFNS.ttf",
        "/System/Library/Fonts/SFNSText.ttf",
        "/System/Library/Fonts/Geneva.ttf",
        // Linux (common distros)
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/freefont/FreeSans.ttf",
        // XDG user fonts
        nullptr
    };
    for (const char* path : kSystemPaths) {
        if (!path) break;
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
bool TextRenderer::openFont(const std::string& path, float pt)
{
    TTF_Font* f = TTF_OpenFont(path.c_str(), pt);
    if (!f) return false;
    if (font_) TTF_CloseFont(font_);
    font_  = f;
    // ready_ flips here when a font is loaded after init() (sampler_ already set).
    ready_ = (sampler_ != nullptr);
    return true;
}
#endif

// ---- Cache interface ----

GlyphTexture TextRenderer::ensureTexture(std::string_view text, float size)
{
    if (!ready_) return {};

    const float  ns  = normaliseSize(size > 0.f ? size : default_size_);
    const Key    key { std::string(text), ns };

    // ── Cache hit ──────────────────────────────────────────────────────────
    if (auto it = cache_.find(key); it != cache_.end()) {
        return it->second;
    }

    // ── Cache miss: render CPU-side ────────────────────────────────────────
#ifdef SDL_TTF_AVAILABLE
    if (!font_) return {};

    // TODO: maintain a TTF_Font* cache keyed by size for multi-size support.
    // Currently we reuse font_ opened at default_size_.

    const SDL_Color white = {255, 255, 255, 255};
    SDL_Surface* raw = TTF_RenderText_Blended(
        font_,
        std::string(text).c_str(),
        0,      // length = 0 → use strlen
        white
    );
    if (!raw) {
        std::cerr << "[TextRenderer] TTF_RenderText_Blended failed: "
                  << SDL_GetError() << "\n";
        return {};
    }

    // Convert to RGBA32 so byte order matches SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM.
    SDL_Surface* surf = SDL_ConvertSurface(raw, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(raw);
    if (!surf) {
        std::cerr << "[TextRenderer] SDL_ConvertSurface failed: "
                  << SDL_GetError() << "\n";
        return {};
    }

    // Pre-allocate the GPU texture (no pixel data yet — filled in flushUploads).
    SDL_GPUTexture* tex = createTexture(surf->w, surf->h);
    if (!tex) {
        SDL_DestroySurface(surf);
        return {};
    }

    // Register in cache now so repeated calls within the same frame are hits.
    GlyphTexture gt{ tex, surf->w, surf->h };
    cache_.emplace(key, gt);

    pending_uploads_.push_back({ key, surf, tex });

    return gt;

#else
    (void)text; (void)size;
    return {};
#endif
}

void TextRenderer::flushUploads(SDL_GPUCopyPass* copy_pass)
{
    if (!device_ || !copy_pass || pending_uploads_.empty()) return;

    for (auto& pu : pending_uploads_) {
        if (!pu.surface || !pu.texture) continue;

        const int    w          = pu.surface->w;
        const int    h          = pu.surface->h;
        const Uint32 byte_count = static_cast<Uint32>(w * h * 4);

        // ── Create a one-shot transfer buffer ─────────────────────────────
        SDL_GPUTransferBufferCreateInfo tbci{};
        tbci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        tbci.size  = byte_count;
        tbci.props = 0;

        SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(device_, &tbci);
        if (!tb) {
            std::cerr << "[TextRenderer] SDL_CreateGPUTransferBuffer failed: "
                      << SDL_GetError() << "\n";
            SDL_DestroySurface(pu.surface);
            pu.surface = nullptr;
            continue;
        }

        // ── Copy surface pixels into the transfer buffer ──────────────────
        void* ptr = SDL_MapGPUTransferBuffer(device_, tb, /*cycle=*/false);
        if (!ptr) {
            std::cerr << "[TextRenderer] SDL_MapGPUTransferBuffer failed: "
                      << SDL_GetError() << "\n";
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
        tr.w = static_cast<Uint32>(w);
        tr.h = static_cast<Uint32>(h);
        tr.d = 1;

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

void TextRenderer::clearCache()
{
    if (device_) {
        for (auto& [key, gt] : cache_) {
            if (gt.texture) {
                SDL_ReleaseGPUTexture(device_, gt.texture);
            }
        }
    }
    cache_.clear();
}

// ---- Private helpers ----

float TextRenderer::normaliseSize(float s) noexcept
{
    // Round to nearest 0.5pt so near-duplicate sizes don't pollute the cache.
    s = std::clamp(s, 6.f, 256.f);
    return std::round(s * 2.f) / 2.f;
}

SDL_GPUTexture* TextRenderer::createTexture(int w, int h)
{
    if (!device_ || w <= 0 || h <= 0) return nullptr;

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

    SDL_GPUTexture* tex = SDL_CreateGPUTexture(device_, &tci);
    if (!tex) {
        std::cerr << "[TextRenderer] SDL_CreateGPUTexture failed: "
                  << SDL_GetError() << "\n";
    }
    return tex;
}

SDL_GPUSampler* TextRenderer::createSampler()
{
    if (!device_) return nullptr;

    SDL_GPUSamplerCreateInfo sci{};
    sci.min_filter     = SDL_GPU_FILTER_LINEAR;
    sci.mag_filter     = SDL_GPU_FILTER_LINEAR;
    sci.mipmap_mode    = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    sci.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sci.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sci.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sci.props          = 0;

    SDL_GPUSampler* s = SDL_CreateGPUSampler(device_, &sci);
    if (!s) {
        std::cerr << "[TextRenderer] SDL_CreateGPUSampler failed: "
                  << SDL_GetError() << "\n";
    }
    return s;
}

} // namespace pce::sdlos
