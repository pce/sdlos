#include "mem_mount.hh"

#include <algorithm>

namespace pce::vfs {

std::string MemMount::normalise(std::string_view path) noexcept
{
    if (path.empty()) return {};

    std::string out;
    out.reserve(path.size());
    bool last_slash = false;

    for (const char c : path) {
        if (c == '/') {
            if (!last_slash && !out.empty()) out += '/';
            last_slash = true;
        } else {
            out += c;
            last_slash = false;
        }
    }

    if (!out.empty() && out.back() == '/') out.pop_back();
    return out;
}

bool MemMount::has_children(const std::string& dir_prefix) const noexcept
{
    const std::string needle = dir_prefix + '/';
    for (const auto& [key, _] : entries_)
        if (key.starts_with(needle)) return true;
    return false;
}

void MemMount::put(std::string path, std::vector<std::byte> data)
{
    std::unique_lock lock(mu_);
    entries_[normalise(path)] = std::move(data);
}

void MemMount::put_text(std::string path, std::string_view text)
{
    std::vector<std::byte> bytes(text.size());
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    std::memcpy(bytes.data(), text.data(), text.size());
    put(std::move(path), std::move(bytes));
}

std::size_t MemMount::size() const noexcept
{
    std::shared_lock lock(mu_);
    return entries_.size();
}

void MemMount::clear() noexcept
{
    std::unique_lock lock(mu_);
    entries_.clear();
}

std::expected<std::vector<std::byte>, std::string>
MemMount::read(std::string_view path) noexcept
{
    const std::string key = normalise(path);
    std::shared_lock lock(mu_);
    const auto it = entries_.find(key);
    if (it == entries_.end())
        return std::unexpected("not found in mem mount: " + key);
    return it->second;
}

std::expected<void, std::string>
MemMount::write(std::string_view path, std::span<const std::byte> data) noexcept
{
    const std::string key = normalise(path);
    std::unique_lock lock(mu_);
    entries_[key].assign(data.begin(), data.end());
    return {};
}

std::expected<void, std::string>
MemMount::remove(std::string_view path) noexcept
{
    const std::string key = normalise(path);
    std::unique_lock lock(mu_);
    entries_.erase(key);
    return {};
}

Stat MemMount::stat(std::string_view path) noexcept
{
    const std::string key = normalise(path);
    std::shared_lock lock(mu_);

    const auto it = entries_.find(key);
    if (it != entries_.end())
        return Stat{ true, false, static_cast<uint64_t>(it->second.size()), 0 };

    if (!key.empty() && has_children(key))
        return Stat{ true, true, 0, 0 };

    if (key.empty() && !entries_.empty())
        return Stat{ true, true, 0, 0 };

    return {};
}

std::vector<std::string> MemMount::list(std::string_view path) noexcept
{
    const std::string dir    = normalise(path);
    const std::string prefix = dir.empty() ? "" : dir + '/';

    std::shared_lock lock(mu_);

    std::vector<std::string>        result;
    std::unordered_set<std::string> seen;

    for (const auto& [key, _] : entries_) {
        if (!key.starts_with(prefix)) continue;

        const std::string_view rest = std::string_view{key}.substr(prefix.size());
        if (rest.empty()) continue;

        const auto slash = rest.find('/');
        if (slash == std::string_view::npos) {
            result.emplace_back(rest);
        } else {
            std::string subdir{rest.substr(0, slash + 1)};
            if (seen.insert(subdir).second)
                result.push_back(std::move(subdir));
        }
    }

    std::sort(result.begin(), result.end());
    return result;
}

} // namespace pce::vfs
