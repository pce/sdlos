#pragma once
#include <unordered_map>
#include <string>
#include <vector>
#include <memory>
#include <filesystem>
#include <mutex>
#include <ctime>
#include <cstdint>
#include "i_file_system.hh"

namespace pce::sdlos {

struct VirtualFile {
    std::string name;
    std::string content;
    std::string type; // "file", "directory", "link"
    uint64_t size = 0;
    std::time_t modified_time = 0;

    explicit VirtualFile(const std::string& name, const std::string& content = "");
    virtual ~VirtualFile() = default;
};

class VirtualFileSystem : public IFileSystem {
private:
    std::unordered_map<std::string, std::shared_ptr<VirtualFile>> files;
    std::string root_path;
    mutable std::mutex fs_mutex;

public:
    explicit VirtualFileSystem(const std::string& root = "/");
    ~VirtualFileSystem() = default;

    // Extra helper API
    bool mount(const std::string& path, const std::string& virtual_path);

    // IFileSystem implementations
    bool createFile(const std::string& path, const std::string& content = "") override;
    bool createDirectory(const std::string& path) override;
    bool deleteFile(const std::string& path) override;
    std::string readFile(const std::string& path) override;
    std::vector<std::string> listDirectory(const std::string& path) override;
    bool exists(const std::string& path) override;

    // Utilities
    bool isPathSafe(const std::string& path) const;
    std::string getRealPath(const std::string& virtual_path) const;
};

} // namespace pce::sdlos
