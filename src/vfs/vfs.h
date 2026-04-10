#pragma once

// The sdlos virtual file system dispatcher.
//
// Vfs maps URI schemes to IMount implementations and provides a single
// unified API for all file I/O across the engine.
//
// Quick start
//
//   pce::vfs::Vfs vfs;
//
//   // Register mounts at startup:
//   vfs.mount_local("asset", SDL_GetBasePath());          // asset://shaders/ui.metal
//   vfs.mount_local("scene", jade_dir);                   // scene://data/pipeline.pug
//   vfs.mount_local("tmp",   "/tmp/sdlos");               // tmp://cache/atlas.png
//   vfs.mount("mem", std::make_unique<MemMount>());       // mem://scratch/buf
//
//   vfs.set_default_scheme("asset");   // bare "data/foo" resolves via "asset"
//
//   // Read:
//   auto wav  = vfs.read("scene://assets/audio/t-rex-roar.wav");
//   auto text = vfs.read_text("scene://data/pipeline.pug");
//   bool ok   = vfs.exists("asset://shaders/ui.metal");
//
//   // Write:
//   vfs.write_text("tmp://log/session.txt", "hello\n");
//
// URI format
// See vfs/uri.h.  Short recap:
//
//   "scheme://path/to/file"   routed to the mount registered as 'scheme'
//   "bare/path/to/file"       uses the default scheme (set_default_scheme)
//
// Thread-safety
// mount() / unmount() are writer operations (exclusive lock on mounts_ map).
// All IO operations (read, write, stat, …) take a shared lock to find the
// mount, then release it before calling into the mount itself.  This means:
//   - Concurrent reads are fully parallel.
//   - Adding / removing mounts while IO is in flight is safe.
//   - Thread-safety of the data itself is delegated to each IMount.
//
// Error handling
// All IO methods return std::expected<T, std::string>.
// std::unexpected(msg) carries a human-readable error string.
// No exceptions are thrown.

#include "local_mount.h"
#include "mem_mount.h"
#include "mount.h"
#include "uri.h"

#include <filesystem>
#include <memory>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace pce::vfs {

class Vfs {
  public:
    Vfs()  = default;
    ~Vfs() = default;

    /**
     * @brief Vfs  Non-copyable, non-movable: owns a std::shared_mutex and unique_ptr<IMount>
     *             members.  All callers hold Vfs via unique_ptr or as a direct member, so
     *             neither requires moveability.
     *
     */
    Vfs(const Vfs &)            = delete;
    Vfs &operator=(const Vfs &) = delete;

    Vfs(Vfs &&)            = delete;
    Vfs &operator=(Vfs &&) = delete;

    /// Mount a real filesystem directory at `scheme`.
    ///
    /// The directory is created (with parents) if it does not exist.
    /// Any existing mount at `scheme` is replaced.
    ///
    /// Example:
    ///   vfs.mount_local("scene", "/home/user/app/data");
    ///   vfs.read("scene://audio/roar.wav");
    void mount_local(std::string scheme, std::filesystem::path root);

    /// Mount an arbitrary IMount implementation at `scheme`.
    ///
    /// Ownership is transferred.  Any existing mount at `scheme` is replaced.
    ///
    /// Example:
    ///   auto mem = std::make_unique<MemMount>();
    ///   mem->put_text("config.json", R"({"vol":1.0})");
    ///   vfs.mount("mem", std::move(mem));
    void mount(std::string scheme, std::unique_ptr<IMount> impl);

    /// Remove the mount registered at `scheme`.
    ///
    /// No-op if the scheme is not registered.
    /// After this call, IO on URIs with this scheme returns std::unexpected.
    void unmount(std::string_view scheme) noexcept;

    /// Return all currently registered scheme names (unordered).
    /// Returns an empty vector on (unlikely) allocation failure.
    [[nodiscard]]
    std::vector<std::string> schemes() const noexcept;

    /// Non-owning access to a mounted IMount by scheme name.
    ///
    /// Returns nullptr if the scheme is not registered.
    /// The pointer is valid as long as the mount is not replaced or unmounted.
    /// Caller must not delete the pointer.
    /*
     * @brief Searches for and returns
     *
     * @param scheme  Opaque resource handle
     *
     * @return Pointer to the result, or nullptr if the scheme is not registered.
     */
    [[nodiscard]]
    IMount *find(std::string_view scheme) noexcept;
    [[nodiscard]]
    const IMount *find(std::string_view scheme) const noexcept;

    /**
     * @brief Sets default scheme
     *
     * When a URI string has no scheme component (e.g. "data/pipeline.pug"),
     * Vfs routes it through the default scheme.
     * This lets existing code that
     * uses bare paths keep working
     * after mounts are introduced.
     *
     * Set to "" (default) to disable bare-path resolution: bare URIs will
     * return std::unexpected instead of silently routing anywhere.
     *
     * @param scheme  Opaque resource handle
     */
    void set_default_scheme(std::string scheme) noexcept;

    /**
     * @brief Default scheme

     *
     * @return Integer result; negative values indicate an error code
     */
    [[nodiscard]]
    const std::string &default_scheme() const noexcept;

    /// Read the entire file            into a byte vector.
    /// all accept full "scheme://path" URIs or bare paths.
    [[nodiscard]]
    std::expected<std::vector<std::byte>, std::string> read(std::string_view uri) noexcept;

    /// Read the entire file as a UTF-8 string.
    ///
    /// TODO atm just a wrapper around read(), simdut valid UTF-8.
    [[nodiscard]]
    std::expected<std::string, std::string> read_text(std::string_view uri) noexcept;

    /// Create-or-replace a file with the given bytes.
    [[nodiscard]]
    std::expected<void, std::string>
    write(std::string_view uri, std::span<const std::byte> data) noexcept;

    /// Create-or-replace a file with the given UTF-8 text.
    [[nodiscard]]
    std::expected<void, std::string>
    write_text(std::string_view uri, std::string_view text) noexcept;

    /// Remove a file or empty directory (idempotent — succeeds if absent).
    [[nodiscard]]
    std::expected<void, std::string> remove(std::string_view uri) noexcept;

    /// Query metadata.  stat().exists == false if the path is not found.
    [[nodiscard]]
    Stat stat(std::string_view uri) noexcept;

    /// Convenience: return stat().exists.
    [[nodiscard]]
    bool exists(std::string_view uri) noexcept;

    /// List immediate children of a directory.
    ///
    /// File names are returned as-is; subdirectory names have a trailing '/'.
    /// Returns an empty vector if the URI is not a directory or not found.
    [[nodiscard]]
    std::vector<std::string> list(std::string_view uri) noexcept;

    /**
     * @brief Mkdir
     *
     * @param uri    Create a directory (and any missing parents).
     *
     * @return Integer result; negative values indicate an error code
     */
    [[nodiscard]]
    std::expected<void, std::string> mkdir(std::string_view uri) noexcept;

    /// Emit a summary of all registered mounts to stderr.
    /// Format: "[vfs] scheme://  →  <type> @ <root-or-desc>\n"
    void dump_mounts() const noexcept;

    /// Mount the platform-default "user://" and "asset://" schemes.
    ///
    ///   - user://   →  SDL_GetPrefPath(org, app)  (writable user data)
    ///   - asset://  →  SDL_GetBasePath()          (read-only binary assets)
    ///
    /// Returns true if both were mounted successfully.
    /// Sets "user" as the default scheme if it was mounted.
    bool mount_platform_defaults(const char *org = "pce", const char *app = "sdlos");

  private:
    // Parsed dispatch result: a raw (non-owning) pointer to the IMount and
    // the mount-relative path string.  The pointer is valid only while the
    // shared lock is held — callers must copy the path before releasing it.
    struct Dispatch {
        IMount *mount = nullptr;
        std::string path;
    };

    /**
     * @brief Dispatches
     * Parse `uri`, look up the scheme (or default scheme for bare paths),
     * and return the mount + path.
     *
     * Returns std::unexpected if:
     *   - The scheme is absent and default_scheme_ is empty
     *   - The scheme is not registered
     *
     * @param uri
     *
     * @return Integer result; negative values indicate an error code
     */
    [[nodiscard]]
    std::expected<Dispatch, std::string> dispatch(std::string_view uri) const noexcept;

    mutable std::shared_mutex mu_;
    std::unordered_map<std::string, std::unique_ptr<IMount>> mounts_;
    std::string default_scheme_;
};

}  // namespace pce::vfs
