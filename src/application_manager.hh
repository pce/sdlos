#pragma once

// ApplicationManager.h
// Application-level process management for pce::sdlos.
//
// Ownership model
// ---------------
// Application holds *only* application-level state. It does not own any SDL
// resource (SDL_Window, SDL_Renderer, SDL_GPUDevice, etc.). Those live
// exclusively inside the Window instances managed by WindowManager.
//
// When an Application is launched the OS allocates a Window via WindowManager
// and stores the resulting application-level window ID in `window_id`. All
// window manipulation (minimize, maximize, focus, close) is then performed by
// asking the WindowManager for that ID — never by touching SDL directly from
// Application code.
//
// Thread safety
// -------------
// `is_running` and `is_minimized` are std::atomic so the execution_thread can
// update them without a mutex. Everything else is written only from the main
// thread (launch / terminate / update).

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "i_application.hh"

namespace pce::sdlos {

// ---------------------------------------------------------------------------
// Application — process descriptor + execution context
//
// SDL resources are intentionally absent. The `window_id` field is the only
// bridge between an Application and its visible window; all native handle
// operations go through WindowManager::getWindow(window_id).
// ---------------------------------------------------------------------------
class Application : public IApplication {
public:
    // ---- Identity / metadata ---------------------------------------------

    std::string name;
    std::string executable_path;
    std::string icon_path;
    std::string category;
    std::vector<std::string> dependencies;

    // ---- Process state ---------------------------------------------------

    std::atomic<bool> is_running{false};
    std::atomic<bool> is_minimized{false};

    // Background thread that simulates / runs the application logic.
    // Always joined in terminate() before the object is destroyed.
    std::thread execution_thread;

    // ---- Window reference ------------------------------------------------

    // Application-level window ID assigned by WindowManager when the OS
    // creates a window for this application on launch.
    // -1 means no window has been assigned yet.
    int window_id{-1};

    // ---- Construction / destruction --------------------------------------

    Application(const std::string& name, const std::string& path);
    ~Application() override;

    // Non-copyable (owns a std::thread)
    Application(const Application&)            = delete;
    Application& operator=(const Application&) = delete;

    // ---- IApplication interface ------------------------------------------

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

// ---------------------------------------------------------------------------
// ApplicationManager — registry and lifecycle manager for Application objects
//
// Applications are registered by name + path. Launching an application starts
// its execution thread; the OS is responsible for creating the corresponding
// Window via WindowManager and setting app->window_id.
//
// running_apps_ maps window_id → Application for fast lookup when a window
// event needs to be forwarded to the owning application.
// ---------------------------------------------------------------------------
class ApplicationManager {
public:
    ApplicationManager()  = default;
    ~ApplicationManager() = default;

    // Non-copyable
    ApplicationManager(const ApplicationManager&)            = delete;
    ApplicationManager& operator=(const ApplicationManager&) = delete;

    // ---- Registration / launch -------------------------------------------

    /// Register an application by name. Returns false if the name is already
    /// registered or the path is empty.
    bool registerApplication(const std::string& name, const std::string& path);

    /// Launch a registered application.
    /// Returns the Application on success, nullptr on failure.
    /// The caller (typically OS) should create a Window afterwards and set
    /// app->window_id to wire the window to this application.
    std::shared_ptr<Application> launchApplication(const std::string& name);

    /// Terminate a running application by name.
    void terminateApplication(const std::string& name);

    // ---- Per-frame tick --------------------------------------------------

    /// Remove finished applications from the running_apps_ map.
    void update();

    // ---- Queries ---------------------------------------------------------

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
