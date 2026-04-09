#include "audio_player.h"
#include "vfs/vfs.h"

#include <iostream>
#include <string>

namespace pce::sdlos {

void AudioPlayer::set_vfs(pce::vfs::Vfs* vfs) noexcept
{
    vfs_ = vfs;
}

bool AudioPlayer::play_bytes(const std::vector<std::byte>& wav_bytes)
{
    stop();

    SDL_InitSubSystem(SDL_INIT_AUDIO);

    SDL_IOStream* io = SDL_IOFromConstMem(
        static_cast<const void*>(wav_bytes.data()),
        wav_bytes.size());
    if (!io) {
        std::clog << "[audio] IOFromConstMem -- " << SDL_GetError() << "\n";
        return false;
    }

    SDL_AudioSpec spec{};
    Uint8*  buf = nullptr;
    Uint32  len = 0;
    if (!SDL_LoadWAV_IO(io, true, &spec, &buf, &len)) {
        std::clog << "[audio] LoadWAV_IO -- " << SDL_GetError() << "\n";
        return false;
    }

    stream_ = SDL_OpenAudioDeviceStream(
        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!stream_) {
        SDL_free(buf);
        std::clog << "[audio] OpenAudioDeviceStream -- " << SDL_GetError() << "\n";
        return false;
    }

    SDL_PutAudioStreamData(stream_,
        static_cast<const void*>(buf), static_cast<int>(len));
    SDL_free(buf);
    SDL_ResumeAudioStreamDevice(stream_);
    playing_ = true;
    return true;
}

bool AudioPlayer::play(const std::string& uri)
{
    if (!vfs_) {
        std::clog << "[audio] no VFS attached -- cannot resolve: " << uri << "\n";
        return false;
    }

    auto bytes = vfs_->read(uri);
    if (!bytes) {
        std::clog << "[audio] VFS read failed: " << bytes.error() << "\n";
        return false;
    }

    return play_bytes(*bytes);
}

void AudioPlayer::stop()
{
    if (stream_) {
        SDL_PauseAudioStreamDevice(stream_);
        SDL_DestroyAudioStream(stream_);
        stream_ = nullptr;
    }
    playing_ = false;
}

} // namespace pce::sdlos
