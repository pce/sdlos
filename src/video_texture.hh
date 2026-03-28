#pragma once

// VideoTexture — wraps SDL3 Camera and exposes the latest frame as a GPU texture.
//
// Per-frame protocol (mirrors ImageCache / TextRenderer):
//   1. updateFrame()        — acquire latest SDL_Camera frame, convert to RGBA,
//                             map into the transfer buffer (CPU side).
//   2. (open SDL_GPUCopyPass)
//   3. flushUpload(cp)      — upload staged frame to the GPU texture.
//   4. (open render pass)   — drawVideo / drawVideoWithShader sample texture().
//
// A single SDL_GPUTexture is allocated lazily on the first frame and
// reallocated whenever the camera delivers a different resolution.  The
// same transfer buffer is reused each frame so there is no per-frame heap
// allocation after the first frame.

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_camera.h>

#include <string>
#include <vector>

namespace pce::sdlos {

class VideoTexture {
public:
    struct DeviceInfo {
        SDL_CameraID id   = 0;
        std::string  name;
    };

    VideoTexture()  = default;
    ~VideoTexture() { shutdown(); }

    VideoTexture(const VideoTexture&)            = delete;
    VideoTexture& operator=(const VideoTexture&) = delete;
    VideoTexture(VideoTexture&&)                 = delete;
    VideoTexture& operator=(VideoTexture&&)      = delete;

    // ---- Lifecycle -------------------------------------------------------

    bool init(SDL_GPUDevice* device) noexcept;
    void shutdown() noexcept;

    [[nodiscard]] bool isReady() const noexcept { return device_ != nullptr; }

    // ---- Camera enumeration ---------------------------------------------

    [[nodiscard]] std::vector<DeviceInfo> enumerate() const noexcept;

    // ---- Open / close ---------------------------------------------------

    /// Opens camera by SDL_CameraID; closes the previously open camera first.
    bool open(SDL_CameraID id) noexcept;

    /// Opens the camera at position `index` in the enumerated device list.
    /// 0 = first available camera.  Returns false if there are no cameras.
    bool openByIndex(int index) noexcept;

    void close() noexcept;

    [[nodiscard]] bool isOpen()         const noexcept { return cam_ != nullptr; }

    /// Returns -1 = denied, 0 = pending approval, 1 = approved.
    [[nodiscard]] int permissionState() const noexcept;

    // ---- Per-frame -------------------------------------------------------

    /// Acquire the latest camera frame and stage it for GPU upload.
    /// Call once per render frame, *before* the SDL_GPUCopyPass.
    /// No-op when the camera is not open, not yet approved, or has no new frame.
    void updateFrame() noexcept;

    /// Upload the staged frame to the GPU texture.
    /// Must be called inside an open SDL_GPUCopyPass, before the render pass.
    /// No-op when hasPendingUpload() is false.
    void flushUpload(SDL_GPUCopyPass* copy_pass) noexcept;

    [[nodiscard]] bool hasPendingUpload() const noexcept { return has_pending_; }

    // ---- GPU resources --------------------------------------------------

    /// The GPU-resident frame texture; null until the first frame is uploaded.
    [[nodiscard]] SDL_GPUTexture* texture() const noexcept { return tex_; }

    /// Bilinear / clamp-to-edge sampler — shared, owned by this object.
    [[nodiscard]] SDL_GPUSampler* sampler() const noexcept { return sampler_; }

    [[nodiscard]] int width()  const noexcept { return w_; }
    [[nodiscard]] int height() const noexcept { return h_; }

private:
    // ---- Internal helpers -----------------------------------------------

    /// (Re-)allocate the GPU texture at w×h RGBA8.
    bool ensureTexture(int w, int h) noexcept;

    /// (Re-)allocate the transfer buffer to hold at least w*h*4 bytes.
    bool ensureTransferBuffer(int w, int h) noexcept;

    // ---- State ----------------------------------------------------------

    SDL_GPUDevice*         device_       = nullptr;
    SDL_Camera*            cam_          = nullptr;
    SDL_GPUTexture*        tex_          = nullptr;
    SDL_GPUSampler*        sampler_      = nullptr;
    SDL_GPUTransferBuffer* transfer_     = nullptr;
    int                    w_            = 0;
    int                    h_            = 0;
    int                    transfer_cap_ = 0;  // allocated bytes in transfer_
    bool                   has_pending_  = false;
};

} // namespace pce::sdlos
