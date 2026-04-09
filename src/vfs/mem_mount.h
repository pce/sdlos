#pragma once

// Directory model
//
// Directories are implicit — they are synthesised from the common path
// prefixes of stored entries.  There is no need to call mkdir() before
// writing a nested path:
//
//   mem.put_text("audio/sfx/click.wav", ...);
//   mem.list("audio")        →  { "sfx/" }
//   mem.list("audio/sfx")    →  { "click.wav" }
//   mem.stat("audio")        →  Stat{ exists=true, is_dir=true }
//
// Thread-safety
//
// A std::shared_mutex allows any number of concurrent reads.
// Writes (put, write, remove) take an exclusive lock.
// All public methods are noexcept.

#include "mount.h"

#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>

namespace pce::vfs {

class MemMount final : public IMount {
public:

    /**
     * @brief Mem mount
     */
    MemMount()  = default;
    /**
     * @brief ~mem mount
     */
    ~MemMount() override = default;

    // Non-copyable (owns a mutex and a heap map).
    /**
     * @brief Mem mount
     *
     * @param param0  Red channel component [0, 1]
     */
    MemMount(const MemMount&)            = delete;
    MemMount& operator=(const MemMount&) = delete;

    // Non-movable: owns a std::shared_mutex.  Store via unique_ptr if
    // indirection is needed.
    /**
     * @brief Mem mount
     *
     * @param param0  Red channel component [0, 1]
     */
    MemMount(MemMount&&)            = delete;
    MemMount& operator=(MemMount&&) = delete;

    //  Seeding
    //
    // These helpers are typically called once at startup to pre-populate the
    // mount with known-good data (embedded assets, default config, etc.).
    // They are safe to call concurrently with reads but NOT with other writes.

    /// Store raw bytes at `path` (create-or-replace).
    void put(std::string path, std::vector<std::byte> data);

    /// Store a UTF-8 string at `path` (create-or-replace).
    void put_text(std::string path, std::string_view text);

    /// Return the number of stored entries.
    [[nodiscard]] std::size_t size() const noexcept;

    /// Remove all entries.
    void clear() noexcept;

    //  IMount

    [[nodiscard]] std::expected<std::vector<std::byte>, std::string>
    read(std::string_view path) noexcept override;

    /**
     * @brief Writes
     *
     * @param path  Filesystem path
     * @param data  Raw payload bytes
     *
     * @return Integer result; negative values indicate an error code
     */
    /**
     * @brief Writes
     *
     * @param path  Filesystem path
     * @param data  Raw payload bytes
     *
     * @return Integer result; negative values indicate an error code
     */
    /**
     * @brief Writes
     *
     * @param path  Filesystem path
     * @param data  Raw payload bytes
     *
     * @return Integer result; negative values indicate an error code
     */
    /**
     * @brief Writes
     *
     * @param path  Filesystem path
     * @param data  Raw payload bytes
     *
     * @return Integer result; negative values indicate an error code
     */
    /**
     * @brief Writes
     *
     * @param path  Filesystem path
     * @param data  Raw payload bytes
     *
     * @return Integer result; negative values indicate an error code
     */
    /**
     * @brief Writes
     *
     * @param path  Filesystem path
     * @param data  Raw payload bytes
     *
     * @return Integer result; negative values indicate an error code
     */
    /**
     * @brief Writes
     *
     * @param path  Filesystem path
     * @param data  Raw payload bytes
     *
     * @return Integer result; negative values indicate an error code
     */
    /**
     * @brief Writes
     *
     * @param path  Filesystem path
     * @param data  Raw payload bytes
     *
     * @return Integer result; negative values indicate an error code
     */
    /**
     * @brief Writes
     *
     * @param path  Filesystem path
     * @param data  Raw payload bytes
     *
     * @return Integer result; negative values indicate an error code
     */
    /**
     * @brief Writes
     *
     * @param path  Filesystem path
     * @param data  Raw payload bytes
     *
     * @return Integer result; negative values indicate an error code
     */
    /**
     * @brief Writes
     *
     * @param path  Filesystem path
     * @param data  Raw payload bytes
     *
     * @return Integer result; negative values indicate an error code
     */
    [[nodiscard]] std::expected<void, std::string>
    write(std::string_view path, std::span<const std::byte> data) noexcept override;

    /**
     * @brief Removes
     *
     * @param path  Filesystem path
     *
     * @return Integer result; negative values indicate an error code
     */
    /**
     * @brief Removes
     *
     * @param path  Filesystem path
     *
     * @return Integer result; negative values indicate an error code
     */
    /**
     * @brief Removes
     *
     * @param path  Filesystem path
     *
     * @return Integer result; negative values indicate an error code
     */
    /**
     * @brief Removes
     *
     * @param path  Filesystem path
     *
     * @return Integer result; negative values indicate an error code
     */
    /**
     * @brief Removes
     *
     * @param path  Filesystem path
     *
     * @return Integer result; negative values indicate an error code
     */
    /**
     * @brief Removes
     *
     * @param path  Filesystem path
     *
     * @return Integer result; negative values indicate an error code
     */
    /**
     * @brief Removes
     *
     * @param path  Filesystem path
     *
     * @return Integer result; negative values indicate an error code
     */
    /**
     * @brief Removes
     *
     * @param path  Filesystem path
     *
     * @return Integer result; negative values indicate an error code
     */
    /**
     * @brief Removes
     *
     * @param path  Filesystem path
     *
     * @return Integer result; negative values indicate an error code
     */
    /**
     * @brief Removes
     *
     * @param path  Filesystem path
     *
     * @return Integer result; negative values indicate an error code
     */
    /**
     * @brief Removes
     *
     * @param path  Filesystem path
     *
     * @return Integer result; negative values indicate an error code
     */
    [[nodiscard]] std::expected<void, std::string>
    remove(std::string_view path) noexcept override;

    /// stat() synthesises directory entries from path prefixes so it never
    /// returns exists=false for a directory that has at least one descendant.
    /**
     * @brief Stat
     *
     * @param path  Filesystem path
     *
     * @return Stat result
     */
    /**
     * @brief Stat
     *
     * @param path  Filesystem path
     *
     * @return Stat result
     */
    /**
     * @brief Stat
     *
     * @param path  Filesystem path
     *
     * @return Stat result
     */
    /**
     * @brief Stat
     *
     * @param path  Filesystem path
     *
     * @return Stat result
     */
    /**
     * @brief Stat
     *
     * @param path  Filesystem path
     *
     * @return Stat result
     */
    /**
     * @brief Stat
     *
     * @param path  Filesystem path
     *
     * @return Stat result
     */
    /**
     * @brief Stat
     *
     * @param path  Filesystem path
     *
     * @return Stat result
     */
    /**
     * @brief Stat
     *
     * @param path  Filesystem path
     *
     * @return Stat result
     */
    /**
     * @brief Stat
     *
     * @param path  Filesystem path
     *
     * @return Stat result
     */
    /**
     * @brief Stat
     *
     * @param path  Filesystem path
     *
     * @return Stat result
     */
    /**
     * @brief Stat
     *
     * @param path  Filesystem path
     *
     * @return Stat result
     */
    [[nodiscard]] Stat
    stat(std::string_view path) noexcept override;

    /// list() returns immediate children only.  Subdirectory names carry a
    /// trailing '/' (consistent with LocalMount).
    /**
     * @brief List
     *
     * @param path  Filesystem path
     *
     * @return Integer result; negative values indicate an error code
     */
    /**
     * @brief List
     *
     * @param path  Filesystem path
     *
     * @return Integer result; negative values indicate an error code
     */
    /**
     * @brief List
     *
     * @param path  Filesystem path
     *
     * @return Integer result; negative values indicate an error code
     */
    /**
     * @brief List
     *
     * @param path  Filesystem path
     *
     * @return Integer result; negative values indicate an error code
     */
    /**
     * @brief List
     *
     * @param path  Filesystem path
     *
     * @return Integer result; negative values indicate an error code
     */
    /**
     * @brief List
     *
     * @param path  Filesystem path
     *
     * @return Integer result; negative values indicate an error code
     */
    /**
     * @brief List
     *
     * @param path  Filesystem path
     *
     * @return Integer result; negative values indicate an error code
     */
    /**
     * @brief List
     *
     * @param path  Filesystem path
     *
     * @return Integer result; negative values indicate an error code
     */
    /**
     * @brief List
     *
     * @param path  Filesystem path
     *
     * @return Integer result; negative values indicate an error code
     */
    /**
     * @brief List
     *
     * @param path  Filesystem path
     *
     * @return Integer result; negative values indicate an error code
     */
    /**
     * @brief List
     *
     * @param path  Filesystem path
     *
     * @return Integer result; negative values indicate an error code
     */
    [[nodiscard]] std::vector<std::string>
    list(std::string_view path) noexcept override;

    /// mkdir() is a no-op for MemMount: directories are implicit.
    /**
     * @brief Mkdir
     *
     * @param param0  Red channel component [0, 1]
     *
     * @return Integer result; negative values indicate an error code
     */
    /**
     * @brief Mkdir
     *
     * @param param0  Red channel component [0, 1]
     *
     * @return Integer result; negative values indicate an error code
     */
    /**
     * @brief Mkdir
     *
     * @param param0  Red channel component [0, 1]
     *
     * @return Integer result; negative values indicate an error code
     */
    /**
     * @brief Mkdir
     *
     * @param param0  Red channel component [0, 1]
     *
     * @return Integer result; negative values indicate an error code
     */
    /**
     * @brief Mkdir
     *
     * @param param0  Red channel component [0, 1]
     *
     * @return Integer result; negative values indicate an error code
     */
    /**
     * @brief Mkdir
     *
     * @param param0  Red channel component [0, 1]
     *
     * @return Integer result; negative values indicate an error code
     */
    /**
     * @brief Mkdir
     *
     * @param param0  Red channel component [0, 1]
     *
     * @return Integer result; negative values indicate an error code
     */
    /**
     * @brief Mkdir
     *
     * @param param0  Red channel component [0, 1]
     *
     * @return Integer result; negative values indicate an error code
     */
    /**
     * @brief Mkdir
     *
     * @param param0  Red channel component [0, 1]
     *
     * @return Integer result; negative values indicate an error code
     */
    /**
     * @brief Mkdir
     *
     * @param param0  Red channel component [0, 1]
     *
     * @return Integer result; negative values indicate an error code
     */
    /**
     * @brief Mkdir
     *
     * @param param0  Red channel component [0, 1]
     *
     * @return Integer result; negative values indicate an error code
     */
    [[nodiscard]] std::expected<void, std::string>
    mkdir(std::string_view /*path*/) noexcept override
    {
        return {};
    }

private:


    /// Normalise a virtual path: strip leading '/', collapse duplicate '/'.
    /// "//foo//bar/" → "foo/bar"
    [[nodiscard]] static std::string normalise(std::string_view path) noexcept;

    /// Return true if at least one stored key has `dir_prefix` as a prefix,
    /// where `dir_prefix` is a directory path (no trailing slash expected here).
    /// Used by stat() to synthesise directory existence.
    [[nodiscard]] bool has_children(const std::string& dir_prefix) const noexcept;


    mutable std::shared_mutex                              mu_;
    std::unordered_map<std::string, std::vector<std::byte>> entries_;
};

} // namespace pce::vfs
