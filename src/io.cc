// boot() → run() → shutdown() is the only valid lifecycle sequence.
// Calling run() without a successful boot() is a no-op (desktop will be empty).

#include "io.h"

#include <csignal>
#include <filesystem>
#include <iostream>
#include <random>
#include <chrono>

#include <SDL3/SDL.h>
#include <SDL3/SDL_video.h>


namespace pce::sdlos {

namespace {

// Only the atomic store is called from the signal handler — no heap, no I/O.
std::atomic<bool> g_quit_requested{false};

extern "C" void signal_handler(int) noexcept
{
    g_quit_requested.store(true);
}

} // anonymous namespace


/**
 * @brief IO
 */
IO::IO()
{
    // Prevent SDL from installing its own signal handlers so SIGINT/SIGTERM
    // reach our handler cleanly.
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
}


/**
 * @brief Boot
 *
 * @return true on success, false on failure
 */
bool IO::boot()
{
    if (running_.load()) {
        std::cerr << "[IO] boot() called while already running — ignored\n";
        return true;
    }

    // SDL_INIT_AUDIO is included so the IO shell can route audio events and
    // the vfs "asset" scheme can resolve audio files for the desktop shell.
    // Individual jade apps also call SDL_Init(SDL_INIT_AUDIO) in jade_host,
    // but SDL reference-counts subsystem init calls so this is safe.
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_CAMERA)) {
        std::cerr << "[IO] SDL_Init failed: " << SDL_GetError() << "\n";
        return false;
    }

    //  Signal handlers
    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    //  Virtual filesystem
    //
    // Scheme layout for the desktop shell:
    //
    //   user://   →  SDL_GetPrefPath("user", "sdlos")  (writable)
    //   asset://  →  SDL_GetBasePath()                 (read-only)
    //
    // Additional schemes ("scene://") are registered per-jade-app in
    // jade_host — they are not the IO shell's concern.
    // if (!vfs_.mount_platform_defaults("sdlos")) {
    // Errors are logged from inside Vfs; non-fatal — continue boot.
    // }
    vfs_.dump_mounts();

    //  Desktop window
    // In debug builds open a normal resizable window so tools like RenderDoc
    // can attach. In release builds go fullscreen.
    try {
#ifdef BUILD_TYPE_DEBUG
        desktop_.open("sdlos Desktop", 100, 100, 1280, 800);
#else
        desktop_.open("sdlos Desktop", 0, 0, 0, 0,
                      static_cast<SDL_WindowFlags>(SDL_WINDOW_FULLSCREEN));
#endif
    } catch (const std::exception& e) {
        std::cerr << "[IO] Failed to open desktop window: " << e.what() << "\n";
        SDL_Quit();
        return false;
    }

    notifications_.post(Notification::system("sdlos started"));

    std::cout << "[IO] booted\n";
    return true;
}


/**
 * @brief Run
 */
void IO::run()
{
    if (desktop_.empty()) {
        std::cerr << "[IO] run() called before a successful boot() — aborting\n";
        return;
    }

    running_.store(true);
    SDL_Event event;

    while (running_.load()) {
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running_.store(false);
                break;
            }
            desktop_.handleEvent(&event);
            handleSystemEvents();
        }

        if (g_quit_requested.load()) {
            running_.store(false);
            break;
        }

        tick();
        render();

        SDL_Delay(16); // ~60 fps cap; replace with proper frame timing later
    }
}

/**
 * @brief Shuts down
 */
void IO::shutdown()
{
    running_.store(false);

    for (auto& [wid, proc] : processes_) {
        proc->stop();
    }
    processes_.clear();   // dtors join threads

    cleanup();
    SDL_Quit();

    std::cout << "[IO] shut down\n";
}



/**
 * @brief Launch
 *
 * @param app_name  Human-readable name or identifier string
 *
 * @return Integer result; negative values indicate an error code
 */
int IO::launch(const std::string& app_name)
{
    // Removed: catalog lookup was an early launcher idea.
    // Callers now provide AppBundle directly.
    // Platform.Spawn is still a valid API for external launchers
    std::cerr << "[IO] launch: not implemented (use direct AppBundle instead)\n";
    return -1;
}


/**
 * @brief Terminate
 *
 * @param window_id  Unique object identifier
 */
void IO::terminate(int window_id)
{
    const auto it = processes_.find(window_id);
    if (it == processes_.end()) return;

    it->second->stop();
    processes_.erase(it);   // dtor joins the thread
    desktop_.close(window_id);

    std::cout << "[IO] terminated window_id=" << window_id << "\n";
}

// Background services
void IO::addService(std::function<void()> service)
{
    if (service) {
        services_.push_back(std::move(service));
    }
}

/**
 * @brief Handles system events
 */
void IO::handleSystemEvents()
{
    events_.publish("system_tick");
}

/**
 * @brief Ticks one simulation frame for
 */
void IO::tick()
{
    desktop_.tick();
    notifications_.tick();

    for (const auto& svc : services_) {
        svc();
    }

    reapFinished();
}

/**
 * @brief Renders
 */
void IO::render()
{
    desktop_.render();
    notifications_.render();
}

/**
 * @brief Reap finished
 */
void IO::reapFinished()
{
    // Collect finished window IDs first to avoid invalidating the iterator
    // while erasing from the map.
    std::vector<int> finished;
    for (const auto& [wid, proc] : processes_) {
        if (!proc->isRunning()) {
            finished.push_back(wid);
        }
    }

    for (const int wid : finished) {
        processes_.erase(wid);   // dtor joins thread
        desktop_.close(wid);
        std::cout << "[IO] reaped finished process (window_id=" << wid << ")\n";
    }
}

/**
 * @brief Cleanup
 */
void IO::cleanup()
{
    services_.clear();
}

} // namespace pce::sdlos
