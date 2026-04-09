#pragma once

// SfxPlayer — fire-and-forget UI sound effects for the sdlos engine.
//
// Pre-loads WAV files into memory (from VFS URIs or raw bytes); play() opens
// a short-lived SDL3 audio stream per invocation so multiple overlapping
// sounds work without a full mixing library.
//
// VFS integration
//   Attach a pce::vfs::Vfs instance and load clips by URI:
//
//     pce::sdlos::SfxPlayer sfx;
//     sfx.set_vfs(&vfs);                                   // optional
//     sfx.load("click",  "asset://sounds/nf-shot-01.wav"); // via VFS
//     sfx.load("select", "asset://sounds/neueis_13.wav");
//     sfx.play("click");                                   // fire-and-forget
//
//   Without VFS the URI is treated as a local file path (SDL_IOFromFile).
//
// Raw-bytes overload
//   sfx.load_bytes("beep", wav_data);   // std::vector<std::byte>
//
// Thread-safety
//   load() / play() are safe to call from any thread.  SDL3 audio streams
//   are inherently thread-safe.  The clips_ map is guarded by a mutex.
//
// NOTE: Each play() call opens its own audio device stream so multiple
//       overlapping sounds work.  For heavy mixing consider SDL_mixer.

#include <SDL3/SDL.h>

#include <cstddef>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace pce::vfs { class Vfs; }

namespace pce::sdlos {


/// Decoded PCM clip stored in memory.
struct SfxClip {
    SDL_AudioSpec          spec{};
    std::vector<Uint8>     pcm;      ///< raw PCM data

    [[nodiscard]] bool valid() const noexcept { return !pcm.empty(); }
};


class SfxPlayer {
public:
    SfxPlayer()  = default;
    ~SfxPlayer() = default;

    // Non-copyable, non-movable: owns a std::mutex.
    SfxPlayer(const SfxPlayer&)            = delete;
    SfxPlayer& operator=(const SfxPlayer&) = delete;
    SfxPlayer(SfxPlayer&&)                 = delete;
    SfxPlayer& operator=(SfxPlayer&&)      = delete;


    /// Attach a VFS instance.  When set, load() treats `path_or_uri` as a VFS
    /// URI (e.g. "asset://sounds/click.wav").  Without VFS, the string is
    /// passed directly to SDL_IOFromFile.
    ///
    /// The Vfs must outlive the SfxPlayer.  Pass nullptr to detach.
    void set_vfs(pce::vfs::Vfs* vfs) noexcept;

    /// Load a WAV from a file path or VFS URI and store it under `name`.
    ///
    /// Returns true on success.  On failure the clip is not registered and
    /// a diagnostic is logged.
    bool load(const std::string& name, const std::string& path_or_uri);

    /// Load a WAV from raw bytes (e.g. already fetched from VFS / network).
    bool load_bytes(const std::string& name, const std::vector<std::byte>& wav_bytes);

    /// Play a previously loaded clip.  Fire-and-forget: the stream drains
    /// and SDL3 reclaims it automatically.
    void play(const std::string& name);

    /// Return true if a clip named `name` has been loaded.
    [[nodiscard]] bool has(const std::string& name) const;

    /// Remove a previously loaded clip.
    void unload(const std::string& name);

    /// Remove all loaded clips.
    void clear();

    /// Number of loaded clips.
    [[nodiscard]] std::size_t size() const;

private:
    /// Decode a WAV from an SDL_IOStream into a SfxClip.
    static bool decode_wav(SDL_IOStream* io, SfxClip& out, const std::string& label);

    mutable std::mutex                             mu_;
    std::unordered_map<std::string, SfxClip>       clips_;
    pce::vfs::Vfs*                                 vfs_ = nullptr;
};

} // namespace pce::sdlos

