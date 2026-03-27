#include "image_cache.hh"

#include <algorithm>
#include <cstring>
#include <iostream>

#ifdef SDL_IMAGE_AVAILABLE
#  include <SDL3_image/SDL_image.h>
#endif

namespace pce::sdlos {

// ---- Lifecycle ----

bool ImageCache::init(SDL_GPUDevice* device) noexcept
{
    if (!device) {
        std::cerr << "[ImageCache] init: null device\n";
        return false;
    }

    device_  = device;
    sampler_ = createSampler();

    if (!sampler_) {
        std::cerr << "[ImageCache] init: failed to create sampler\n";
        device_ = nullptr;
        return false;
    }

    std::cerr << "[ImageCache] initialised\n";
    return true;
}

void ImageCache::shutdown() noexcept
{
    // Drain pending uploads — GPU textures are already registered in cache_,
    // so only the CPU surfaces need freeing here.
    for (auto& pu : pending_) {
        if (pu.surface) {
            SDL_DestroySurface(pu.surface);
            pu.surface = nullptr;
        }
        // pu.texture is owned by cache_ — do NOT release here.
    }
    pending_.clear();

    clearCache();

    if (sampler_ && device_) {
        SDL_ReleaseGPUSampler(device_, sampler_);
        sampler_ = nullptr;
    }

    device_ = nullptr;
}

// ---- Base path ----

void ImageCache::setBasePath(std::string path) noexcept
{
    base_path_ = std::move(path);
    if (!base_path_.empty() && base_path_.back() != '/')
        base_path_ += '/';
    std::cerr << "[ImageCache] base path: " << base_path_ << "\n";
}

// ---- Cache interface ----

ImageTexture ImageCache::ensureTexture(std::string_view path)
{
    if (!device_ || path.empty()) return {};

    // Resolve relative paths against base_path_ (set via setBasePath()).
    // Absolute paths (starting with '/') are used as-is.
    std::string key;
    if (!base_path_.empty() && !path.empty() && path[0] != '/')
        key = base_path_ + std::string(path);
    else
        key = std::string(path);

    // ── Cache hit ──────────────────────────────────────────────────────────
    if (auto it = cache_.find(key); it != cache_.end())
        return it->second;

    // ── Load CPU-side surface ──────────────────────────────────────────────
    SDL_Surface* raw = nullptr;

#ifdef SDL_IMAGE_AVAILABLE
    raw = IMG_Load(key.c_str());
#else
    // SDL_LoadBMP requires no extra library — BMP only, but zero-dependency.
    raw = SDL_LoadBMP(key.c_str());
#endif

    if (!raw) {
        std::cerr << "[ImageCache] failed to load '" << key
                  << "': " << SDL_GetError() << "\n";
        // Insert a null sentinel so we never retry a missing/corrupt file.
        cache_.emplace(key, ImageTexture{});
        return {};
    }

    // ── Convert to RGBA32 ─────────────────────────────────────────────────
    // SDL_PIXELFORMAT_RGBA32 is the byte-order-correct alias for R8G8B8A8 on
    // the host endianness — it matches SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM
    // expected by the GPU texture.  ConvertSurface is a no-op when the format
    // already matches, so the branch is free in the common case.
    SDL_Surface* surf = raw;
    if (raw->format != SDL_PIXELFORMAT_RGBA32) {
        surf = SDL_ConvertSurface(raw, SDL_PIXELFORMAT_RGBA32);
        SDL_DestroySurface(raw);
        if (!surf) {
            std::cerr << "[ImageCache] SDL_ConvertSurface failed for '" << key
                      << "': " << SDL_GetError() << "\n";
            cache_.emplace(key, ImageTexture{});
            return {};
        }
    }

    // ── Pre-allocate GPU texture ───────────────────────────────────────────
    // The texture is created now but contains undefined pixels until
    // flushUploads() runs during the next frame's copy pass.
    SDL_GPUTexture* tex = createTexture(surf->w, surf->h);
    if (!tex) {
        SDL_DestroySurface(surf);
        cache_.emplace(key, ImageTexture{});
        return {};
    }

    // Register in cache immediately — repeated calls within the same frame
    // return this entry (cache hit) rather than double-loading the file.
    const ImageTexture img{ tex, surf->w, surf->h };
    cache_.emplace(key, img);

    // Queue the upload for the next flushUploads() call.
    pending_.push_back(PendingUpload{ key, surf, tex });

    std::cerr << "[ImageCache] queued '" << key << "' ("
              << surf->w << "×" << surf->h << ")\n";

    return img;
}

void ImageCache::flushUploads(SDL_GPUCopyPass* copy_pass)
{
    if (!device_ || !copy_pass || pending_.empty()) return;

    for (auto& pu : pending_) {
        if (!pu.surface || !pu.texture) continue;

        const int w = pu.surface->w;
        const int h = pu.surface->h;

        // pitch may be > w*4 when SDL adds row padding; use it for
        // pixels_per_row so the upload stride matches the surface layout.
        const int    pitch      = pu.surface->pitch;
        const Uint32 byte_count = static_cast<Uint32>(pitch * h);

        // ── Create a one-shot host-visible transfer buffer ─────────────────
        SDL_GPUTransferBufferCreateInfo tbci{};
        tbci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        tbci.size  = byte_count;
        tbci.props = 0;

        SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(device_, &tbci);
        if (!tb) {
            std::cerr << "[ImageCache] SDL_CreateGPUTransferBuffer failed: "
                      << SDL_GetError() << "\n";
            SDL_DestroySurface(pu.surface);
            pu.surface = nullptr;
            continue;
        }

        // ── Map → copy surface pixels → unmap ─────────────────────────────
        void* ptr = SDL_MapGPUTransferBuffer(device_, tb, /*cycle=*/false);
        if (!ptr) {
            std::cerr << "[ImageCache] SDL_MapGPUTransferBuffer failed: "
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

        // ── Record the upload command into the copy pass ───────────────────
        SDL_GPUTextureTransferInfo tfi{};
        tfi.transfer_buffer = tb;
        tfi.offset          = 0;
        tfi.pixels_per_row  = static_cast<Uint32>(pitch / 4);  // stride in texels
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

        // Release the transfer buffer after recording — SDL3 GPU maintains
        // an internal ref until the GPU finishes; no need to keep it alive.
        SDL_ReleaseGPUTransferBuffer(device_, tb);

        SDL_DestroySurface(pu.surface);
        pu.surface = nullptr;
    }

    pending_.clear();
}

// ---- Cache management ----

void ImageCache::evict(std::string_view path)
{
    const std::string key(path);
    auto it = cache_.find(key);
    if (it == cache_.end()) return;
    if (it->second.texture && device_)
        SDL_ReleaseGPUTexture(device_, it->second.texture);
    cache_.erase(it);
}

void ImageCache::clearCache() noexcept
{
    if (device_) {
        for (auto& [path, img] : cache_) {
            if (img.texture)
                SDL_ReleaseGPUTexture(device_, img.texture);
        }
    }
    cache_.clear();
}

// ---- Private helpers ----

SDL_GPUTexture* ImageCache::createTexture(int w, int h) noexcept
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
        std::cerr << "[ImageCache] SDL_CreateGPUTexture failed: "
                  << SDL_GetError() << "\n";
    }
    return tex;
}

SDL_GPUSampler* ImageCache::createSampler() noexcept
{
    if (!device_) return nullptr;

    // Bilinear magnification + minification; clamp to edge on all axes so
    // image borders don't bleed into neighbouring atlas regions.
    // LINEAR mipmap mode for smooth downscaling when images are presented
    // at sizes smaller than their native resolution.
    SDL_GPUSamplerCreateInfo sci{};
    sci.min_filter        = SDL_GPU_FILTER_LINEAR;
    sci.mag_filter        = SDL_GPU_FILTER_LINEAR;
    sci.mipmap_mode       = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    sci.address_mode_u    = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sci.address_mode_v    = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sci.address_mode_w    = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sci.mip_lod_bias      = 0.f;
    sci.max_anisotropy    = 1.f;
    sci.compare_op        = SDL_GPU_COMPAREOP_NEVER;
    sci.min_lod           = 0.f;
    sci.max_lod           = 1000.f;
    sci.enable_anisotropy = false;
    sci.enable_compare    = false;
    sci.props             = 0;

    SDL_GPUSampler* samp = SDL_CreateGPUSampler(device_, &sci);
    if (!samp) {
        std::cerr << "[ImageCache] SDL_CreateGPUSampler failed: "
                  << SDL_GetError() << "\n";
    }
    return samp;
}

} // namespace pce::sdlos
