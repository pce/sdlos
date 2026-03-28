#include "video_texture.hh"

#include <cstdint>
#include <cstring>
#include <iostream>

namespace pce::sdlos {

bool VideoTexture::init(SDL_GPUDevice* device) noexcept
{
    device_ = device;
    if (!device_) return false;

    // Create the shared sampler (bilinear, clamp-to-edge).
    SDL_GPUSamplerCreateInfo sci{};
    sci.min_filter        = SDL_GPU_FILTER_LINEAR;
    sci.mag_filter        = SDL_GPU_FILTER_LINEAR;
    sci.mipmap_mode       = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    sci.address_mode_u    = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sci.address_mode_v    = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sci.address_mode_w    = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_ = SDL_CreateGPUSampler(device_, &sci);
    if (!sampler_) {
        std::cerr << "[VideoTexture] sampler creation failed: " << SDL_GetError() << "\n";
        return false;
    }
    return true;
}

void VideoTexture::shutdown() noexcept
{
    close();
    if (device_) {
        if (tex_)      { SDL_ReleaseGPUTexture(device_, tex_);               tex_      = nullptr; }
        if (sampler_)  { SDL_ReleaseGPUSampler(device_, sampler_);           sampler_  = nullptr; }
        if (transfer_) { SDL_ReleaseGPUTransferBuffer(device_, transfer_);   transfer_ = nullptr; }
    }
    device_       = nullptr;
    w_            = 0;
    h_            = 0;
    transfer_cap_ = 0;
    has_pending_  = false;
}

std::vector<VideoTexture::DeviceInfo> VideoTexture::enumerate() const noexcept
{
    std::vector<DeviceInfo> result;
    int count = 0;
    SDL_CameraID* ids = SDL_GetCameras(&count);
    if (!ids) return result;
    result.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        DeviceInfo info;
        info.id   = ids[i];
        const char* name = SDL_GetCameraName(ids[i]);
        info.name = name ? name : ("Camera " + std::to_string(i));
        result.push_back(std::move(info));
    }
    SDL_free(ids);
    return result;
}

bool VideoTexture::open(SDL_CameraID id) noexcept
{
    close();

    // Preferred spec: RGBA32, 1280x720, 30fps.
    // SDL will match the closest supported spec; AcquireCameraFrame still
    // returns the actual format so we always convert to RGBA32 ourselves.
    SDL_CameraSpec spec{};
    spec.format                 = SDL_PIXELFORMAT_RGBA32;
    spec.colorspace             = SDL_COLORSPACE_SRGB;
    spec.width                  = 1280;
    spec.height                 = 720;
    spec.framerate_numerator    = 30;
    spec.framerate_denominator  = 1;

    cam_ = SDL_OpenCamera(id, &spec);
    if (!cam_) {
        std::cerr << "[VideoTexture] SDL_OpenCamera failed: " << SDL_GetError() << "\n";
        return false;
    }
    std::cerr << "[VideoTexture] camera opened (awaiting permission)\n";
    return true;
}

bool VideoTexture::openByIndex(int index) noexcept
{
    int count = 0;
    SDL_CameraID* ids = SDL_GetCameras(&count);
    if (!ids || count == 0) {
        std::cerr << "[VideoTexture] no cameras available\n";
        if (ids) SDL_free(ids);
        return false;
    }
    if (index < 0 || index >= count) index = 0;
    const SDL_CameraID id = ids[index];
    SDL_free(ids);
    return open(id);
}

void VideoTexture::close() noexcept
{
    if (cam_) {
        SDL_CloseCamera(cam_);
        cam_ = nullptr;
    }
    has_pending_ = false;
}

int VideoTexture::permissionState() const noexcept
{
    if (!cam_) return -1;
    return SDL_GetCameraPermissionState(cam_);
}

bool VideoTexture::ensureTexture(int w, int h) noexcept
{
    if (!device_ || w <= 0 || h <= 0) return false;
    if (tex_ && w_ == w && h_ == h) return true;

    if (tex_) {
        SDL_ReleaseGPUTexture(device_, tex_);
        tex_ = nullptr;
    }

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

    tex_ = SDL_CreateGPUTexture(device_, &tci);
    if (!tex_) {
        std::cerr << "[VideoTexture] texture alloc failed (" << w << "x" << h
                  << "): " << SDL_GetError() << "\n";
        w_ = h_ = 0;
        return false;
    }
    w_ = w;
    h_ = h;
    return true;
}

bool VideoTexture::ensureTransferBuffer(int w, int h) noexcept
{
    if (!device_) return false;
    const int needed = w * h * 4;
    if (transfer_ && transfer_cap_ >= needed) return true;

    if (transfer_) {
        SDL_ReleaseGPUTransferBuffer(device_, transfer_);
        transfer_     = nullptr;
        transfer_cap_ = 0;
    }

    SDL_GPUTransferBufferCreateInfo tbi{};
    tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tbi.size  = static_cast<Uint32>(needed);
    tbi.props = 0;

    transfer_ = SDL_CreateGPUTransferBuffer(device_, &tbi);
    if (!transfer_) {
        std::cerr << "[VideoTexture] transfer buffer alloc failed: " << SDL_GetError() << "\n";
        return false;
    }
    transfer_cap_ = needed;
    return true;
}

void VideoTexture::updateFrame() noexcept
{
    if (!cam_ || !device_) return;
    if (permissionState() != 1) return;  // still waiting for permission

    Uint64 ts = 0;
    SDL_Surface* frame = SDL_AcquireCameraFrame(cam_, &ts);
    if (!frame) return;  // no new frame yet

    // Ensure GPU resources match frame dimensions.
    const int fw = frame->w;
    const int fh = frame->h;
    if (!ensureTexture(fw, fh) || !ensureTransferBuffer(fw, fh)) {
        SDL_ReleaseCameraFrame(cam_, frame);
        return;
    }

    // Convert to RGBA32 if the camera delivers a different format.
    SDL_Surface* rgba = frame;
    bool converted = false;
    if (frame->format != SDL_PIXELFORMAT_RGBA32) {
        rgba = SDL_ConvertSurface(frame, SDL_PIXELFORMAT_RGBA32);
        converted = (rgba != nullptr);
        if (!converted) rgba = frame;  // fallback: upload whatever we have
    }

    // Map the transfer buffer and copy pixel rows (handles non-power-of-2 pitch).
    void* map = SDL_MapGPUTransferBuffer(device_, transfer_, false);
    if (map) {
        const int   stride = fw * 4;
        const auto* src    = static_cast<const Uint8*>(rgba->pixels);
        auto*       dst    = static_cast<Uint8*>(map);
        for (int row = 0; row < fh; ++row)
            SDL_memcpy(dst + row * stride,
                       src + row * rgba->pitch,
                       static_cast<std::size_t>(stride));
        SDL_UnmapGPUTransferBuffer(device_, transfer_);
        has_pending_ = true;
    }

    if (converted) SDL_DestroySurface(rgba);
    SDL_ReleaseCameraFrame(cam_, frame);
}

void VideoTexture::flushUpload(SDL_GPUCopyPass* copy_pass) noexcept
{
    if (!has_pending_ || !tex_ || !transfer_ || !copy_pass) return;

    SDL_GPUTextureTransferInfo src_info{};
    src_info.transfer_buffer = transfer_;
    src_info.offset          = 0;
    src_info.pixels_per_row  = static_cast<Uint32>(w_);
    src_info.rows_per_layer  = static_cast<Uint32>(h_);

    SDL_GPUTextureRegion dst_region{};
    dst_region.texture   = tex_;
    dst_region.mip_level = 0;
    dst_region.layer     = 0;
    dst_region.x         = 0;
    dst_region.y         = 0;
    dst_region.z         = 0;
    dst_region.w         = static_cast<Uint32>(w_);
    dst_region.h         = static_cast<Uint32>(h_);
    dst_region.d         = 1;

    // cycle=true: don't stall on previous reads — the texture is write-only
    // each frame from the perspective of the copy pass.
    SDL_UploadToGPUTexture(copy_pass, &src_info, &dst_region, true);
    has_pending_ = false;
}

} // namespace pce::sdlos
