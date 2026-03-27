#pragma once

// IMount that layers multiple mounts with priority ordering.
//
// UnionMount is the "base + patch" primitive for sdlos's virtual file system.
// It holds an ordered list of IMount layers (highest priority first) and
// dispatches reads as a waterfall: try layer[0], fall through to layer[1], …
// until one succeeds.  Writes go to the first non-read-only layer.  Directory
// listings are the union of all layers (higher-priority entries shadow lower).
//
// Typical usage pattern (theroretical)s:
//
//  1. Base game + DLC patch:
//
//       auto scene = std::make_unique<UnionMount>();
//       scene->add(std::make_unique<LocalMount>(dlc_dir),  10);  // DLC first
//       scene->add(std::make_unique<LocalMount>(base_dir),  0);  // fallback
//       vfs.mount("scene", std::move(scene));
//
//       vfs.read("scene://textures/hero.png");
//         ↳ tries dlc_dir/textures/hero.png  → found  → done
//         ↳ OR: not found → tries base_dir/textures/hero.png → done
//
//  2. Hot-reload in-memory overlay on top of disk:
//
//       auto mem = std::make_shared<MemMount>();
//       auto scene = std::make_unique<UnionMount>();
//       scene->add(mem, 10, /*read_only=*/false);  // in-memory edits override
//       scene->add(std::make_unique<LocalMount>(jade_dir), 0, /*read_only=*/true);
//       vfs.mount("scene", std::move(scene));
//
//       // Later, inject a hot-reloaded file:
//       mem->put_text("shaders/ui.metal", new_source);
//       // vfs.read("scene://shaders/ui.metal") now returns the injected bytes.
//
//  3. LOD selection (high-res + low-res):
//
//       auto tex = std::make_unique<UnionMount>();
//       if (high_end_device)
//           tex->add(std::make_unique<LocalMount>(hq_dir), 10, true);
//       tex->add(std::make_unique<LocalMount>(base_dir), 0, true);
//       vfs.mount("asset", std::move(tex));
//
//       // vfs.read("asset://tex/player.png") picks HQ when available.
//
// Read semantics
//   - Layers are sorted by priority descending (highest = checked first).
//   - A "miss" is any result where std::expected carries an error.
//   - The first *success* is returned immediately; subsequent layers are not
//     consulted.
//   - If all layers miss, the last error message is returned.
//
// Write semantics
//   - The first non-read-only layer in priority order receives the write.
//   - If all layers are read-only, returns std::unexpected.
//
// stat() semantics
//   - Returns the Stat from the first layer that reports exists=true.
//   - A file that only exists in layer[1] (shadowed by a read of layer[0])
//     is still stat-able: the Stat of the highest layer containing it is
//     returned.
//
// list() semantics
//   - Returns the union of immediate children across ALL layers.
//   - A name present in multiple layers is deduplicated (first occurrence,
//     i.e. the highest-priority one, wins).
//   - Results are sorted lexicographically.
//
// remove() semantics
//   - Removes from the first non-read-only layer where the path exists.
//   - Does NOT attempt to "shadow-delete" a file that lives only in a
//     lower-priority read-only layer — that would require a "whiteout" entry,
//     which is out of scope here.
//
// Thread-safety
//   - The layers_ vector is guarded by a shared_mutex.
//   - add() / remove_priority() are exclusive writes.
//   - All IO operations (read, write, stat, …) take a shared lock to snapshot
//     the layer list, then release it before calling into individual mounts.
//     Each IMount is responsible for its own internal locking.

#include "mount.h"

#include <memory>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

namespace pce::vfs {

class UnionMount final : public IMount {
public:

    /**
     * @brief Union mount
     */
    UnionMount()  = default;
    /**
     * @brief ~union mount
     */
    ~UnionMount() = default;

    // Non-copyable; non-movable (owns a shared_mutex).
    /**
     * @brief Union mount
     *
     * @param param0  Red channel component [0, 1]
     */
    UnionMount(const UnionMount&)            = delete;
    UnionMount& operator=(const UnionMount&) = delete;
    /**
     * @brief Union mount
     *
     * @param param0  Red channel component [0, 1]
     */
    UnionMount(UnionMount&&)                 = delete;
    UnionMount& operator=(UnionMount&&)      = delete;

    // =========================================================================
    // Layer management
    // =========================================================================

    // Layer descriptor — returned by layers() for inspection / debugging.
    struct LayerInfo {
        int         priority  = 0;
        bool        read_only = false;
        IMount*     mount     = nullptr;   // non-owning view
    };

    /// Add a layer.  Ownership is transferred.
    ///
    /// `priority`  — higher values are consulted first for reads.
    ///               Layers with the same priority are ordered by insertion
    ///               time (most recently added = first among equals).
    ///
    /// `read_only` — when true the layer is never the target of write(),
    ///               remove(), or mkdir().  Does NOT prevent reads through it.
    ///
    /// Returns a raw non-owning pointer to the added mount (useful for
    /// configuring the mount after insertion, e.g. seeding a MemMount).
    IMount* add(std::unique_ptr<IMount> mount,
                int  priority  = 0,
                bool read_only = false);

    /// Add a shared-ownership layer.  Use this when you need to retain a
    /// reference outside the UnionMount (e.g. to hot-patch a MemMount):
    ///
    ///   auto mem = std::make_shared<MemMount>();
    ///   union_mount->add_shared(mem, 10);
    ///   mem->put_text("config.json", "{}");  // override after registration
    IMount* add_shared(std::shared_ptr<IMount> mount,
                       int  priority  = 0,
                       bool read_only = false);

    /// Remove all layers with the given priority.
    /// No-op if none match.
    void remove_priority(int priority) noexcept;

    /// Remove all layers.
    void clear() noexcept;

    /// Return a snapshot of current layer descriptors (priority desc order).
    [[nodiscard]] std::vector<LayerInfo> layers() const noexcept;

    /// Return the number of registered layers.
    [[nodiscard]] std::size_t layer_count() const noexcept;

    // =========================================================================
    // IMount
    // =========================================================================

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

private:

    // =========================================================================
    // Internal
    // =========================================================================

    struct Layer {
        std::shared_ptr<IMount> mount;     // shared so add_shared() works cleanly
        int                     priority  = 0;
        bool                    read_only = false;
    };

    /// Insert a Layer into layers_, maintaining priority-descending order.
    /// Among equal priorities, newest insertion goes first (prepended before
    /// existing same-priority layers).
    void insert_sorted(Layer layer);

    /// Under a shared lock, copy the layer list into a local snapshot.
    /// Returns raw pointers into the shared_ptrs, valid as long as no
    /// remove() or clear() happens concurrently with the caller's use.
    ///
    /// Because mount lifetimes extend beyond the lock (shared_ptr keeps them
    /// alive even after remove()), callers that hold a shared_ptr snapshot
    /// are safe even if another thread concurrently removes layers.
    [[nodiscard]] std::vector<Layer> snapshot() const noexcept;

    // ---- State -------------------------------------------------------

    mutable std::shared_mutex mu_;
    std::vector<Layer>        layers_;  // sorted priority desc
};

} // namespace pce::vfs
