#include "vfs.hh"

#include <iostream>

namespace pce::vfs {

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

void Vfs::unmount(std::string_view scheme) noexcept
{
    std::unique_lock lock(mu_);
    const auto it = mounts_.find(std::string(scheme));
    if (it != mounts_.end()) mounts_.erase(it);
}

std::vector<std::string> Vfs::schemes() const noexcept
{
    std::shared_lock lock(mu_);
    std::vector<std::string> result;
    result.reserve(mounts_.size());
    for (const auto& [s, _] : mounts_) result.push_back(s);
    return result;
}

IMount* Vfs::find(std::string_view scheme) noexcept
{
    std::shared_lock lock(mu_);
    const auto it = mounts_.find(std::string(scheme));
    return (it != mounts_.end()) ? it->second.get() : nullptr;
}

const IMount* Vfs::find(std::string_view scheme) const noexcept
{
    std::shared_lock lock(mu_);
    const auto it = mounts_.find(std::string(scheme));
    return (it != mounts_.end()) ? it->second.get() : nullptr;
}

void Vfs::set_default_scheme(std::string scheme) noexcept
{
    std::unique_lock lock(mu_);
    default_scheme_ = std::move(scheme);
}

const std::string& Vfs::default_scheme() const noexcept
{
    return default_scheme_;
}

std::expected<Vfs::Dispatch, std::string>
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
Vfs::read_text(std::string_view uri) noexcept
{
    auto bytes = read(uri);
    if (!bytes) return std::unexpected(bytes.error());
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return std::string(reinterpret_cast<const char*>(bytes->data()), bytes->size());
}

std::expected<void, std::string>
Vfs::write(std::string_view uri, std::span<const std::byte> data) noexcept
{
    auto d = dispatch(uri);
    if (!d) return std::unexpected(d.error());
    return d->mount->write(d->path, data);
}

std::expected<void, std::string>
Vfs::write_text(std::string_view uri, std::string_view text) noexcept
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const auto span = std::span<const std::byte>{
        reinterpret_cast<const std::byte*>(text.data()),
        text.size()
    };
    return write(uri, span);
}

std::expected<void, std::string>
Vfs::remove(std::string_view uri) noexcept
{
    auto d = dispatch(uri);
    if (!d) return std::unexpected(d.error());
    return d->mount->remove(d->path);
}

Stat Vfs::stat(std::string_view uri) noexcept
{
    auto d = dispatch(uri);
    if (!d) return {};
    return d->mount->stat(d->path);
}

bool Vfs::exists(std::string_view uri) noexcept
{
    return stat(uri).exists;
}

std::vector<std::string> Vfs::list(std::string_view uri) noexcept
{
    auto d = dispatch(uri);
    if (!d) return {};
    return d->mount->list(d->path);
}

std::expected<void, std::string>
Vfs::mkdir(std::string_view uri) noexcept
{
    auto d = dispatch(uri);
    if (!d) return std::unexpected(d.error());
    return d->mount->mkdir(d->path);
}

void Vfs::dump_mounts() const noexcept
{
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
}

} // namespace pce::vfs
