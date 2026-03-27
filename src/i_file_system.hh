#pragma once


namespace pce::sdlos {

    class IFileSystem {
    public:
        virtual ~IFileSystem() = default;
        virtual bool createFile(const std::string& path, const std::string& content) = 0;
        virtual bool createDirectory(const std::string& path) = 0;
        virtual std::string readFile(const std::string& path) = 0;
        virtual bool deleteFile(const std::string& path) = 0;
        virtual bool exists(const std::string& path) = 0;
        virtual std::vector<std::string> listDirectory(const std::string& path) = 0;
    };
}
