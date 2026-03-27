// ApplicationManager.cxx
// Application process management implementation for pce::sdlos.
//
// Key design points (see ApplicationManager.h for full rationale):
//   - Application owns NO SDL resources. SDL_Window / GPU handles live
//     exclusively inside WindowManager's Window instances.
//   - window_id is the only link between an Application and its Window.
//   - execution_thread is always joined in terminate() so the object is
//     safe to destroy from any thread.

#include "application_manager.hh"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace pce::sdlos {

// ===========================================================================
// Application
// ===========================================================================

Application::Application(const std::string& name, const std::string& path)
    : name(name), executable_path(path)
{
    if (path.empty()) {
        throw std::runtime_error("Application path cannot be empty");
    }
}

Application::~Application()
{
    if (is_running.load()) {
        terminate();
    }
}

bool Application::launch()
{
    if (!std::filesystem::exists(executable_path)) {
        std::cerr << "[Application] Executable not found: " << executable_path << "\n";
        return false;
    }

    // Prevent double-launch with a compare-exchange on the atomic flag.
    bool expected = false;
    if (!is_running.compare_exchange_strong(expected, true)) {
        std::cerr << "[Application] Already running: " << name << "\n";
        return false;
    }

    execution_thread = std::thread([this]() {
        std::cout << "[Application] Started: " << name << "\n";

        // TODO: replace with real subprocess / plugin execution.
        std::this_thread::sleep_for(std::chrono::seconds(2));

        is_running.store(false);
        std::cout << "[Application] Exited: " << name << "\n";
    });

    return true;
}

void Application::terminate()
{
    // Signal the thread to stop (for real apps this would send a shutdown
    // message; here we just wait for the sleep to finish).
    is_running.store(false);

    if (execution_thread.joinable()) {
        execution_thread.join();
    }

    std::cout << "[Application] Terminated: " << name << "\n";
}

bool Application::isRunning() const
{
    return is_running.load();
}

std::string Application::getName() const
{
    return name;
}

std::string Application::getPath() const
{
    return executable_path;
}

// ===========================================================================
// ApplicationManager
// ===========================================================================

bool ApplicationManager::registerApplication(const std::string& name,
                                              const std::string& path)
{
    if (applications_.count(name)) {
        std::cerr << "[ApplicationManager] Already registered: " << name << "\n";
        return false;
    }

    try {
        applications_[name] = std::make_shared<Application>(name, path);
    } catch (const std::exception& e) {
        std::cerr << "[ApplicationManager] registerApplication failed: " << e.what() << "\n";
        return false;
    }

    return true;
}

std::shared_ptr<Application>
ApplicationManager::launchApplication(const std::string& name)
{
    auto it = applications_.find(name);
    if (it == applications_.end()) {
        std::cerr << "[ApplicationManager] Not registered: " << name << "\n";
        return nullptr;
    }

    auto& app = it->second;

    if (app->isRunning()) {
        // Already running — return the existing instance so the caller can
        // raise / focus its window if desired.
        return app;
    }

    if (!app->launch()) {
        std::cerr << "[ApplicationManager] launch() failed: " << name << "\n";
        return nullptr;
    }

    // The OS is responsible for calling WindowManager::createWindow() and
    // then setting app->window_id. We track by window_id once it is set.
    // If a window_id was already assigned (re-launch scenario), re-insert it.
    if (app->window_id >= 0) {
        running_apps_[app->window_id] = app;
    }

    return app;
}

void ApplicationManager::terminateApplication(const std::string& name)
{
    auto it = applications_.find(name);
    if (it == applications_.end()) {
        std::cerr << "[ApplicationManager] Not found: " << name << "\n";
        return;
    }

    auto& app = it->second;

    if (!app->isRunning()) return;

    // Remove from the running map before terminating so that update() does
    // not race with the erase below.
    if (app->window_id >= 0) {
        running_apps_.erase(app->window_id);
    }

    app->terminate();
}

void ApplicationManager::update()
{
    // Collect finished window IDs to avoid mutating the map during iteration.
    std::vector<int> to_remove;
    for (const auto& [wid, app] : running_apps_) {
        if (!app->isRunning()) {
            to_remove.push_back(wid);
        }
    }
    for (int wid : to_remove) {
        running_apps_.erase(wid);
    }
}

std::vector<std::string> ApplicationManager::getInstalledApps() const
{
    std::vector<std::string> names;
    names.reserve(applications_.size());
    for (const auto& [name, _] : applications_) {
        names.push_back(name);
    }
    return names;
}

std::shared_ptr<Application>
ApplicationManager::getApplication(const std::string& name) const
{
    auto it = applications_.find(name);
    return it != applications_.end() ? it->second : nullptr;
}

std::shared_ptr<Application>
ApplicationManager::getApplicationByWindowId(int window_id) const
{
    auto it = running_apps_.find(window_id);
    return it != running_apps_.end() ? it->second : nullptr;
}

} // namespace pce::sdlos
