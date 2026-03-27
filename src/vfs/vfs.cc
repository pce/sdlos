#include "vfs.h"

#include <iostream>
#include <SDL3/SDL_filesystem.h>

namespace pce::vfs {

/**
 * @brief Mounts local
 *
 * @param scheme  Opaque resource handle
 * @param root    Red channel component [0, 1]
 */
void Vfs::mount_local(std::string scheme, std::filesystem::path root)
{
    auto impl = std::make_unique<LocalMount>(std::move(root));
    mount(std::move(scheme), std::move(impl));
}

void Vfs::mount(std::string scheme, std::unique_ptr<IMount> impl)
{
    std::unique_lock lock(mu_);
    mounts_[std::move(scheme)] = std::move(impl);
}

/**
 * @brief Unmounts
 *
 * @param scheme  Opaque resource handle
 */
void Vfs::unmount(std::string_view scheme) noexcept
{
    std::unique_lock lock(mu_);
    const auto it = mounts_.find(std::string(scheme));
    if (it != mounts_.end()) mounts_.erase(it);
}

/**
 * @brief Schemes
 *
 * @return Integer result; negative values indicate an error code
 */
std::vector<std::string> Vfs::schemes() const noexcept
{
    try {
        std::shared_lock lock(mu_);
        std::vector<std::string> result;
        result.reserve(mounts_.size());
        for (const auto& [s, _] : mounts_) result.push_back(s);
        return result;
    } catch (...) {
        return {};
    }
}

/**
 * @brief Searches for and returns
 *
 * @param scheme  Opaque resource handle
 *
 * @return Pointer to the result, or nullptr on failure
 */
IMount* Vfs::find(std::string_view scheme) noexcept
{
    std::shared_lock lock(mu_);
    const auto it = mounts_.find(std::string(scheme));
    return (it != mounts_.end()) ? it->second.get() : nullptr;
}

/**
 * @brief Searches for and returns
 *
 * @param scheme  Opaque resource handle
 *
 * @return Pointer to the result, or nullptr on failure
 */
const IMount* Vfs::find(std::string_view scheme) const noexcept
{
    std::shared_lock lock(mu_);
    const auto it = mounts_.find(std::string(scheme));
    return (it != mounts_.end()) ? it->second.get() : nullptr;
}

/**
 * @brief Sets default scheme
 *
 * @param scheme  Opaque resource handle
 */
void Vfs::set_default_scheme(std::string scheme) noexcept
{
    std::unique_lock lock(mu_);
    default_scheme_ = std::move(scheme);
}

/**
 * @brief Default scheme
 *
 * @return Integer result; negative values indicate an error code
 */
const std::string& Vfs::default_scheme() const noexcept
{
    return default_scheme_;
}

std::expected<Vfs::Dispatch, std::string>
/**
 * @brief Dispatches
 *
 * @param uri  Red channel component [0, 1]
 *
 * @return Integer result; negative values indicate an error code
 */
Vfs::dispatch(std::string_view uri) const noexcept
{
    const Uri parsed  = Uri::parse(uri);
    const std::string& chosen = parsed.has_scheme() ? parsed.scheme : default_scheme_;

    if (chosen.empty()) {
        return std::unexpected(
            std::string{"no scheme and no default scheme for URI: "}.append(uri));
    }

    std::shared_lock lock(mu_);
    const auto it = mounts_.find(chosen);
    if (it == mounts_.end()) {
        return std::unexpected(
            std::string{"unknown scheme '"} + chosen + "' in URI: " + std::string(uri));
    }

    return Dispatch{ it->second.get(), parsed.path };
}

std::expected<std::vector<std::byte>, std::string>
Vfs::read(std::string_view uri) noexcept
{
    auto d = dispatch(uri);
    if (!d) return std::unexpected(d.error());
    return d->mount->read(d->path);
}

std::expected<std::string, std::string>
/**
 * @brief Reads text
 *
 * @param uri  Red channel component [0, 1]
 *
 * @return Integer result; negative values indicate an error code
 */
Vfs::read_text(std::string_view uri) noexcept
{
    auto bytes = read(uri);
    if (!bytes) return std::unexpected(bytes.error());
    return std::string{reinterpret_cast<const char*>(bytes->data()), bytes->size()}; // NOLINT
}

std::expected<void, std::string>
Vfs::write(std::string_view uri, std::span<const std::byte> data) noexcept
{
    auto d = dispatch(uri);
    if (!d) return std::unexpected(d.error());
    return d->mount->write(d->path, data);
}

std::expected<void, std::string>
/**
 * @brief Writes text
 *
 * @param uri   Red channel component [0, 1]
 * @param text  UTF-8 text content
 *
 * @return Integer result; negative values indicate an error code
 */
Vfs::write_text(std::string_view uri, std::string_view text) noexcept
{
    const auto span = std::span<const std::byte>{
        reinterpret_cast<const std::byte*>(text.data()), // NOLINT
        text.size()
    };
    return write(uri, span);
}

std::expected<void, std::string>
/**
 * @brief Removes
 *
 * @param uri  Red channel component [0, 1]
 *
 * @return Integer result; negative values indicate an error code
 */
Vfs::remove(std::string_view uri) noexcept
{
    auto d = dispatch(uri);
    if (!d) return std::unexpected(d.error());
    return d->mount->remove(d->path);
}

/**
 * @brief Stat
 *
 * @param uri  Red channel component [0, 1]
 *
 * @return Stat result
 */
Stat Vfs::stat(std::string_view uri) noexcept
{
    auto d = dispatch(uri);
    if (!d) return {};
    return d->mount->stat(d->path);
}

/**
 * @brief Exists
 *
 * @param uri  Red channel component [0, 1]
 *
 * @return true on success, false on failure
 */
bool Vfs::exists(std::string_view uri) noexcept
{
    return stat(uri).exists;
}

/**
 * @brief List
 *
 * @param uri  Red channel component [0, 1]
 *
 * @return Integer result; negative values indicate an error code
 */
std::vector<std::string> Vfs::list(std::string_view uri) noexcept
{
    auto d = dispatch(uri);
    if (!d) return {};
    return d->mount->list(d->path);
}

std::expected<void, std::string>
/**
 * @brief Mkdir
 *
 * @param uri  Red channel component [0, 1]
 *
 * @return Integer result; negative values indicate an error code
 */
Vfs::mkdir(std::string_view uri) noexcept
{
    auto d = dispatch(uri);
    if (!d) return std::unexpected(d.error());
    return d->mount->mkdir(d->path);
}

/**
 * @brief Mounts platform defaults
 *
 * @param org  Red channel component [0, 1]
 * @param app  Alpha channel component [0, 1]
 *
 * @return true on success, false on failure
 */
bool Vfs::mount_platform_defaults(const char* org, const char* app)
{
    bool ok = true;

    // user:// — platform user-data directory (writable).
    if (const char* pref = SDL_GetPrefPath(org, app)) {
        if (pref[0] != '\0') {
            mount_local("user", std::filesystem::path(pref));
            set_default_scheme("user");
        } else {
            ok = false;
        }
        SDL_free((void*)pref);
    } else {
        ok = false;
    }

    // asset:// — binary base path (read-only: installed assets, shaders).
    if (const char* base = SDL_GetBasePath()) {
        if (base[0] != '\0') {
            mount_local("asset", std::filesystem::path(base));
        } else {
            ok = false;
        }
        SDL_free((void*)base);
    } else {
        ok = false;
    }

    return ok;
}

/**
 * @brief Dumps mounts
 */
void Vfs::dump_mounts() const noexcept
{
    try {
        std::shared_lock lock(mu_);
        std::cerr << "[vfs] registered mounts (" << mounts_.size() << "):\n";
        for (const auto& [scheme, mount] : mounts_) {
            const auto* local = dynamic_cast<const LocalMount*>(mount.get());
            const auto* mem   = dynamic_cast<const MemMount*>(mount.get());
            std::string desc;
            if      (local) desc = "LocalMount @ " + local->root().string();
            else if (mem)   desc = "MemMount ("  + std::to_string(mem->size()) + " entries)";
            else            desc = "IMount (custom)";
            std::cerr << "  " << scheme << "://  →  " << desc << "\n";
        }
        if (!default_scheme_.empty())
            std::cerr << "  (default: " << default_scheme_ << ")\n";
    } catch (...) {}
}

} // namespace pce::vfs
