#pragma once

// IMount interface + Stat POD for the sdlos virtual file system.
//
// Every scheme registered with Vfs is backed by one IMount implementation.
// The interface is intentionally minimal and binary-safe: all data flows as
// std::span<const std::byte> / std::vector<std::byte> so text and binary
// assets (audio, images, fonts, shader blobs) are handled uniformly.
//
// Error handling uses std::expected<T, std::string> throughout:
//   - A populated expected  → success
//   - std::unexpected(msg) → failure with a human-readable reason
//
// Thread-safety is the responsibility of each concrete implementation.
// LocalMount relies on the OS filesystem.
// MemMount uses its own std::shared_mutex.
// Vfs itself guards the mounts_ map with a std::shared_mutex.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pce::vfs {

// ---------------------------------------------------------------------------
// Stat  —  metadata for a single VFS node.
// ---------------------------------------------------------------------------

struct Stat {
    bool     exists = false;
    bool     is_dir = false;
    uint64_t size   = 0;    ///< byte count; 0 for directories or unknown
    int64_t  mtime  = 0;    ///< seconds since implementation-defined epoch;
                            ///< 0 = unknown.  Use for change-detection only,
                            ///< not as a portable wall-clock timestamp.
};

// ---------------------------------------------------------------------------
// IMount  —  abstract mount point.
// ---------------------------------------------------------------------------

class IMount {
public:
    virtual ~IMount() = default;

    // Non-copyable: implementations own resources (file handles, maps, …).
    IMount(const IMount&)            = delete;
    IMount& operator=(const IMount&) = delete;
    IMount(IMount&&)                 = default;
    IMount& operator=(IMount&&)      = default;

protected:
    IMount() = default;

public:

    // ---- Read ------------------------------------------------------------

    /// Read the entire file at `path` into a byte vector.
    ///
    /// Returns std::unexpected on any error (not found, permission denied, …).
    [[nodiscard]] virtual std::expected<std::vector<std::byte>, std::string>
    read(std::string_view path) noexcept = 0;

    // ---- Write -----------------------------------------------------------

    /// Create or replace the file at `path` with `data`.
    ///
    /// Intermediate directories are created automatically where possible.
    /// Returns std::unexpected if the mount is read-only or the write fails.
    [[nodiscard]] virtual std::expected<void, std::string>
    write(std::string_view path, std::span<const std::byte> data) noexcept = 0;

    // ---- Remove ----------------------------------------------------------

    /// Remove a file or an empty directory.
    ///
    /// Succeeds silently if the path does not exist (idempotent).
    /// Returns std::unexpected if the mount is read-only or removal fails.
    [[nodiscard]] virtual std::expected<void, std::string>
    remove(std::string_view path) noexcept = 0;

    // ---- Stat ------------------------------------------------------------

    /// Query metadata for `path`.
    ///
    /// Always succeeds: if the path does not exist, returns Stat{}.
    /// Implementations must never throw.
    [[nodiscard]] virtual Stat
    stat(std::string_view path) noexcept = 0;

    // ---- List ------------------------------------------------------------

    /// Return the immediate children of the directory at `path`.
    ///
    /// Plain files are returned as bare names ("foo.txt").
    /// Subdirectory entries are returned with a trailing slash ("subdir/")
    /// so callers can distinguish them without a follow-up stat().
    ///
    /// Returns an empty vector if `path` is not a directory or does not exist.
    [[nodiscard]] virtual std::vector<std::string>
    list(std::string_view path) noexcept = 0;

    // ---- Mkdir -----------------------------------------------------------

    /// Create `path` as a directory, including any missing parents.
    ///
    /// Default implementation returns an error — read-only or flat mounts
    /// (e.g. MemMount implicit dirs) do not need to override this.
    [[nodiscard]] virtual std::expected<void, std::string>
    mkdir(std::string_view /*path*/) noexcept
    {
        return std::unexpected(std::string{"mkdir not supported by this mount"});
    }

    // ---- Helpers ---------------------------------------------------------

    /// Convenience: test whether `path` exists (wraps stat()).
    [[nodiscard]] bool exists(std::string_view path) noexcept
    {
        return stat(path).exists;
    }
};

} // namespace pce::vfs
