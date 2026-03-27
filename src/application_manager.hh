#pragma once

// Application holds only application-level state — no SDL resources.
// SDL_Window / GPU handles live exclusively in WindowManager's Window instances.
//
// Thread safety: is_running and is_minimized are std::atomic so the
// execution_thread can update them without a mutex. Everything else is
// written only from the main thread (launch / terminate / update).

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "i_application.hh"

namespace pce::sdlos {

// window_id is the only bridge between an Application and its visible window;
// all native handle operations go through WindowManager::getWindow(window_id).
class Application : public IApplication {
public:
    std::string name;
    std::string executable_path;
    std::string icon_path;
    std::string category;
    std::vector<std::string> dependencies;

    std::atomic<bool> is_running{false};
    std::atomic<bool> is_minimized{false};

    // Always joined in terminate() before the object is destroyed.
    std::thread execution_thread;

    // -1 means no window has been assigned yet.
    int window_id{-1};

    Application(const std::string& name, const std::string& path);
    ~Application() override;

    // Non-copyable (owns a std::thread)
    Application(const Application&)            = delete;
    Application& operator=(const Application&) = delete;

    /// Start the application's execution thread.
    /// Returns false if the executable path does not exist or the app is
    /// already running.
    bool launch() override;

    /// Signal the execution thread to stop and block until it exits.
    void terminate() override;

    bool        isRunning() const override;
    std::string getName()   const override;
    std::string getPath()   const override;
};

// running_apps_ maps window_id → Application for fast lookup when a window
// event needs to be forwarded to the owning application.
class ApplicationManager {
public:
    ApplicationManager()  = default;
    ~ApplicationManager() = default;

    ApplicationManager(const ApplicationManager&)            = delete;
    ApplicationManager& operator=(const ApplicationManager&) = delete;

    /// Register an application by name. Returns false if the name is already
    /// registered or the path is empty.
    bool registerApplication(const std::string& name, const std::string& path);

    /// Launch a registered application.
    /// Returns the Application on success, nullptr on failure.
    /// The caller (typically OS) should create a Window afterwards and set
    /// app->window_id to wire the window to this application.
    std::shared_ptr<Application> launchApplication(const std::string& name);

    void terminateApplication(const std::string& name);

    void update();

    std::vector<std::string>     getInstalledApps() const;
    std::shared_ptr<Application> getApplication(const std::string& name) const;

    /// Look up the application that owns the given window ID.
    /// Returns nullptr if no application is associated with that window.
    std::shared_ptr<Application> getApplicationByWindowId(int window_id) const;

private:
    // name → Application (all registered apps, running or not)
    std::unordered_map<std::string, std::shared_ptr<Application>> applications_;

    // window_id → Application (only apps that have been assigned a window)
    std::unordered_map<int, std::shared_ptr<Application>> running_apps_;
};

} // namespace pce::sdlos
