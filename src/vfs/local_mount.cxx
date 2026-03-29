#include "local_mount.hh"

#include <chrono>
#include <fstream>
#include <system_error>

namespace pce::vfs {

LocalMount::LocalMount(std::filesystem::path root) noexcept
{
    std::error_code ec;
    root_ = std::filesystem::absolute(root, ec).lexically_normal();
    if (ec) root_ = root.lexically_normal();
    std::filesystem::create_directories(root_, ec);
}

std::expected<std::filesystem::path, std::string>
LocalMount::resolve(std::string_view vpath) const noexcept
{
    if (vpath.empty()) return root_;

    if (vpath.front() == '/' || vpath.front() == '\\') {
        return std::unexpected(
            std::string{"absolute paths not permitted: "}.append(vpath));
    }

    const auto full     = (root_ / vpath).lexically_normal();
    const auto root_end = root_.end();

    // Iterator mismatch is immune to the "/root/foo" vs "/root/foobar"
    // ambiguity that plagues raw string prefix comparisons.
    auto [ra, fb] = std::mismatch(root_.begin(), root_end, full.begin());
    if (ra != root_end) {
        return std::unexpected(
            std::string{"path escapes mount root: "}.append(vpath));
    }

    return full;
}

std::expected<std::vector<std::byte>, std::string>
LocalMount::read(std::string_view path) noexcept
{
    const auto res = resolve(path);
    if (!res) return std::unexpected(res.error());

    std::ifstream f(*res, std::ios::binary);
    if (!f) return std::unexpected("cannot open: " + res->string());

    f.seekg(0, std::ios::end);
    const auto sz = f.tellg();
    if (sz < 0) return std::unexpected("seekg failed: " + res->string());
    f.seekg(0, std::ios::beg);

    std::vector<std::byte> buf(static_cast<std::size_t>(sz));
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(sz));
    if (!f && !f.eof()) return std::unexpected("read error: " + res->string());

    buf.resize(static_cast<std::size_t>(f.gcount()));
    return buf;
}

std::expected<void, std::string>
LocalMount::write(std::string_view path, std::span<const std::byte> data) noexcept
{
    const auto res = resolve(path);
    if (!res) return std::unexpected(res.error());

    std::error_code ec;
    std::filesystem::create_directories(res->parent_path(), ec);
    if (ec) {
        return std::unexpected("cannot create parent dirs for "
                               + res->string() + ": " + ec.message());
    }

    std::ofstream f(*res, std::ios::binary | std::ios::trunc);
    if (!f) return std::unexpected("cannot create: " + res->string());

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    f.write(reinterpret_cast<const char*>(data.data()),
            static_cast<std::streamsize>(data.size()));
    if (!f) return std::unexpected("write error: " + res->string());

    return {};
}

std::expected<void, std::string>
LocalMount::remove(std::string_view path) noexcept
{
    const auto res = resolve(path);
    if (!res) return std::unexpected(res.error());

    std::error_code ec;
    std::filesystem::remove(*res, ec);
    if (ec) {
        return std::unexpected("remove failed for "
                               + res->string() + ": " + ec.message());
    }
    return {};
}

Stat LocalMount::stat(std::string_view path) noexcept
{
    const auto res = resolve(path);
    if (!res) return {};

    std::error_code ec;
    const auto s = std::filesystem::status(*res, ec);
    if (ec || s.type() == std::filesystem::file_type::not_found
           || s.type() == std::filesystem::file_type::none) {
        return {};
    }

    Stat out;
    out.exists = true;
    out.is_dir = std::filesystem::is_directory(s);

    if (!out.is_dir) {
        const auto sz = std::filesystem::file_size(*res, ec);
        if (!ec) out.size = static_cast<uint64_t>(sz);
    }

    const auto t = std::filesystem::last_write_time(*res, ec);
    if (!ec) {
        out.mtime = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                t.time_since_epoch()).count());
    }

    return out;
}

std::vector<std::string> LocalMount::list(std::string_view path) noexcept
{
    const auto res = resolve(path);
    if (!res) return {};

    std::vector<std::string> result;
    std::error_code ec;

    for (const auto& entry : std::filesystem::directory_iterator(*res, ec)) {
        std::string name = entry.path().filename().string();
        if (entry.is_directory(ec)) name += '/';
        result.push_back(std::move(name));
    }

    return result;
}

std::expected<void, std::string>
LocalMount::mkdir(std::string_view path) noexcept
{
    const auto res = resolve(path);
    if (!res) return std::unexpected(res.error());

    std::error_code ec;
    std::filesystem::create_directories(*res, ec);
    if (ec) {
        return std::unexpected("mkdir failed for "
                               + res->string() + ": " + ec.message());
    }
    return {};
}

} // namespace pce::vfs
