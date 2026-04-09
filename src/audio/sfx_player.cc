#include "sfx_player.h"
#include "vfs/vfs.h"

#include <iostream>
#include <string>

namespace pce::sdlos {
/**
 * @brief Sets vfs
 *
 * @param vfs  pce::vfs::Vfs * value
 *
 * @warning Parameter 'vfs' is a non-const raw pointer — Raw pointer parameter —
 *          ownership is ambiguous; consider std::span (non-owning view),
 *          std::unique_ptr (transfer), or const T* (borrow)
 */
void SfxPlayer::set_vfs(pce::vfs::Vfs* vfs) noexcept
{
    std::lock_guard lock(mu_);
    vfs_ = vfs;
}
/**
 * @brief Decodes wav
 *
 * @param io     SDL_IOStream * value
 * @param out    Output parameter written by the callee
 * @param label  Display label
 *
 * @return true on success, false on failure
 */
bool SfxPlayer::decode_wav(SDL_IOStream* io, SfxClip& out, const std::string& label)
{
    SDL_AudioSpec spec{};
    Uint8*  buf = nullptr;
    Uint32  len = 0;
    if (!SDL_LoadWAV_IO(io, true, &spec, &buf, &len)) {
        std::clog << "[sfx] WAV decode failed: " << label
                  << " -- " << SDL_GetError() << "\n";
        return false;
    }
    out.spec = spec;
    out.pcm.assign(buf, buf + len);
    SDL_free(buf);
    return true;
}
/**
 * @brief Loads
 *
 * @param name         Human-readable name or identifier string
 * @param path_or_uri  Filesystem path
 *
 * @return true on success, false on failure
 */
bool SfxPlayer::load(const std::string& name, const std::string& path_or_uri)
{
    SDL_InitSubSystem(SDL_INIT_AUDIO);

    pce::vfs::Vfs* vfs = nullptr;
    {
        std::lock_guard lock(mu_);
        vfs = vfs_;
    }

    if (vfs) {
        auto bytes = vfs->read(path_or_uri);
        if (bytes) {
            return load_bytes(name, *bytes);
        }
        std::clog << "[sfx] VFS read failed for '" << path_or_uri
                  << "', trying direct file...\n";
    }

    SDL_IOStream* io = SDL_IOFromFile(path_or_uri.c_str(), "rb");
    if (!io) {
        std::clog << "[sfx] cannot open: " << path_or_uri
                  << " -- " << SDL_GetError() << "\n";
        return false;
    }

    SfxClip clip;
    if (!decode_wav(io, clip, path_or_uri))
        return false;

    std::size_t pcm_size = clip.pcm.size();
    {
        std::lock_guard lock(mu_);
        clips_[name] = std::move(clip);
    }

    std::clog << "[sfx] loaded '" << name << "' <- " << path_or_uri
              << "  (" << pcm_size << " bytes)\n";
    return true;
}

bool SfxPlayer::load_bytes(const std::string& name,
                           const std::vector<std::byte>& wav_bytes)
{
    SDL_InitSubSystem(SDL_INIT_AUDIO);

    SDL_IOStream* io = SDL_IOFromConstMem(
        static_cast<const void*>(wav_bytes.data()),
        wav_bytes.size());
    if (!io) {
        std::clog << "[sfx] IOFromConstMem failed: " << SDL_GetError() << "\n";
        return false;
    }

    SfxClip clip;
    if (!decode_wav(io, clip, name))
        return false;

    std::size_t pcm_size = clip.pcm.size();
    {
        std::lock_guard lock(mu_);
        clips_[name] = std::move(clip);
    }

    std::clog << "[sfx] loaded '" << name << "' from bytes  ("
              << wav_bytes.size() << " raw, " << pcm_size << " PCM)\n";
    return true;
}

void SfxPlayer::play(const std::string& name)
{
    std::lock_guard lock(mu_);
    auto it = clips_.find(name);
    if (it == clips_.end()) return;

    const SfxClip& clip = it->second;
    SDL_AudioStream* stream = SDL_OpenAudioDeviceStream(
        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &clip.spec, nullptr, nullptr);
    if (!stream) return;

    SDL_PutAudioStreamData(stream,
        static_cast<const void*>(clip.pcm.data()),
        static_cast<int>(clip.pcm.size()));
    SDL_FlushAudioStream(stream);
    SDL_ResumeAudioStreamDevice(stream);
}

bool SfxPlayer::has(const std::string& name) const
{
    std::lock_guard lock(mu_);
    return clips_.contains(name);
}

void SfxPlayer::unload(const std::string& name)
{
    std::lock_guard lock(mu_);
    clips_.erase(name);
}

void SfxPlayer::clear()
{
    std::lock_guard lock(mu_);
    clips_.clear();
}

std::size_t SfxPlayer::size() const
{
    std::lock_guard lock(mu_);
    return clips_.size();
}

} // namespace pce::sdlos
