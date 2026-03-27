// boot() → run() → shutdown() is the only valid lifecycle sequence.
// Calling run() without a successful boot() is a no-op (desktop will be empty).

#include "os.hh"

#include <csignal>
#include <filesystem>
#include <iostream>
#include <random>
#include <chrono>

#include <SDL3/SDL.h>
#include <SDL3/SDL_video.h>

#include "virtual_file_system.hh"

namespace pce::sdlos {

namespace {

// Only the atomic store is called from the signal handler — no heap, no I/O.
std::atomic<bool> g_quit_requested{false};

extern "C" void signal_handler(int) noexcept
{
    g_quit_requested.store(true);
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

OS::OS()
{
    // Prevent SDL from installing its own signal handlers so SIGINT/SIGTERM
    // reach our handler cleanly.
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
}

// ---------------------------------------------------------------------------
// boot
// ---------------------------------------------------------------------------

bool OS::boot()
{
    if (running_.load()) {
        std::cerr << "[OS] boot() called while already running — ignored\n";
        return true;
    }

    // ---- SDL init --------------------------------------------------------
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::cerr << "[OS] SDL_Init failed: " << SDL_GetError() << "\n";
        return false;
    }

    // ---- Signal handlers -------------------------------------------------
    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    // ---- Virtual filesystem ----------------------------------------------
    fs_ = std::make_unique<VirtualFileSystem>("/tmp/sdlos");

    // ---- Desktop window --------------------------------------------------
    // In debug builds open a normal resizable window so tools like RenderDoc
    // can attach. In release builds go fullscreen.
    try {
#ifdef BUILD_TYPE_DEBUG
        desktop_.open("sdlos Desktop", 100, 100, 1280, 800);
#else
        desktop_.open("sdlos Desktop", 0, 0, 0, 0,
                      static_cast<SDL_WindowFlags>(SDL_WINDOW_FULLSCREEN_DESKTOP));
#endif
    } catch (const std::exception& e) {
        std::cerr << "[OS] Failed to open desktop window: " << e.what() << "\n";
        SDL_Quit();
        return false;
    }

    notifications_.post(Notification::system("sdlos started"));

    std::cout << "[OS] booted\n";
    return true;
}

// ---------------------------------------------------------------------------
// run
// ---------------------------------------------------------------------------

void OS::run()
{
    if (desktop_.empty()) {
        std::cerr << "[OS] run() called before a successful boot() — aborting\n";
        return;
    }

    running_.store(true);
    SDL_Event event;

    while (running_.load()) {
        // ---- Event pump --------------------------------------------------
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

// ---------------------------------------------------------------------------
// shutdown
// ---------------------------------------------------------------------------

void OS::shutdown()
{
    running_.store(false);

    for (auto& [wid, proc] : processes_) {
        proc->stop();
    }
    processes_.clear();   // dtors join threads

    cleanup();
    SDL_Quit();

    std::cout << "[OS] shut down\n";
}

// ---------------------------------------------------------------------------
// Application API
// ---------------------------------------------------------------------------

bool OS::install(AppBundle bundle)
{
    return catalog_.install(std::move(bundle));
}

int OS::launch(const std::string& app_name)
{
    const AppBundle* bundle = catalog_.find(app_name);
    if (!bundle) {
        std::cerr << "[OS] launch: '" << app_name << "' is not installed\n";
        return -1;
    }

    if (!std::filesystem::exists(bundle->executable_path)) {
        std::cerr << "[OS] launch: executable not found: "
                  << bundle->executable_path << "\n";
        return -1;
    }

    int window_id = -1;
    try {
        window_id = desktop_.open(
            bundle->name,
            /* x */ 200, /* y */ 150,
            /* w */ 800, /* h */ 600);
    } catch (const std::exception& e) {
        std::cerr << "[OS] launch: failed to open window for '"
                  << app_name << "': " << e.what() << "\n";
        return -1;
    }

    auto proc = std::make_unique<Process>(bundle->name, window_id);

    // Capture by value so the task is safe to run on a background thread
    // without touching the catalog or desktop directly.
    const std::string name = bundle->name;
    const std::string path = bundle->executable_path;

    proc->start([name, path]() {
        std::cout << "[Process] '" << name << "' executing: " << path << "\n";
        // TODO: replace with real subprocess / plugin execution.
        std::this_thread::sleep_for(std::chrono::seconds(5));
    });

    processes_.emplace(window_id, std::move(proc));

    notifications_.post(Notification::system("Launched: " + app_name));
    std::cout << "[OS] launched '" << app_name
              << "' → window_id=" << window_id << "\n";

    return window_id;
}

void OS::terminate(int window_id)
{
    const auto it = processes_.find(window_id);
    if (it == processes_.end()) return;

    it->second->stop();
    processes_.erase(it);   // dtor joins the thread
    desktop_.close(window_id);

    std::cout << "[OS] terminated window_id=" << window_id << "\n";
}

// ---------------------------------------------------------------------------
// Background services
// ---------------------------------------------------------------------------

void OS::addService(std::function<void()> service)
{
    if (service) {
        services_.push_back(std::move(service));
    }
}

// ---------------------------------------------------------------------------
// Main-loop stages (private)
// ---------------------------------------------------------------------------

void OS::handleSystemEvents()
{
    events_.publish("system_tick");
}

void OS::tick()
{
    desktop_.tick();
    notifications_.tick();

    for (const auto& svc : services_) {
        svc();
    }

    reapFinished();
}

void OS::render()
{
    desktop_.render();
    notifications_.render();
}

void OS::reapFinished()
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
        std::cout << "[OS] reaped finished process (window_id=" << wid << ")\n";
    }
}

void OS::cleanup()
{
    services_.clear();
    fs_.reset();
}

} // namespace pce::sdlos
