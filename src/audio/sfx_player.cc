#include "sfx_player.h"

#include "vfs/vfs.h"

#include <iostream>
#include <random>
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
void SfxPlayer::set_vfs(pce::vfs::Vfs *vfs) noexcept {
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
bool SfxPlayer::decode_wav(SDL_IOStream *io, SfxClip &out, const std::string &label) {
    SDL_AudioSpec spec{};
    Uint8 *buf = nullptr;
    Uint32 len = 0;
    if (!SDL_LoadWAV_IO(io, true, &spec, &buf, &len)) {
        std::clog << "[sfx] WAV decode failed: " << label << " -- " << SDL_GetError() << "\n";
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
bool SfxPlayer::load(const std::string &name, const std::string &path_or_uri) {
    SDL_InitSubSystem(SDL_INIT_AUDIO);

    pce::vfs::Vfs *vfs = nullptr;
    {
        std::lock_guard lock(mu_);
        vfs = vfs_;
    }

    if (vfs) {
        auto bytes = vfs->read(path_or_uri);
        if (bytes) {
            return load_bytes(name, *bytes);
        }
        std::clog << "[sfx] VFS read failed for '" << path_or_uri << "', trying direct file...\n";
    }

    SDL_IOStream *io = SDL_IOFromFile(path_or_uri.c_str(), "rb");
    if (!io) {
        std::clog << "[sfx] cannot open: " << path_or_uri << " -- " << SDL_GetError() << "\n";
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

    std::clog << "[sfx] loaded '" << name << "' <- " << path_or_uri << "  (" << pcm_size
              << " bytes)\n";
    return true;
}

/**
 * @brief Loads bytes
 *
 * @param name       Human-readable name or identifier string
 * @param wav_bytes  Raw byte span
 *
 * @return true on success, false on failure
 */
bool SfxPlayer::load_bytes(const std::string &name, const std::vector<std::byte> &wav_bytes) {
    SDL_InitSubSystem(SDL_INIT_AUDIO);

    SDL_IOStream *io =
        SDL_IOFromConstMem(static_cast<const void *>(wav_bytes.data()), wav_bytes.size());
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

    std::clog << "[sfx] loaded '" << name << "' from bytes  (" << wav_bytes.size() << " raw, "
              << pcm_size << " PCM)\n";
    return true;
}

/**
 * @brief Loads group
 *
 * @param group_name     Human-readable name or identifier string
 * @param paths_or_uris  Filesystem path
 * @param mode           Operation mode selector
 *
 * @return true on success, false on failure
 */
bool SfxPlayer::load_group(
    const std::string &group_name,
    const std::vector<std::string> &paths_or_uris,
    SfxGroup::Mode mode) {
    if (paths_or_uris.empty())
        return false;

    std::vector<std::string> child_names;
    bool all_ok = true;

    for (std::size_t i = 0; i < paths_or_uris.size(); ++i) {
        const std::string child_name = "__group_" + group_name + "_" + std::to_string(i);
        if (load(child_name, paths_or_uris[i])) {
            child_names.push_back(child_name);
        } else {
            all_ok = false;
        }
    }

    if (child_names.empty())
        return false;

    {
        std::lock_guard lock(mu_);
        groups_[group_name] = SfxGroup{.names = std::move(child_names), .mode = mode};
    }

    const char *mode_str = "sequential";
    switch (mode) {
    case SfxGroup::Mode::Random:
        mode_str = "random";
        break;
    case SfxGroup::Mode::Unisono:
        mode_str = "unisono";
        break;
    case SfxGroup::Mode::Chase:
        mode_str = "chase";
        break;
    case SfxGroup::Mode::Sequential:
        mode_str = "sequential";
        break;
    }

    std::clog << "[sfx] loaded group '" << group_name << "' with " << child_names.size() << " clips"
              << " (" << mode_str << ")\n";

    return all_ok;
}

/**
 * @brief Play
 *
 * @param name  Human-readable name or identifier string
 */
void SfxPlayer::play(const std::string &name) {
    std::lock_guard lock(mu_);

    auto grit = groups_.find(name);
    if (grit != groups_.end()) {
        auto &group = grit->second;
        if (group.names.empty())
            return;

        if (group.mode == SfxGroup::Mode::Unisono) {
            for (const auto &clip_name : group.names) {
                play_single(clip_name);
            }
        } else if (group.mode == SfxGroup::Mode::Chase) {
            // Chase mode: queue all clips in the group sequentially in a single stream.
            // SDL_AudioStream can take multiple source put calls and plays them in order.

            // We need a common spec for the stream if we want to chain them perfectly.
            // For UI sounds, we'll just use the first clip's spec and hope the rest match
            // or let SDL handle the resampling/format conversion per put.
            const SfxClip &first    = clips_[group.names[0]];
            SDL_AudioStream *stream = SDL_OpenAudioDeviceStream(
                SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                &first.spec,
                nullptr,
                nullptr);
            if (!stream)
                return;

            for (const auto &clip_name : group.names) {
                auto it = clips_.find(clip_name);
                if (it == clips_.end())
                    continue;
                const SfxClip &clip = it->second;
                SDL_PutAudioStreamData(stream, clip.pcm.data(), static_cast<int>(clip.pcm.size()));
            }
            SDL_FlushAudioStream(stream);
            SDL_ResumeAudioStreamDevice(stream);
        } else if (group.mode == SfxGroup::Mode::Random) {
            static std::mt19937 gen(std::random_device{}());
            std::uniform_int_distribution<std::size_t> dist(0, group.names.size() - 1);
            play_single(group.names[dist(gen)]);
        } else {
            // Sequential / Round-robin
            play_single(group.names[group.next_index]);
            group.next_index = (group.next_index + 1) % group.names.size();
        }
    } else {
        // Individual clip
        play_single(name);
    }
}

/**
 * @brief Play single
 *
 * @param clip_name  Human-readable name or identifier string
 *
 * @return Pointer to the result, or nullptr on failure
 */
SDL_AudioStream *SfxPlayer::play_single(const std::string &clip_name) {
    auto it = clips_.find(clip_name);
    if (it == clips_.end())
        return nullptr;

    const SfxClip &clip = it->second;
    SDL_AudioStream *stream =
        SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &clip.spec, nullptr, nullptr);
    if (!stream)
        return nullptr;

    SDL_PutAudioStreamData(
        stream,
        static_cast<const void *>(clip.pcm.data()),
        static_cast<int>(clip.pcm.size()));
    SDL_FlushAudioStream(stream);
    SDL_ResumeAudioStreamDevice(stream);
    return stream;
}

/**
 * @brief Tests for the presence of
 *
 * @param name  Human-readable name or identifier string
 *
 * @return true on success, false on failure
 */
bool SfxPlayer::has(const std::string &name) const {
    std::lock_guard lock(mu_);
    return clips_.contains(name);
}

/**
 * @brief Unload
 *
 * @param name  Human-readable name or identifier string
 */
void SfxPlayer::unload(const std::string &name) {
    std::lock_guard lock(mu_);
    clips_.erase(name);
}

/**
 * @brief Clears
 */
void SfxPlayer::clear() {
    std::lock_guard lock(mu_);
    clips_.clear();
    groups_.clear();
}

/**
 * @brief Size
 *
 * @return Integer result; negative values indicate an error code
 */
std::size_t SfxPlayer::size() const {
    std::lock_guard lock(mu_);
    return clips_.size();
}

}  // namespace pce::sdlos
