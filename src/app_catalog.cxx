#include "app_catalog.hh"

#include <iostream>

namespace pce::sdlos {

bool AppCatalog::install(AppBundle bundle)
{
    if (!bundle.valid()) {
        std::cerr << "[AppCatalog] install: bundle '" << bundle.name
                  << "' is not valid (name or executable_path is empty)\n";
        return false;
    }

    if (bundles_.count(bundle.name)) {
        std::cerr << "[AppCatalog] install: '" << bundle.name
                  << "' is already installed\n";
        return false;
    }

    const std::string name = bundle.name;          // copy before move
    bundles_.emplace(name, std::move(bundle));
    std::cout << "[AppCatalog] installed: '" << name << "'\n";
    return true;
}

bool AppCatalog::uninstall(const std::string& name)
{
    const auto erased = bundles_.erase(name);
    if (erased == 0) {
        std::cerr << "[AppCatalog] uninstall: '" << name << "' not found\n";
        return false;
    }

    std::cout << "[AppCatalog] uninstalled: '" << name << "'\n";
    return true;
}

const AppBundle* AppCatalog::find(const std::string& name) const
{
    const auto it = bundles_.find(name);
    return it != bundles_.end() ? &it->second : nullptr;
}

bool AppCatalog::contains(const std::string& name) const
{
    return bundles_.count(name) > 0;
}

std::vector<std::string> AppCatalog::installedNames() const
{
    std::vector<std::string> names;
    names.reserve(bundles_.size());
    for (const auto& [name, _] : bundles_) {
        names.push_back(name);
    }
    return names;
}

} // namespace pce::sdlos
