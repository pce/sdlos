#pragma once

// Launching is the OS's job:
//   1. OS calls catalog.find(name)    → gets the AppBundle descriptor.
//   2. OS calls desktop.open(...)     → gets a window_id.
//   3. OS constructs a Process and calls process.start(task).

#include "app.hh"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace pce::sdlos {

class AppCatalog {
public:
    AppCatalog()  = default;
    ~AppCatalog() = default;

    // Non-copyable: owns the authoritative bundle set.
    AppCatalog(const AppCatalog&)            = delete;
    AppCatalog& operator=(const AppCatalog&) = delete;

    /// Register a bundle. Returns false when:
    ///   • bundle.valid() is false (name or executable_path is empty), or
    ///   • a bundle with the same name is already installed.
    bool install(AppBundle bundle);

    /// Remove a bundle by name. Returns false if the name is not found.
    bool uninstall(const std::string& name);

    /// Return a pointer to the installed bundle, or nullptr if not found.
    /// The pointer is invalidated by any subsequent call to install() or
    /// uninstall() — do not store it across mutations.
    [[nodiscard]] const AppBundle* find(const std::string& name) const;

    [[nodiscard]] bool contains(const std::string& name) const;

    /// Names of all currently installed bundles (order unspecified).
    [[nodiscard]] std::vector<std::string> installedNames() const;

    [[nodiscard]] std::size_t size()  const { return bundles_.size();  }
    [[nodiscard]] bool        empty() const { return bundles_.empty(); }

private:
    std::unordered_map<std::string, AppBundle> bundles_;
};

} // namespace pce::sdlos
