#pragma once

// Two-step GPU upload model:
//   1. ensureTexture(text, size)  — during update(); renders glyph CPU-side,
//      pre-allocates GPU texture, queues to pending_uploads_.
//   2. flushUploads(copy_pass)    — must be called BEFORE SDL_BeginGPURenderPass,
//      inside a SDL_GPUCopyPass; uploads surfaces to GPU, releases CPU memory.
//
// Not thread-safe — all calls must come from the render thread that owns the device.

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

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

// ---- GlyphTexture ----

struct GlyphTexture {
    SDL_GPUTexture* texture = nullptr;
    int             width   = 0;
    int             height  = 0;

    [[nodiscard]] bool valid() const noexcept { return texture != nullptr; }
};

// ---- TextRenderer ----

class TextRenderer {
public:
    TextRenderer()  = default;
    ~TextRenderer() { shutdown(); }

    // Non-copyable: owns GPU resources.
    TextRenderer(const TextRenderer&)            = delete;
    TextRenderer& operator=(const TextRenderer&) = delete;

    // ---- Lifecycle -------------------------------------------------------

    /// `default_size` is the point size used when ensureTexture() is called
    /// without an explicit size argument.
    /// Returns false if TTF init fails; text rendering is disabled but the
    /// renderer no-ops all subsequent calls gracefully.
    bool init(SDL_GPUDevice* device, float default_size = 17.f);

    /// Safe to call multiple times.
    void shutdown();

    [[nodiscard]] bool isReady() const noexcept { return ready_; }

    // ---- Font loading ----------------------------------------------------

    /// On failure, the previous font (if any) remains active.
    bool loadFont(const std::string& path, float pt_size);

    /// Try each path in order; load the first that succeeds.
    bool loadFirstAvailable(std::initializer_list<std::string_view> candidates,
                            float pt_size);

    /// Explicit opt-in — never called automatically by init().
    /// Call after loadFirstAvailable() fails if a system fallback is desired.
    bool tryLoadSystemFont(float pt_size);

    // ---- Cache interface -------------------------------------------------

    /// Returns an invalid GlyphTexture{} if the renderer is not ready.
    [[nodiscard]] GlyphTexture ensureTexture(std::string_view text,
                                              float            size = 0.f);

    [[nodiscard]] GlyphTexture ensureTexture(std::string_view text)
    {
        return ensureTexture(text, 0.f);
    }

    /// Must be called BEFORE SDL_BeginGPURenderPass, inside a SDL_GPUCopyPass.
    void flushUploads(SDL_GPUCopyPass* copy_pass);

    /// Useful to skip creating a copy pass when nothing changed.
    [[nodiscard]] bool hasPendingUploads() const noexcept
    {
        return !pending_uploads_.empty();
    }

    // ---- Sampler ---------------------------------------------------------

    /// Non-null only after a successful init().
    [[nodiscard]] SDL_GPUSampler* sampler() const noexcept { return sampler_; }

    // ---- Cache management ------------------------------------------------

    /// The renderer remains ready; textures will be re-created on the next
    /// ensureTexture() call.
    void clearCache();

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

    /// Clamp to [6, 256] and round to nearest 0.5pt to prevent near-duplicate
    /// cache entries from floating-point variation.
    [[nodiscard]] static float normaliseSize(float s) noexcept;

    [[nodiscard]] SDL_GPUTexture* createTexture(int w, int h);

    /// Called once during init().
    [[nodiscard]] SDL_GPUSampler* createSampler();

    // ---- State -----------------------------------------------------------

    SDL_GPUDevice*  device_       = nullptr;
    SDL_GPUSampler* sampler_      = nullptr;
    float           default_size_ = 17.f;
    bool            ready_        = false;
    bool            ttf_inited_   = false;  // true after TTF_Init(), gates TTF_Quit()

#ifdef SDL_TTF_AVAILABLE
    TTF_Font* font_ = nullptr;

    /// Closes the previous font before opening the new one.
    bool openFont(const std::string& path, float pt);
#endif

    // (text, size) → resident GPU texture + dimensions.
    std::unordered_map<Key, GlyphTexture, KeyHash> cache_;

    // Queued by ensureTexture(); drained by flushUploads().
    std::vector<PendingUpload> pending_uploads_;
};

} // namespace pce::sdlos
