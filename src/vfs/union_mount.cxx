#include "union_mount.hh"

#include <algorithm>
#include <unordered_set>

namespace pce::vfs {

void UnionMount::insert_sorted(Layer layer)
{
    auto it = std::find_if(layers_.begin(), layers_.end(),
        [p = layer.priority](const Layer& l) { return l.priority < p; });
    layers_.insert(it, std::move(layer));
}

std::vector<UnionMount::Layer> UnionMount::snapshot() const noexcept
{
    std::shared_lock lock(mu_);
    return layers_;
}

IMount* UnionMount::add(std::unique_ptr<IMount> mount, int priority, bool read_only)
{
    if (!mount) return nullptr;
    IMount* raw = mount.get();
    std::unique_lock lock(mu_);
    insert_sorted(Layer{ std::shared_ptr<IMount>(std::move(mount)), priority, read_only });
    return raw;
}

IMount* UnionMount::add_shared(std::shared_ptr<IMount> mount, int priority, bool read_only)
{
    if (!mount) return nullptr;
    IMount* raw = mount.get();
    std::unique_lock lock(mu_);
    insert_sorted(Layer{ std::move(mount), priority, read_only });
    return raw;
}

void UnionMount::remove_priority(int priority) noexcept
{
    std::unique_lock lock(mu_);
    layers_.erase(
        std::remove_if(layers_.begin(), layers_.end(),
            [priority](const Layer& l) { return l.priority == priority; }),
        layers_.end());
}

void UnionMount::clear() noexcept
{
    std::unique_lock lock(mu_);
    layers_.clear();
}

std::vector<UnionMount::LayerInfo> UnionMount::layers() const noexcept
{
    std::shared_lock lock(mu_);
    std::vector<LayerInfo> out;
    out.reserve(layers_.size());
    for (const auto& l : layers_)
        out.push_back(LayerInfo{ l.priority, l.read_only, l.mount.get() });
    return out;
}

std::size_t UnionMount::layer_count() const noexcept
{
    std::shared_lock lock(mu_);
    return layers_.size();
}

std::expected<std::vector<std::byte>, std::string>
UnionMount::read(std::string_view path) noexcept
{
    const auto snap = snapshot();
    if (snap.empty()) {
        return std::unexpected(
            std::string{"UnionMount: no layers for read of: "}.append(path));
    }

    std::string last_err;
    for (const auto& layer : snap) {
        auto result = layer.mount->read(path);
        if (result) return result;
        last_err = result.error();
    }
    return std::unexpected(std::move(last_err));
}

std::expected<void, std::string>
UnionMount::write(std::string_view path, std::span<const std::byte> data) noexcept
{
    for (const auto& layer : snapshot()) {
        if (layer.read_only) continue;
        return layer.mount->write(path, data);
    }
    return std::unexpected(
        std::string{"UnionMount: no writable layer for write of: "}.append(path));
}

std::expected<void, std::string>
UnionMount::remove(std::string_view path) noexcept
{
    for (const auto& layer : snapshot()) {
        if (layer.read_only) continue;
        if (layer.mount->stat(path).exists)
            return layer.mount->remove(path);
    }
    return {};
}

Stat UnionMount::stat(std::string_view path) noexcept
{
    for (const auto& layer : snapshot()) {
        Stat s = layer.mount->stat(path);
        if (s.exists) return s;
    }
    return {};
}

std::vector<std::string> UnionMount::list(std::string_view path) noexcept
{
    std::vector<std::string>        result;
    std::unordered_set<std::string> seen;

    for (const auto& layer : snapshot()) {
        for (auto& entry : layer.mount->list(path)) {
            if (seen.insert(entry).second)
                result.push_back(std::move(entry));
        }
    }

    std::sort(result.begin(), result.end());
    return result;
}

std::expected<void, std::string>
UnionMount::mkdir(std::string_view path) noexcept
{
    for (const auto& layer : snapshot()) {
        if (layer.read_only) continue;
        return layer.mount->mkdir(path);
    }
    return std::unexpected(
        std::string{"UnionMount: no writable layer for mkdir of: "}.append(path));
}

} // namespace pce::vfs
