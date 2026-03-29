#pragma once

// Two-step GPU image upload model — mirrors TextRenderer's protocol exactly:
//
//   1. ensureTexture(path)      — first call: loads the file via SDL_image
//                                 (or SDL_LoadBMP as a no-dependency fallback),
//                                 converts to RGBA32, pre-allocates a GPU
//                                 texture, and queues a PendingUpload.
//                                 Subsequent calls within the same frame return
//                                 the cached entry instantly (O(1) hash lookup).
//
//   2. flushUploads(copy_pass)  — call BEFORE SDL_BeginGPURenderPass, inside a
//                                 SDL_GPUCopyPass; uploads queued surfaces to
//                                 the GPU and releases CPU memory.
//                                 On the very first frame an image is drawn its
//                                 GPU texture contains undefined pixels; from
//                                 the second frame onwards it is correct.
//                                 (Same one-frame latency as TextRenderer.)
//
// Textures are cached for the lifetime of the ImageCache object; call
// evict() to explicitly drop a single entry or clearCache() for all.
//
// Scene-scoped lifetime
// ─────────────────────
// Textures accumulate in GPU memory as scenes are navigated.  Use the scope
// API to tag textures with a logical owner and release them on transition:
//
//   // Before building a new scene:
//   image_cache->evict_scope("scene");   // release previous scene's textures
//   image_cache->set_scope("scene");     // tag textures loaded this scene
//
//   // Permanent shared textures (UI icons, fonts atlas) carry no scope ("").
//   image_cache->set_scope("");          // back to permanent
//
// evict_scope("scene") is safe to call immediately after
// SDLRenderer::SetScene(nullptr) because no draw callbacks referencing those
// textures are executing, and SDL3 GPU defers the actual GPU-side release
// until all in-flight command buffers have completed.
//
// Not thread-safe — all calls must come from the render thread that owns the
// SDL GPU device.

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace pce::sdlos {

// ---------------------------------------------------------------------------
// ImageTexture  —  a GPU-resident image and its pixel dimensions.
//
// Returned by ImageCache::ensureTexture().  The texture pointer is owned by
// the ImageCache; do not release it manually.
// ---------------------------------------------------------------------------

struct ImageTexture {
    SDL_GPUTexture* texture = nullptr;
    int             width   = 0;
    int             height  = 0;

    [[nodiscard]] bool valid() const noexcept { return texture != nullptr; }
};

// ---------------------------------------------------------------------------
// ImageCache
// ---------------------------------------------------------------------------

class ImageCache {
public:
    ImageCache()  = default;
    ~ImageCache() { shutdown(); }

    // Non-copyable / non-movable: owns GPU resources tied to a specific device.
    ImageCache(const ImageCache&)            = delete;
    ImageCache& operator=(const ImageCache&) = delete;
    ImageCache(ImageCache&&)                 = delete;
    ImageCache& operator=(ImageCache&&)      = delete;

    // ---- Lifecycle -------------------------------------------------------

    /// Returns false if the sampler cannot be created (GPU init failed).
    /// On failure all subsequent ensureTexture() calls return invalid textures
    /// gracefully — no null-pointer deref.
    bool init(SDL_GPUDevice* device) noexcept;

    /// Safe to call multiple times; called automatically by the destructor.
    void shutdown() noexcept;

    [[nodiscard]] bool isReady() const noexcept { return device_ != nullptr; }

    // ---- Base path for relative src= paths -------------------------------

    /// Set the directory prepended to relative paths passed to ensureTexture().
    /// A trailing '/' is appended automatically if absent.
    /// Call once after init(), typically with SDL_GetBasePath().
    /// Empty string (default) = paths are used as-is (relative to CWD).
    void setBasePath(std::string path) noexcept;

    // ---- Cache interface -------------------------------------------------

    /// Returns the GPU-resident ImageTexture for `path`, loading it on first
    /// access.  The texture is pre-allocated on the first call but its pixels
    /// are only valid after the next flushUploads() invocation.
    ///
    /// On load failure (missing file, unsupported format) an invalid
    /// ImageTexture{} is returned and a null entry is inserted into the cache
    /// so the failed path is not retried every frame.
    [[nodiscard]] ImageTexture ensureTexture(std::string_view path);

    /// Must be called BEFORE SDL_BeginGPURenderPass, inside an open
    /// SDL_GPUCopyPass.  Uploads all pending CPU surfaces to GPU textures and
    /// releases the CPU memory.  No-op when hasPendingUploads() is false.
    void flushUploads(SDL_GPUCopyPass* copy_pass);

    /// Returns true when at least one ensureTexture() call this frame produced
    /// a new load — caller should open a copy pass before the render pass.
    [[nodiscard]] bool hasPendingUploads() const noexcept
    {
        return !pending_.empty();
    }

    // ---- Cache management ------------------------------------------------

    /// Drop a single cached entry and release its GPU texture.
    /// The path will be reloaded on the next ensureTexture() call.
    void evict(std::string_view path);

    /// Drop all cached entries.  ensureTexture() will reload on next access.
    void clearCache() noexcept;

    [[nodiscard]] std::size_t cacheSize() const noexcept { return cache_.size(); }

    // ---- Scope-based lifetime management ---------------------------------
    //
    // A scope is a string tag associated with textures loaded after
    // set_scope() is called.  Scoped textures can be bulk-released with
    // evict_scope() — typically at the start of a scene transition.
    //
    // The empty scope ("") is the default and means "permanent" — those
    // textures are never touched by evict_scope and only released by
    // evict() or clearCache().

    /// Set the scope tag applied to all subsequently loaded textures.
    /// Pass "" to revert to the permanent (un-scoped) default.
    void set_scope(std::string scope) noexcept;

    /// Return the currently active scope tag.
    [[nodiscard]] const std::string& current_scope() const noexcept
    {
        return current_scope_;
    }

    /// Release all GPU textures whose scope matches `scope`.
    ///
    /// Safe to call while no scene is attached to the renderer
    /// (immediately after SDLRenderer::SetScene(nullptr, k_null_handle)).
    /// SDL3 GPU defers GPU-side release until all in-flight work completes.
    ///
    /// No-op if no textures carry the requested scope.
    void evict_scope(std::string_view scope) noexcept;

    /// Return the number of textures currently tagged with `scope`.
    [[nodiscard]] std::size_t scope_size(std::string_view scope) const noexcept;

    // ---- Sampler ---------------------------------------------------------

    /// Bilinear, clamp-to-edge sampler shared by all images.
    /// Non-null only after a successful init().
    [[nodiscard]] SDL_GPUSampler* sampler() const noexcept { return sampler_; }

private:
    // ---- Internal helpers ------------------------------------------------

    /// Allocate an RGBA8 GPU texture sized w×h with SAMPLER usage.
    [[nodiscard]] SDL_GPUTexture* createTexture(int w, int h) noexcept;

    /// Create the shared linear / clamp sampler.
    [[nodiscard]] SDL_GPUSampler* createSampler() noexcept;

    // ---- Pending-upload descriptor ---------------------------------------

    struct PendingUpload {
        std::string     path;
        SDL_Surface*    surface = nullptr;  // RGBA32 CPU surface; freed post-upload
        SDL_GPUTexture* texture = nullptr;  // pre-allocated GPU texture; owned by cache_
    };

    // ---- State -----------------------------------------------------------

    SDL_GPUDevice*  device_  = nullptr;
    SDL_GPUSampler* sampler_ = nullptr;

    // path → resident GPU texture (value may be invalid{} for failed loads)
    std::unordered_map<std::string, ImageTexture> cache_;

    // Filled by ensureTexture(); drained by flushUploads().
    std::vector<PendingUpload> pending_;

    // Prepended to relative paths in ensureTexture() — set via setBasePath().
    std::string base_path_;

    // ---- Scope tracking --------------------------------------------------
    //
    // current_scope_  — applied to every new cache entry created by
    //                   ensureTexture().  Empty = permanent (never evicted
    //                   by evict_scope).
    //
    // entry_scopes_   — parallel map: path → scope.  Kept in sync with
    //                   cache_ so evict_scope() can find all candidates in
    //                   O(entries-in-scope) without scanning the full cache.
    //                   Only entries with a non-empty scope are stored here;
    //                   permanent entries have no entry_scopes_ record.

    std::string current_scope_;
    std::unordered_map<std::string, std::string> entry_scopes_;  // path → scope
};

} // namespace pce::sdlos
