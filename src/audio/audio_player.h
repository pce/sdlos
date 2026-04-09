#pragma once

// AudioPlayer — single-stream audio player for the sdlos engine.
//
// Owns one SDL3 audio stream at a time.  Suited for longer playback (music,
// ambient loops, previews) rather than fire-and-forget UI bleeps (use
// SfxPlayer for those).
//
// VFS integration
// ───────────────
//   pce::sdlos::AudioPlayer audio;
//   audio.set_vfs(&vfs);
//   audio.play("asset://audio/ambient.wav");   // loads via VFS, starts playback
//   audio.stop();                               // stops + frees the stream
//
// Raw-bytes overload
// ──────────────────
//   auto wav = vfs.read("asset://audio/click.wav");
//   audio.play_bytes(*wav);
//
// Thread-safety
// ─────────────
//   NOT thread-safe — call from a single thread (typically the main/render
//   thread).  SDL audio streams themselves are thread-safe, but the
//   play/stop state machine is not guarded.

#include <SDL3/SDL.h>

#include <cstddef>
#include <string>
#include <vector>

namespace pce::vfs { class Vfs; }

namespace pce::sdlos {


class AudioPlayer {
public:
    AudioPlayer()  = default;
    ~AudioPlayer() { stop(); }

    // Non-copyable: owns an SDL audio stream.
    AudioPlayer(const AudioPlayer&)            = delete;
    AudioPlayer& operator=(const AudioPlayer&) = delete;
    AudioPlayer(AudioPlayer&&)                 = default;
    AudioPlayer& operator=(AudioPlayer&&)      = default;


    /// Attach a VFS instance for URI-based playback.
    /// The Vfs must outlive this AudioPlayer.  Pass nullptr to detach.
    void set_vfs(pce::vfs::Vfs* vfs) noexcept;

    /// Load a WAV from a VFS URI and start playback.
    /// Stops and replaces any currently active stream.
    /// Returns false on failure (VFS read error or SDL decode error).
    bool play(const std::string& uri);

    /// Load a WAV from raw bytes and start playback.
    /// Stops and replaces any currently active stream.
    bool play_bytes(const std::vector<std::byte>& wav_bytes);

    /// Stop playback and release the audio stream.
    void stop();

    /// True while a stream is open and has not been stopped.
    [[nodiscard]] bool is_playing() const noexcept { return playing_; }

private:
    SDL_AudioStream* stream_  = nullptr;
    bool             playing_ = false;
    pce::vfs::Vfs*   vfs_     = nullptr;
};

} // namespace pce::sdlos

