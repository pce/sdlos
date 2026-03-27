#pragma once

// OS.h — Top-level system coordinator.
//
// OS owns all first-class system components and coordinates their interactions.
// It is the only place where Desktop, AppCatalog, and Process are wired
// together — none of those types know about each other.
//
// Responsibilities
// ----------------
//   boot()      — Init SDL, create the desktop window, start services.
//   run()       — Drive the main event/update/render loop.
//   shutdown()  — Tear down components in reverse-construction order, quit SDL.
//   install()   — Register an AppBundle in the catalog.
//   launch()    — Look up a bundle, open a Window, start a Process.
//   terminate() — Stop the Process and close its Window by window_id.
//
// What OS is NOT
// --------------
//   • It does not own SDL_Window or SDL_GPUDevice — those live inside Window.
//   • It does not know how shaders work — that is SDLRenderer's job.
//   • It does not build components via a factory — it constructs them directly.

#include "desktop.hh"
#include "app_catalog.hh"
#include "app.hh"
#include "event_bus.hh"
#include "virtual_file_system.hh"
#include "notification_system.hh"
#include "i_file_system.hh"

#include <SDL3/SDL.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace pce::sdlos {

class OS {
public:
    OS();
    ~OS() = default;

    // Non-copyable: owns unique system state.
    OS(const OS&)            = delete;
    OS& operator=(const OS&) = delete;

    // ---- Lifecycle -------------------------------------------------------

    /// Initialise SDL, create the desktop window, and wire up services.
    /// Returns false on unrecoverable failure (SDL init, no GPU device, …).
    bool boot();

    /// Run the main loop until the user quits or shutdown() is called.
    void run();

    /// Tear down all resources in safe reverse order, then call SDL_Quit().
    void shutdown();

    // ---- Application API -------------------------------------------------

    /// Register an AppBundle so it can be launched by name.
    /// Forwards to catalog_.install(); returns false on invalid bundle or
    /// duplicate name.
    bool install(AppBundle bundle);

    /// Launch a registered application:
    ///   1. Looks up the bundle in catalog_.
    ///   2. Opens a Window via desktop_.
    ///   3. Constructs a Process and calls start() on it.
    /// Returns the window_id on success, -1 on any failure.
    int launch(const std::string& app_name);

    /// Stop the Process associated with `window_id` and close its Window.
    /// Silently ignores unknown IDs.
    void terminate(int window_id);

    // ---- Background services ---------------------------------------------

    /// Register a zero-argument callable that will be called once per frame
    /// during the main loop (after update, before render).
    void addService(std::function<void()> service);

    // ---- Component accessors (for tooling / tests) -----------------------

    [[nodiscard]] Desktop&          desktop()    { return desktop_;    }
    [[nodiscard]] AppCatalog&       catalog()    { return catalog_;    }
    [[nodiscard]] EventBus&         events()     { return events_;     }
    [[nodiscard]] IFileSystem&      filesystem() { return *fs_;        }
    [[nodiscard]] NotificationCenter& notifications() { return notifications_; }

private:
    // ---- Main-loop stages ------------------------------------------------

    void handleSystemEvents();
    void tick();
    void render();

    /// Scan processes_ for finished entries; close their windows and remove
    /// them from the map. Called once per frame from tick().
    void reapFinished();

    /// Free application-level resources before SDL_Quit().
    void cleanup();

    // ---- Components ------------------------------------------------------
    // Value members where possible so construction/destruction order is
    // determined by declaration order (destroyed in reverse).

    Desktop            desktop_;
    AppCatalog         catalog_;
    EventBus           events_;
    NotificationCenter notifications_;

    // VirtualFileSystem requires a root path so it is heap-allocated and
    // initialised in boot() once we know the path.
    std::unique_ptr<IFileSystem> fs_;

    // ---- Active processes ------------------------------------------------
    // Keyed by window_id for O(1) lookup from both window events and the
    // reaper. Only live (started) processes are stored here.
    std::unordered_map<int, std::unique_ptr<Process>> processes_;

    // ---- Background services ---------------------------------------------
    std::vector<std::function<void()>> services_;

    // ---- Run state -------------------------------------------------------
    std::atomic<bool> running_{false};
};

} // namespace pce::sdlos
