#pragma once

#include "desktop.h"
#include "app.h"
#include "event_bus.h"
#include "notification_system.h"
#include "vfs/vfs.h"

#include <SDL3/SDL.h>

#include <atomic>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace pce::sdlos {

class IO {
public:
    /**
     * @brief IO
     */
    IO();
    /**
     * @brief ~io
     */
    ~IO() = default;

    /**
     * @brief IO
     *
     * @param param0  Red channel component [0, 1]
     */
    IO(const IO&)            = delete;
    IO& operator=(const IO&) = delete;


    /// Initialise SDL, create the desktop window, and wire up services.
    /// Returns false on unrecoverable failure (SDL init, no GPU device, …).
    bool boot();

    /// Run the main loop until the user quits or shutdown() is called.
    void run();

    /// Tear down all resources in safe reverse order, then call SDL_Quit().
    void shutdown();


    /// Register an AppBundle so it can be launched by name.
    /// Returns false on invalid bundle or duplicate name.
    bool install(AppBundle bundle);

    /// Launch a registered application by name.
    /// Returns the window_id on success, -1 on any failure.
    int launch(const std::string& app_name);

    /// Stop the Process associated with `window_id` and close its Window.
    /// Silently ignores unknown IDs.
    void terminate(int window_id);


    /// Register a callable invoked once per frame (after update, before render).
    void addService(std::function<void()> service);

    // Component accessors (for tooling / tests)
    /**
     * @brief Desktop
     *
     * @return Reference to the result
     */
     Desktop&            desktop()       { return desktop_;       }
    /**
     * @brief Events
     *
     * @return Reference to the result
     */
     EventBus&           events()        { return events_;        }
     /**
     * @brief Notifications
     *
     * @return Reference to the result
     */
     NotificationCenter& notifications() { return notifications_; }

    /**
     * @brief Vfs
     *
     * @return Reference to the result
     */
    pce::vfs::Vfs&       vfs() noexcept       { return vfs_; }
    /**
     * @brief Vfs
     *
     * @return Reference to the result
     */
    const pce::vfs::Vfs& vfs() const noexcept { return vfs_; }

private:
    //  Main-loop stages

    /**
     * @brief Handles system events
     */
    void handleSystemEvents();
    /**
     * @brief Ticks one simulation frame for
     */
    void tick();
    /**
     * @brief Renders
     */
    void render();

    /// Scan processes_ for finished entries; close their windows and remove
    /// them from the map. Called once per frame from tick().
    void reapFinished();

    /// Free application-level resources before SDL_Quit().
    void cleanup();

    // Components
    // Value members: construction/destruction order is determined by
    // declaration order (destroyed in reverse).
    Desktop            desktop_;
    EventBus           events_;
    NotificationCenter notifications_;

    // VFS — scheme-routed URI dispatcher.
    // Initialised in boot() once SDL_GetBasePath() / SDL_GetPrefPath() are available.
    pce::vfs::Vfs vfs_;

    //  Active processes
    // Only live (started) processes are stored here.
    std::unordered_map<int, std::unique_ptr<Process>> processes_;

    // Background services
    std::vector<std::function<void()>> services_;

    //  Run state
    std::atomic<bool> running_{false};
};

} // namespace pce::sdlos
