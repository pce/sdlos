#include "virtual_file_system.hh"

#include <iostream>

namespace pce::sdlos {

VirtualFileSystem::VirtualFileSystem() {}

void VirtualFileSystem::mount(std::string scheme, std::filesystem::path real_root)
{
    vfs_.mount_local(std::move(scheme), std::move(real_root));
}

void VirtualFileSystem::mount(std::string scheme,
                              std::unique_ptr<pce::vfs::IMount> impl)
{
    vfs_.mount(std::move(scheme), std::move(impl));
}

bool VirtualFileSystem::createFile(std::string_view path, std::string_view content)
{
    const auto result = vfs_.write_text(path, content);
    if (!result) {
        std::cerr << "[vfs] createFile: " << result.error() << "\n";
        return false;
    }
    return true;
}

bool VirtualFileSystem::createDirectory(std::string_view path)
{
    const auto result = vfs_.mkdir(path);
    if (!result) {
        std::cerr << "[vfs] createDirectory: " << result.error() << "\n";
        return false;
    }
    return true;
}

bool VirtualFileSystem::deleteFile(std::string_view path)
{
    const auto result = vfs_.remove(path);
    if (!result) {
        std::cerr << "[vfs] deleteFile: " << result.error() << "\n";
        return false;
    }
    return true;
}

std::string VirtualFileSystem::readFile(std::string_view path)
{
    // Silently returns "" on miss — callers use readFile() for existence probes.
    // Use vfs_.read_text() directly when distinguishing missing vs. error matters.
    const auto result = vfs_.read_text(path);
    return result ? *result : std::string{};
}

std::vector<std::string> VirtualFileSystem::listDirectory(std::string_view path)
{
    return vfs_.list(path);
}

bool VirtualFileSystem::exists(std::string_view path)
{
    return vfs_.exists(path);
}

} // namespace pce::sdlos
