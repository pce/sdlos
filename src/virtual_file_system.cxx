#include "virtual_file_system.hh"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <mutex>

namespace pce::sdlos {

VirtualFileSystem::VirtualFileSystem(const std::string& root) : root_path(root) {
    if (!std::filesystem::exists(root_path)) {
        std::filesystem::create_directories(root_path);
    }
}

bool VirtualFileSystem::isPathSafe(const std::string& path) const {
    if (path.empty() || path.find("..") != std::string::npos) {
        return false;
    }

    std::filesystem::path real_path = std::filesystem::absolute(path);
    std::filesystem::path root = std::filesystem::absolute(root_path);

    return real_path.string().find(root.string()) == 0;
}

std::string VirtualFileSystem::getRealPath(const std::string& virtual_path) const {
    std::lock_guard<std::mutex> lock(fs_mutex);
    if (!isPathSafe(virtual_path)) {
        throw std::runtime_error("Invalid path: " + virtual_path);
    }
    return root_path + "/" + virtual_path;
}

bool VirtualFileSystem::createFile(const std::string& path, const std::string& content) {
    try {
        std::lock_guard<std::mutex> lock(fs_mutex);
        std::filesystem::path full_path = getRealPath(path);
        std::filesystem::create_directories(full_path.parent_path());

        std::ofstream file(full_path);
        if (file.is_open()) {
            file << content;
            file.close();
            return true;
        }
        return false;
    } catch (const std::exception&) {
        return false;
    }
}

bool VirtualFileSystem::createDirectory(const std::string& path) {
    try {
        std::lock_guard<std::mutex> lock(fs_mutex);
        std::filesystem::create_directories(getRealPath(path));
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

std::string VirtualFileSystem::readFile(const std::string& path) {
    try {
        std::lock_guard<std::mutex> lock(fs_mutex);
        std::ifstream file(getRealPath(path));
        if (file.is_open()) {
            std::stringstream buffer;
            buffer << file.rdbuf();
            file.close();
            return buffer.str();
        }
        return "";
    } catch (const std::exception&) {
        return "";
    }
}

bool VirtualFileSystem::deleteFile(const std::string& path) {
    try {
        std::lock_guard<std::mutex> lock(fs_mutex);
        return std::filesystem::remove(getRealPath(path));
    } catch (const std::exception&) {
        return false;
    }
}

bool VirtualFileSystem::exists(const std::string& path) {
    try {
        std::lock_guard<std::mutex> lock(fs_mutex);
        return std::filesystem::exists(getRealPath(path));
    } catch (const std::exception&) {
        return false;
    }
}

std::vector<std::string> VirtualFileSystem::listDirectory(const std::string& path) {
    std::vector<std::string> result;
    try {
        std::lock_guard<std::mutex> lock(fs_mutex);
        for (const auto& entry : std::filesystem::directory_iterator(getRealPath(path))) {
            result.push_back(entry.path().filename().string());
        }
    } catch (const std::exception&) {
        // Return empty vector on error
    }
    return result;
}

} // namespace pce::sdlos
