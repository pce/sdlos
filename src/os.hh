#pragma once

#include "desktop.hh"
#include "app_catalog.hh"
#include "app.hh"
#include "event_bus.hh"
#include "virtual_file_system.hh"
#include "notification_system.hh"
#include "i_file_system.hh"
#include "vfs/vfs.hh"

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
    /// Returns false on invalid bundle or duplicate name.
    bool install(AppBundle bundle);

    /// Launch a registered application by name.
    /// Returns the window_id on success, -1 on any failure.
    int launch(const std::string& app_name);

    /// Stop the Process associated with `window_id` and close its Window.
    /// Silently ignores unknown IDs.
    void terminate(int window_id);

    // ---- Background services ---------------------------------------------

    /// Register a callable invoked once per frame (after update, before render).
    void addService(std::function<void()> service);

    // ---- Component accessors (for tooling / tests) -----------------------

    [[nodiscard]] Desktop&            desktop()       { return desktop_;       }
    [[nodiscard]] AppCatalog&         catalog()       { return catalog_;       }
    [[nodiscard]] EventBus&           events()        { return events_;        }
    [[nodiscard]] NotificationCenter& notifications() { return notifications_; }

    /// Legacy IFileSystem interface — delegates to the "tmp" LocalMount.
    ///
    /// Prefer vfs() for new code — it provides scheme routing, binary IO,
    /// and std::expected<T,E> error propagation.
    [[nodiscard]] IFileSystem& filesystem() noexcept { return *fs_; }

    /// Direct access to the underlying Vfs for scheme-routed URI IO.
    ///
    ///   os.vfs().read("scene://assets/audio/t-rex-roar.wav")
    ///   os.vfs().write_text("tmp://log/session.txt", entry)
    ///   os.vfs().mount_local("scene", jade_dir)
    [[nodiscard]] pce::vfs::Vfs&       vfs() noexcept       { return fs_->vfs(); }
    [[nodiscard]] const pce::vfs::Vfs& vfs() const noexcept { return fs_->vfs(); }

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
    // Value members: construction/destruction order is determined by
    // declaration order (destroyed in reverse).

    Desktop            desktop_;
    AppCatalog         catalog_;
    EventBus           events_;
    NotificationCenter notifications_;

    // VirtualFileSystem requires a root path — heap-allocated and
    // initialised in boot() once we know the path.
    // Stored as VirtualFileSystem (not IFileSystem) so vfs() can reach
    // the underlying Vfs for scheme-routed IO without a static_cast.
    std::unique_ptr<VirtualFileSystem> fs_;

    // ---- Active processes ------------------------------------------------
    // Only live (started) processes are stored here.
    std::unordered_map<int, std::unique_ptr<Process>> processes_;

    // ---- Background services ---------------------------------------------
    std::vector<std::function<void()>> services_;

    // ---- Run state -------------------------------------------------------
    std::atomic<bool> running_{false};
};

} // namespace pce::sdlos
