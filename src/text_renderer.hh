#pragma once

// text_renderer.hh — SDL_ttf-backed text-to-GPU-texture cache.
//
// Namespace : sdlos
// File      : src/text_renderer.hh
//
// Responsibilities
// ================
//   TextRenderer owns a TTF_Font and maintains a cache that maps
//   (text, point-size) → SDL_GPUTexture*.  A texture is created the first
//   time a string is requested and reused on every subsequent call.
//
//   GPU upload flow (SDL3 GPU)
//   --------------------------
//   Text is rendered CPU-side via TTF_RenderText_Blended() into an SDL_Surface.
//   The surface pixels are then transferred to the GPU in two steps:
//
//     1.  ensureTexture(text, size)
//             Called during the update() phase (before the render pass).
//             Checks the cache; if the entry is missing it renders the glyph
//             surface and stashes it in pending_uploads_.
//
//     2.  flushUploads(copy_pass)
//             Called once per frame BEFORE SDL_BeginGPURenderPass, inside a
//             SDL_GPUCopyPass.  Uploads every pending surface to its GPU
//             texture via SDL_UploadToGPUTexture, then releases the surface
//             and the temporary transfer buffer.
//
//   After flushUploads() returns, every texture in the cache is resident on
//   the GPU and safe to bind in the render pass.
//
// Lifetime
// ========
//   TextRenderer must be initialised with a valid SDL_GPUDevice* before any
//   call to ensureTexture().  Call shutdown() (or let the destructor run)
//   to release all GPU textures, the sampler, and the font handle.
//
// Thread safety
// =============
//   Not thread-safe.  All calls must originate from the render thread that
//   owns the SDL_GPUDevice.
//
// Font resolution
// ===============
//   tryLoadFont() probes a list of platform font paths in order and returns
//   the first one that TTF_OpenFont() accepts.  On macOS the system Helvetica
//   Neue is tried first; a bundled fallback (assets/fonts/) is tried last.
//   If no font can be opened, text rendering is silently skipped (drawText
//   no-ops gracefully).

#pragma once

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

// SDL3_ttf — conditional compilation guard so the project still builds
// on systems where SDL3_ttf was not found by CMake.
#ifdef SDL_TTF_AVAILABLE
#  include <SDL3_ttf/SDL_ttf.h>
#endif

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pce::sdlos {

// ---------------------------------------------------------------------------
// GlyphTexture — one cached entry.
//
// Stores the GPU texture and the pixel dimensions of the rendered string so
// callers can lay out the quad without a second query.
// ---------------------------------------------------------------------------

struct GlyphTexture {
    SDL_GPUTexture* texture = nullptr;
    int             width   = 0;
    int             height  = 0;

    [[nodiscard]] bool valid() const noexcept { return texture != nullptr; }
};

// ---------------------------------------------------------------------------
// TextRenderer
// ---------------------------------------------------------------------------

class TextRenderer {
public:
    TextRenderer()  = default;
    ~TextRenderer() { shutdown(); }

    // Non-copyable: owns GPU resources.
    TextRenderer(const TextRenderer&)            = delete;
    TextRenderer& operator=(const TextRenderer&) = delete;

    // ---- Lifecycle -------------------------------------------------------

    /// Initialise with an already-created GPU device.
    /// `default_size` is the point size used when ensureTexture() is called
    /// without an explicit size argument.
    /// Returns true on success; false if TTF init or font loading fails
    /// (text rendering is disabled but the renderer remains in a valid state
    /// that no-ops all subsequent calls).
    bool init(SDL_GPUDevice* device, float default_size = 17.f);

    /// Release all GPU textures, the sampler, and the TTF font.
    /// Safe to call multiple times.
    void shutdown();

    [[nodiscard]] bool isReady() const noexcept { return ready_; }

    // ---- Font loading ----------------------------------------------------

    /// Try to open a font from `path` at `pt_size`.
    /// Replaces the current font on success; returns true.
    /// On failure, the previous font (if any) remains active; returns false.
    bool loadFont(const std::string& path, float pt_size);

    /// Probe a prioritised list of platform font paths and load the first
    /// one that succeeds.  On macOS, Helvetica Neue / Helvetica / SF are
    /// tried; on Linux, DejaVu Sans and Liberation Sans.
    /// Returns true if any font was loaded.
    bool tryLoadSystemFont(float pt_size);

    // ---- Cache interface -------------------------------------------------

    /// Return the cached GlyphTexture for `text` at `size` points.
    /// If the entry is absent, renders it CPU-side (TTF_RenderText_Blended)
    /// and queues it for GPU upload in the next flushUploads() call.
    /// Returns an invalid GlyphTexture{} if the renderer is not ready.
    [[nodiscard]] GlyphTexture ensureTexture(std::string_view text,
                                              float            size = 0.f);

    /// Convenience: same as ensureTexture but uses the default font size.
    [[nodiscard]] GlyphTexture ensureTexture(std::string_view text)
    {
        return ensureTexture(text, 0.f);
    }

    /// Upload all textures that were queued by ensureTexture() since the
    /// last flushUploads() call.
    ///
    /// MUST be called inside a SDL_GPUCopyPass and BEFORE
    /// SDL_BeginGPURenderPass so that the uploaded textures are resident
    /// when the render pass begins.
    ///
    ///   SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
    ///   text_renderer.flushUploads(cp);
    ///   SDL_EndGPUCopyPass(cp);
    ///   // … SDL_BeginGPURenderPass …
    void flushUploads(SDL_GPUCopyPass* copy_pass);

    /// Returns true if there is at least one pending upload waiting to be
    /// flushed.  Useful to skip creating a copy pass when nothing changed.
    [[nodiscard]] bool hasPendingUploads() const noexcept
    {
        return !pending_uploads_.empty();
    }

    // ---- Sampler ---------------------------------------------------------

    /// The linear sampler to bind when drawing text quads.
    /// Non-null only after a successful init().
    [[nodiscard]] SDL_GPUSampler* sampler() const noexcept { return sampler_; }

    // ---- Cache management ------------------------------------------------

    /// Evict all cached textures, releasing their GPU memory.
    /// The renderer remains ready; textures will be re-created on the next
    /// ensureTexture() call.
    void clearCache();

    /// Return the number of currently cached textures.
    [[nodiscard]] std::size_t cacheSize() const noexcept { return cache_.size(); }

private:
    // ---- Cache key -------------------------------------------------------

    struct Key {
        std::string text;
        float       size;   // point size (rounded to 0.5 increments)

        bool operator==(const Key& o) const noexcept
        {
            return size == o.size && text == o.text;
        }
    };

    struct KeyHash {
        std::size_t operator()(const Key& k) const noexcept
        {
            // FNV-1a over text bytes, xor'd with a size hash.
            std::size_t h = 0xcbf29ce484222325ull;
            for (char c : k.text) {
                h ^= static_cast<unsigned char>(c);
                h *= 0x100000001b3ull;
            }
            // Mix in size as a bit pattern to distinguish same-text queries
            // at different sizes.
            uint32_t si;
            __builtin_memcpy(&si, &k.size, sizeof(si));
            h ^= static_cast<std::size_t>(si) * 0x9e3779b97f4a7c15ull;
            return h;
        }
    };

    // ---- Pending upload descriptor ---------------------------------------

    struct PendingUpload {
        Key             key;
        SDL_Surface*    surface  = nullptr;   // CPU pixels (freed after upload)
        SDL_GPUTexture* texture  = nullptr;   // pre-allocated GPU texture
    };

    // ---- Helpers ---------------------------------------------------------

    /// Normalise size: clamp to [6, 256] and round to nearest 0.5pt so
    /// the cache doesn't explode with floating-point near-duplicates.
    [[nodiscard]] static float normaliseSize(float s) noexcept;

    /// Create and return a GPU texture for a surface of the given dimensions.
    /// Returns nullptr on failure.
    [[nodiscard]] SDL_GPUTexture* createTexture(int w, int h);

    /// Create a linear-clamp sampler.  Called once during init().
    [[nodiscard]] SDL_GPUSampler* createSampler();

    // ---- State -----------------------------------------------------------

    SDL_GPUDevice*  device_       = nullptr;
    SDL_GPUSampler* sampler_      = nullptr;
    float           default_size_ = 17.f;
    bool            ready_        = false;

#ifdef SDL_TTF_AVAILABLE
    TTF_Font* font_ = nullptr;

    /// Open (or re-open) font_ from `path` at `pt`.
    /// Closes the previous font first.  Returns true on success.
    bool openFont(const std::string& path, float pt);
#endif

    // Cache: (text, size) → resident GPU texture + dimensions.
    std::unordered_map<Key, GlyphTexture, KeyHash> cache_;

    // Uploads queued by ensureTexture(); drained by flushUploads().
    std::vector<PendingUpload> pending_uploads_;
};

} // namespace pce::sdlos
