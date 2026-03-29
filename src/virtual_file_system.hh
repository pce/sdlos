#pragma once

// virtual_file_system.hh — IFileSystem implementation backed by pce::vfs::Vfs.
//
// VirtualFileSystem is the bridge between the legacy IFileSystem interface
// (used by OS::filesystem()) and the new vfs:: subsystem introduced in
// src/vfs/.  All real work is delegated to a Vfs instance that owns one or
// more IMount implementations.
//
// Default layout (created by the constructor)
// ───────────────────────────────────────────
//
//   VirtualFileSystem vfs("/tmp/sdlos");
//
//   Registers a LocalMount at scheme "tmp" rooted at "/tmp/sdlos".
//   The default scheme is set to "tmp" so that bare IFileSystem calls like
//   readFile("logs/session.txt") resolve to "tmp://logs/session.txt"
//   without requiring callers to be updated.
//
// Extending with additional schemes
// ──────────────────────────────────
//
//   vfs.mount("scene", jade_dir);          // LocalMount at jade app directory
//   vfs.mount("asset", SDL_GetBasePath()); // LocalMount at binary directory
//   vfs.mount("mem",   std::make_unique<pce::vfs::MemMount>());
//
//   Then use the underlying Vfs for scheme-routed IO:
//
//   auto& v = os.filesystem().vfs();
//   auto wav = v.read("scene://assets/audio/t-rex-roar.wav");
//   auto cfg = v.read_text("mem://config.json");
//
// Thread-safety
// ─────────────
// All IFileSystem methods delegate to Vfs which uses a std::shared_mutex for
// the mounts map and delegates further to each IMount's own locking strategy.
// LocalMount is stateless (no mutex needed beyond the OS FS).
// MemMount has its own std::shared_mutex.

#include "i_file_system.hh"
#include "vfs/vfs.hh"

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace pce::sdlos {

class VirtualFileSystem : public IFileSystem {
public:

    // ---- Construction ----------------------------------------------------

    /// Construct an empty VirtualFileSystem with no mounts registered.
    ///
    /// No scheme is registered and no default scheme is set.  The caller is
    /// responsible for mounting at least one scheme before using the
    /// IFileSystem interface.  Use mount() to add LocalMount or MemMount
    /// instances, then call vfs().set_default_scheme() so that bare-path
    /// IFileSystem calls (readFile, createFile, …) resolve correctly.
    ///
    /// Typical setup in OS::boot():
    ///
    ///   fs_ = std::make_unique<VirtualFileSystem>();
    ///   fs_->mount("user",  SDL_GetPrefPath("pce", "sdlos"));
    ///   fs_->mount("asset", SDL_GetBasePath());
    ///   fs_->vfs().set_default_scheme("user");
    VirtualFileSystem();

    ~VirtualFileSystem() = default;

    // Non-copyable: owns the Vfs which owns IMount instances.
    VirtualFileSystem(const VirtualFileSystem&)            = delete;
    VirtualFileSystem& operator=(const VirtualFileSystem&) = delete;
    VirtualFileSystem(VirtualFileSystem&&)                 = delete;
    VirtualFileSystem& operator=(VirtualFileSystem&&)      = delete;

    // ---- Scheme mounting -------------------------------------------------
    //
    // Register additional mounts beyond the default "tmp" LocalMount.
    // After registration, the underlying Vfs can be used directly for
    // scheme-routed IO: vfs().read("scene://assets/foo.wav").

    /// Mount a real filesystem directory at `scheme`.
    ///
    ///   fs.mount("scene", "/path/to/jade/app");
    ///   fs.vfs().read("scene://data/pipeline.pug");
    void mount(std::string scheme, std::filesystem::path real_root);

    /// Mount a custom IMount implementation at `scheme`.
    ///
    ///   auto mem = std::make_unique<pce::vfs::MemMount>();
    ///   mem->put_text("config.json", R"({"vol":0.8})");
    ///   fs.mount("mem", std::move(mem));
    ///   fs.vfs().read_text("mem://config.json");
    void mount(std::string scheme, std::unique_ptr<pce::vfs::IMount> impl);

    // ---- Vfs access ------------------------------------------------------
    //
    // Direct access to the underlying Vfs for callers that have migrated to
    // the new URI-based API.  The reference is valid for the lifetime of this
    // VirtualFileSystem.

    [[nodiscard]] pce::vfs::Vfs&       vfs()       noexcept { return vfs_; }
    [[nodiscard]] const pce::vfs::Vfs& vfs() const noexcept { return vfs_; }

    // ---- IFileSystem -----------------------------------------------------
    //
    // All methods delegate to the Vfs default scheme ("tmp" by default).
    // They never throw; errors are reported as false / empty string.

    /// Create or replace `path` with `content`.
    /// Parent directories are created automatically.
    [[nodiscard]] bool
    createFile(std::string_view path, std::string_view content = "") override;

    /// Create the directory at `path` (and any missing parents).
    [[nodiscard]] bool
    createDirectory(std::string_view path) override;

    /// Remove the file at `path`.
    [[nodiscard]] bool
    deleteFile(std::string_view path) override;

    /// Read and return the entire file at `path` as a string.
    /// Returns "" on any error.
    [[nodiscard]] std::string
    readFile(std::string_view path) override;

    /// List immediate children of the directory at `path`.
    [[nodiscard]] std::vector<std::string>
    listDirectory(std::string_view path) override;

    /// Return true if `path` exists (file or directory).
    [[nodiscard]] bool
    exists(std::string_view path) override;

private:

    pce::vfs::Vfs vfs_;
};

} // namespace pce::sdlos
