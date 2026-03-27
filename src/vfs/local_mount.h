#pragma once

// vfs/local_mount.h — IMount backed by the real filesystem.
//
// Maps every virtual path to a real path by joining it with a sandboxed root:
//
//   LocalMount mount("/home/user/app/data");
//   mount.read("audio/t-rex-roar.wav");
//         ↳ opens "/home/user/app/data/audio/t-rex-roar.wav"
//
// Security
// resolve() rejects any virtual path whose lexically-normalised form would
// escape the root directory.  Both forms of traversal are blocked:
//
//   "../../etc/passwd"          → error: path escapes mount root
//   "../data/../../../etc"      → error: path escapes mount root
//   "/absolute/injection"       → error: absolute paths not permitted
//
// Note: symlink traversal is NOT checked — that is an OS-level concern.
// For a fully locked-down sandbox use a chroot or OS sandbox primitive in
// addition to this class.
//
// Thread-safety
// All operations are stateless (after construction) and delegate to the OS
// filesystem, which provides its own concurrency guarantees.  No internal
// mutex is needed.
//
// Binary safety
// read() / write() operate on raw bytes so text files, images, audio, fonts
// and shader blobs are all handled uniformly.

#include "mount.h"

#include <filesystem>

namespace pce::vfs {

class LocalMount final : public IMount {
public:


    /// Construct a LocalMount rooted at `root`.
    ///
    /// `root` is normalised to an absolute lexical path.  The directory is
    /// created (including any missing parents) if it does not already exist.
    ///
    /// This constructor does NOT throw; it records any directory-creation
    /// failure silently — individual operations will fail naturally if the
    /// root is inaccessible.
    explicit LocalMount(std::filesystem::path root) noexcept;


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
    [[nodiscard]] std::expected<void, std::string>
    remove(std::string_view path) noexcept override;

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

    /**
     * @brief Mkdir
     *
     * @param path  Filesystem path
     *
     * @return Integer result; negative values indicate an error code
     */
    /**
     * @brief Mkdir
     *
     * @param path  Filesystem path
     *
     * @return Integer result; negative values indicate an error code
     */
    /**
     * @brief Mkdir
     *
     * @param path  Filesystem path
     *
     * @return Integer result; negative values indicate an error code
     */
    /**
     * @brief Mkdir
     *
     * @param path  Filesystem path
     *
     * @return Integer result; negative values indicate an error code
     */
    /**
     * @brief Mkdir
     *
     * @param path  Filesystem path
     *
     * @return Integer result; negative values indicate an error code
     */
    /**
     * @brief Mkdir
     *
     * @param path  Filesystem path
     *
     * @return Integer result; negative values indicate an error code
     */
    /**
     * @brief Mkdir
     *
     * @param path  Filesystem path
     *
     * @return Integer result; negative values indicate an error code
     */
    /**
     * @brief Mkdir
     *
     * @param path  Filesystem path
     *
     * @return Integer result; negative values indicate an error code
     */
    /**
     * @brief Mkdir
     *
     * @param path  Filesystem path
     *
     * @return Integer result; negative values indicate an error code
     */
    [[nodiscard]] std::expected<void, std::string>
    mkdir(std::string_view path) noexcept override;

    // ---- Accessors -------------------------------------------------------

    /// The normalised absolute root used for all path resolution.
    [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }

private:

    // ---- Internal helpers ------------------------------------------------

    /// Resolve `vpath` (a mount-relative virtual path) to a real absolute
    /// filesystem path, enforcing the sandbox constraint.
    ///
    /// Returns std::unexpected with a human-readable message if:
    ///   - `vpath` is absolute (starts with '/' or '\')
    ///   - the normalised result would escape root_
    /**
     * @brief Resolves
     *
     * @param vpath  Filesystem path
     *
     * @return Integer result; negative values indicate an error code
     */
    /**
     * @brief Resolves
     *
     * @param vpath  Filesystem path
     *
     * @return Integer result; negative values indicate an error code
     */
    /**
     * @brief Resolves
     *
     * @param vpath  Filesystem path
     *
     * @return Integer result; negative values indicate an error code
     */
    /**
     * @brief Resolves
     *
     * @param vpath  Filesystem path
     *
     * @return Integer result; negative values indicate an error code
     */
    /**
     * @brief Resolves
     *
     * @param vpath  Filesystem path
     *
     * @return Integer result; negative values indicate an error code
     */
    /**
     * @brief Resolves
     *
     * @param vpath  Filesystem path
     *
     * @return Integer result; negative values indicate an error code
     */
    /**
     * @brief Resolves
     *
     * @param vpath  Filesystem path
     *
     * @return Integer result; negative values indicate an error code
     */
    /**
     * @brief Resolves
     *
     * @param vpath  Filesystem path
     *
     * @return Integer result; negative values indicate an error code
     */
    /**
     * @brief Resolves
     *
     * @param vpath  Filesystem path
     *
     * @return Integer result; negative values indicate an error code
     */
    [[nodiscard]] std::expected<std::filesystem::path, std::string>
    resolve(std::string_view vpath) const noexcept;

    // ---- State -----------------------------------------------------------

    std::filesystem::path root_;   ///< absolute, lexically_normal()'d root
};

} // namespace pce::vfs
