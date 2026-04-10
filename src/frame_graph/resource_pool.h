#pragma once
//
// ResourcePool — transient GPU texture lifetime management for the FrameGraph.
//
// Every named resource declared in pipeline.pug (e.g. resource#lit) maps to
// one SDL_GPUTexture* owned by this pool.  Swapchain-sized textures are
// automatically recreated on resize().  Fixed-size textures are created once.
//
// Ownership:  ResourcePool owns all textures and releases them in release_all()
//             and in the destructor.  FrameGraph::compile() hands out raw
//             pointers into the pool; those pointers remain valid until the
//             next resize() or release_all() call.
//
// Thread safety:  single-threaded (called from the render thread only).

#include "pug_parser.h"

#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace pce::sdlos::fg {

class ResourcePool {
  public:
    /**
     * @brief Resource pool
     */
    ResourcePool() = default;

    /// Non-copyable — owns SDL GPU resources.
    ResourcePool(const ResourcePool &)            = delete;
    ResourcePool &operator=(const ResourcePool &) = delete;

    /// Move-constructible so FrameGraph can own one by value.
    ResourcePool(ResourcePool &&o) noexcept
        : device_(o.device_)
        , sw_w_(o.sw_w_)
        , sw_h_(o.sw_h_)
        , descs_(std::move(o.descs_))
        , pool_(std::move(o.pool_)) {
        o.device_ = nullptr;
    }

    ResourcePool &operator=(ResourcePool &&o) noexcept {
        if (this != &o) {
            release_all();
            device_   = o.device_;
            sw_w_     = o.sw_w_;
            sw_h_     = o.sw_h_;
            descs_    = std::move(o.descs_);
            pool_     = std::move(o.pool_);
            o.device_ = nullptr;
        }
        return *this;
    }

    /**
     * @brief ~resource pool
     */
    ~ResourcePool() { release_all(); }

    /// Bind to a GPU device and swapchain dimensions.
    /// Must be called before acquire().
    void init(SDL_GPUDevice *device, uint32_t swapchain_w, uint32_t swapchain_h) noexcept {
        device_ = device;
        sw_w_   = swapchain_w;
        sw_h_   = swapchain_h;
    }

    /// Register all resource descriptors from a parsed pipeline.
    /// Call before the first acquire().  Safe to call multiple times — later
    /// calls add descriptors without invalidating existing textures.
    void register_descriptors(const std::vector<ResourceDesc> &descs) {
        for (auto &d : descs) {
            if (!descs_.contains(d.id))
                descs_.emplace(d.id, d);
        }
    }

    /// Resize — call when the swapchain dimensions change.
    //
    // Swapchain-sized textures are destroyed and will be lazily recreated on
    // the next acquire() call.  Fixed-size textures are unaffected.
    /**
     * @brief Resizes
     *
     * @param new_w  Width in logical pixels
     * @param new_h  Opaque resource handle
     */
    void resize(uint32_t new_w, uint32_t new_h) noexcept {
        if (new_w == sw_w_ && new_h == sw_h_)
            return;
        sw_w_ = new_w;
        sw_h_ = new_h;

        for (auto &[id, entry] : pool_) {
            auto it = descs_.find(id);
            if (it == descs_.end())
                continue;
            if (it->second.size == TexSize::Swapchain) {
                if (entry.texture) {
                    SDL_ReleaseGPUTexture(device_, entry.texture);
                    entry.texture = nullptr;  // will be recreated on next acquire()
                }
            }
        }
    }

    // acquire — return (or create) the SDL_GPUTexture* for a named resource.
    //
    // Returns nullptr if:
    //   - the id is unknown (not registered via register_descriptors)
    //   - the device is null
    //   - SDL_CreateGPUTexture fails
    /**
     * @brief Acquires
     *
     * @param id  Unique object identifier
     *
     * @return Pointer to the result, or nullptr on failure
     */
    SDL_GPUTexture *acquire(std::string_view id) noexcept {
        if (!device_)
            return nullptr;

        if (auto it = pool_.find(std::string(id)); it != pool_.end()) {
            if (it->second.texture)
                return it->second.texture;
        }
        auto dit = descs_.find(std::string(id));
        if (dit == descs_.end())
            return nullptr;

        return create_texture(dit->second);
    }

    /// release_all — destroy every texture and clear the pool.
    // Called on shutdown or before a full pipeline rebuild.
    /**
     * @brief Releases all
     */
    void release_all() noexcept {
        if (!device_)
            return;
        for (auto &[id, entry] : pool_) {
            if (entry.texture) {
                SDL_ReleaseGPUTexture(device_, entry.texture);
                entry.texture = nullptr;
            }
        }
        pool_.clear();
    }

    /**
     * @brief Swaps chain h
     *
     * @return Integer result; negative values indicate an error code
     */
    /**
     * @brief Swaps chain w
     *
     * @return Integer result; negative values indicate an error code
     */
    /**
     * @brief Swaps chain h
     *
     * @return Integer result; negative values indicate an error code
     */
    /**
     * @brief Swaps chain h
     *
     * @return Integer result; negative values indicate an error code
     */
    /**
     * @brief Swaps chain h
     *
     * @return Integer result; negative values indicate an error code
     */
    /**
     * @brief Swaps chain h
     *
     * @return Integer result; negative values indicate an error code
     */
    /**
     * @brief Swaps chain h
     *
     * @return Integer result; negative values indicate an error code
     */
    /**
     * @brief Swaps chain h
     *
     * @return Integer result; negative values indicate an error code
     */
    /**
     * @brief Swaps chain h
     *
     * @return Integer result; negative values indicate an error code
     */
    /**
     * @brief Swaps chain h
     *
     * @return Integer result; negative values indicate an error code
     */
    /**
     * @brief Swaps chain h
     *
     * @return Integer result; negative values indicate an error code
     */
    /**
     * @brief Swaps chain h
     *
     * @return Integer result; negative values indicate an error code
     */
    /**
     * @brief Swaps chain h
     *
     * @return Integer result; negative values indicate an error code
     */
    [[nodiscard]]
    uint32_t swapchain_w() const noexcept {
        return sw_w_;
    }
    [[nodiscard]]
    uint32_t swapchain_h() const noexcept {
        return sw_h_;
    }

    /**
     * @brief Tests for the presence of
     *
     * @param id  Unique object identifier
     *
     * @return true on success, false on failure
     *
     * @warning Factory function 'create_texture' returns a non-const raw pointer — Raw
     *          pointer parameter — ownership is ambiguous; consider std::span (non-
     *          owning view), std::unique_ptr (transfer), or const T* (borrow)
     */
    [[nodiscard]]
    bool has(std::string_view id) const noexcept {
        return pool_.contains(std::string(id));
    }

  private:
    // Internal entry — one per registered resource id.
    struct Entry {
        SDL_GPUTexture *texture = nullptr;
    };

    /// create_texture — allocate a new GPU texture for the given descriptor.
    // Stores it in pool_[] and returns the raw pointer (or nullptr on failure).
    /**
     * @brief Creates and returns texture
     *
     * @param d  const ResourceDesc & value
     *
     * @return Pointer to the result, or nullptr on failure
     *
     * @warning Factory function 'create_texture' returns a non-const raw pointer — Raw
     *          pointer parameter — ownership is ambiguous; consider std::span (non-
     *          owning view), std::unique_ptr (transfer), or const T* (borrow)
     */
    /**
     * @brief Creates and returns texture
     *
     * @param d  const ResourceDesc & value
     *
     * @return Pointer to the result, or nullptr on failure
     *
     * @warning Factory function 'create_texture' returns a non-const raw pointer — Raw
     *          pointer parameter — ownership is ambiguous; consider std::span (non-
     *          owning view), std::unique_ptr (transfer), or const T* (borrow)
     */
    /**
     * @brief Creates and returns texture
     *
     * @param d  const ResourceDesc & value
     *
     * @return Pointer to the result, or nullptr on failure
     *
     * @warning Factory function 'create_texture' returns a non-const raw pointer — Raw
     *          pointer parameter — ownership is ambiguous; consider std::span (non-
     *          owning view), std::unique_ptr (transfer), or const T* (borrow)
     */
    /**
     * @brief Creates and returns texture
     *
     * @param d  const ResourceDesc & value
     *
     * @return Pointer to the result, or nullptr on failure
     *
     * @warning Factory function 'create_texture' returns a non-const raw pointer — Raw
     *          pointer parameter — ownership is ambiguous; consider std::span (non-
     *          owning view), std::unique_ptr (transfer), or const T* (borrow)
     */
    /**
     * @brief Creates and returns texture
     *
     * @param d  const ResourceDesc & value
     *
     * @return Pointer to the result, or nullptr on failure
     *
     * @warning Factory function 'create_texture' returns a non-const raw pointer — Raw
     *          pointer parameter — ownership is ambiguous; consider std::span (non-
     *          owning view), std::unique_ptr (transfer), or const T* (borrow)
     */
    /**
     * @brief Creates and returns texture
     *
     * @param d  const ResourceDesc & value
     *
     * @return Pointer to the result, or nullptr on failure
     *
     * @warning Factory function 'create_texture' returns a non-const raw pointer — Raw
     *          pointer parameter — ownership is ambiguous; consider std::span (non-
     *          owning view), std::unique_ptr (transfer), or const T* (borrow)
     */
    /**
     * @brief Creates and returns texture
     *
     * @param d  const ResourceDesc & value
     *
     * @return Pointer to the result, or nullptr on failure
     *
     * @warning Factory function 'create_texture' returns a non-const raw pointer — Raw
     *          pointer parameter — ownership is ambiguous; consider std::span (non-
     *          owning view), std::unique_ptr (transfer), or const T* (borrow)
     */
    /**
     * @brief Creates and returns texture
     *
     * @param d  const ResourceDesc & value
     *
     * @return Pointer to the result, or nullptr on failure
     *
     * @warning Factory function 'create_texture' returns a non-const raw pointer — Raw
     *          pointer parameter — ownership is ambiguous; consider std::span (non-
     *          owning view), std::unique_ptr (transfer), or const T* (borrow)
     */
    /**
     * @brief Creates and returns texture
     *
     * @param d  const ResourceDesc & value
     *
     * @return Pointer to the result, or nullptr on failure
     *
     * @warning Factory function 'create_texture' returns a non-const raw pointer — Raw
     *          pointer parameter — ownership is ambiguous; consider std::span (non-
     *          owning view), std::unique_ptr (transfer), or const T* (borrow)
     */
    /**
     * @brief Creates and returns texture
     *
     * @param d  const ResourceDesc & value
     *
     * @return Pointer to the result, or nullptr on failure
     *
     * @warning Factory function 'create_texture' returns a non-const raw pointer — Raw
     *          pointer parameter — ownership is ambiguous; consider std::span (non-
     *          owning view), std::unique_ptr (transfer), or const T* (borrow)
     */
    /**
     * @brief Creates and returns texture
     *
     * @param d  const ResourceDesc & value
     *
     * @return Pointer to the result, or nullptr on failure
     *
     * @warning Factory function 'create_texture' returns a non-const raw pointer — Raw
     *          pointer parameter — ownership is ambiguous; consider std::span (non-
     *          owning view), std::unique_ptr (transfer), or const T* (borrow)
     */
    /**
     * @brief Creates and returns texture
     *
     * @param d  const ResourceDesc & value
     *
     * @return Pointer to the result, or nullptr on failure
     *
     * @warning Factory function 'create_texture' returns a non-const raw pointer — Raw
     *          pointer parameter — ownership is ambiguous; consider std::span (non-
     *          owning view), std::unique_ptr (transfer), or const T* (borrow)
     */
    [[nodiscard]]
    SDL_GPUTexture *create_texture(const ResourceDesc &d) noexcept {
        const uint32_t w = (d.size == TexSize::Swapchain) ? sw_w_ : d.w;
        const uint32_t h = (d.size == TexSize::Swapchain) ? sw_h_ : d.h;

        if (w == 0 || h == 0)
            return nullptr;

        const SDL_GPUTextureFormat fmt = to_sdl_format(d.format);

        // Colour vs depth attachment usage flags.
        const SDL_GPUTextureUsageFlags usage =
            (d.format == TexFormat::Depth32F)
                ? SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET
                : (SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);

        SDL_GPUTextureCreateInfo info{};
        info.type                 = SDL_GPU_TEXTURETYPE_2D;
        info.format               = fmt;
        info.usage                = usage;
        info.width                = w;
        info.height               = h;
        info.layer_count_or_depth = 1;
        info.num_levels           = 1;
        info.sample_count         = SDL_GPU_SAMPLECOUNT_1;

        SDL_GPUTexture *tex = SDL_CreateGPUTexture(device_, &info);
        if (!tex)
            return nullptr;

        // Debug label — useful in Metal GPU captures.
        SDL_SetGPUTextureName(device_, tex, d.id.c_str());

        pool_[d.id] = {tex};
        return tex;
    }
    SDL_GPUDevice *device_ = nullptr;
    uint32_t sw_w_         = 0;
    uint32_t sw_h_         = 0;

    /// Descriptors registered from pipeline.pug — immutable after register.
    std::unordered_map<std::string, ResourceDesc> descs_;

    /// Live texture pool — may have null entries for stale swapchain textures.
    std::unordered_map<std::string, Entry> pool_;
};

}  // namespace pce::sdlos::fg
