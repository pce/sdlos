#pragma once

// i_file_system.hh — legacy IFileSystem interface (string-based, text-only).
//
// This interface predates the vfs:: subsystem and is kept for backward
// compatibility with existing callers (OS::filesystem(), desktop shell, etc.).
//
// For new code prefer pce::vfs::Vfs directly — it provides:
//   - URI scheme routing  ("scene://", "asset://", "tmp://", "mem://", …)
//   - Binary-safe byte IO (std::span<std::byte> / std::vector<std::byte>)
//   - std::expected<T,E> error propagation (no silent empty-string returns)
//   - Stat, list with trailing-'/' directory markers, mkdir
//
// See src/vfs/vfs.hh.

#include <string>
#include <string_view>
#include <vector>

namespace pce::sdlos {

class IFileSystem {
public:
    virtual ~IFileSystem() = default;

    // Non-copyable: implementations own state (mutex, root path, …).
    IFileSystem(const IFileSystem&)            = delete;
    IFileSystem& operator=(const IFileSystem&) = delete;
    IFileSystem(IFileSystem&&)                 = default;
    IFileSystem& operator=(IFileSystem&&)      = default;

protected:
    IFileSystem() = default;

public:

    // ---- Write -----------------------------------------------------------

    /// Create or replace the file at `path` with `content`.
    ///
    /// Returns true on success, false on any error (bad path, I/O failure).
    /// Parent directories are created automatically.
    [[nodiscard]] virtual bool
    createFile(std::string_view path, std::string_view content = "") = 0;

    /// Create the directory at `path` (and any missing parents).
    ///
    /// Returns true on success or if the directory already exists.
    [[nodiscard]] virtual bool
    createDirectory(std::string_view path) = 0;

    /// Remove the file or empty directory at `path`.
    ///
    /// Returns true on success, false if removal failed or path not found.
    [[nodiscard]] virtual bool
    deleteFile(std::string_view path) = 0;

    // ---- Read ------------------------------------------------------------

    /// Read and return the entire file at `path` as a string.
    ///
    /// Returns an empty string on any error (not found, permission denied, …).
    /// Prefer vfs::Vfs::read_text() for error-distinguishable reads.
    [[nodiscard]] virtual std::string
    readFile(std::string_view path) = 0;

    // ---- Query -----------------------------------------------------------

    /// Return the immediate children of the directory at `path`.
    ///
    /// Each entry is a bare filename (no path prefix).  An empty vector is
    /// returned if `path` is not a directory or cannot be opened.
    [[nodiscard]] virtual std::vector<std::string>
    listDirectory(std::string_view path) = 0;

    /// Return true if `path` exists (file or directory).
    [[nodiscard]] virtual bool
    exists(std::string_view path) = 0;
};

} // namespace pce::sdlos
