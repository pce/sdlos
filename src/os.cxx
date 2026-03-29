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
    // SDL_INIT_AUDIO is included so the OS shell can route audio events and
    // the vfs "asset" scheme can resolve audio files for the desktop shell.
    // Individual jade apps also call SDL_Init(SDL_INIT_AUDIO) in jade_host,
    // but SDL reference-counts subsystem init calls so this is safe.
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_CAMERA)) {
        std::cerr << "[OS] SDL_Init failed: " << SDL_GetError() << "\n";
        return false;
    }

    // ---- Signal handlers -------------------------------------------------
    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    // ---- Virtual filesystem ----------------------------------------------
    //
    // Scheme layout for the desktop shell:
    //
    //   user://   →  SDL_GetPrefPath("pce", "sdlos")
    //                Platform-correct writable user data directory.
    //                  macOS:   ~/Library/Application Support/pce/sdlos/
    //                  Linux:   ~/.local/share/sdlos/
    //                  Windows: %APPDATA%\pce\sdlos\
    //                Default scheme: bare IFileSystem paths resolve here.
    //
    //   asset://  →  SDL_GetBasePath()
    //                Read-only directory of the running binary.
    //                Shaders, fonts, bundled assets.  Mounted as read-only
    //                so accidental writes surface as errors rather than
    //                silently landing next to the binary.
    //
    // Callers that have migrated to the URI API use os.vfs() directly:
    //
    //   os.vfs().read_text("asset://shaders/desktop.frag.metal")
    //   os.vfs().write_text("user://logs/session.txt", entry)
    //
    // Legacy IFileSystem callers (readFile / createFile / …) continue to
    // work via the "user" default scheme without any changes.
    //
    // Additional schemes ("scene://") are registered per-jade-app in
    // jade_host — they are not the OS shell's concern.
    fs_ = std::make_unique<VirtualFileSystem>();

    // user:// — platform user-data directory (writable).
    // SDL_GetPrefPath creates the directory if needed and returns a path with
    // a trailing separator, or nullptr on platforms that don't support it.
    {
        const char* pref = SDL_GetPrefPath("pce", "sdlos");
        if (pref && pref[0] != '\0') {
            fs_->mount("user", std::filesystem::path(pref));
            fs_->vfs().set_default_scheme("user");
            std::cout << "[OS] vfs: user://   →  " << pref << "\n";
            SDL_free(const_cast<char*>(pref));
        } else {
            std::cerr << "[OS] vfs: SDL_GetPrefPath() unavailable — "
                         "'user://' scheme not registered, IFileSystem will fail\n";
        }
    }

    // asset:// — binary base path (read-only: installed assets, shaders).
    // SDL_GetBasePath() returns the directory of the running binary with a
    // trailing separator, or nullptr when indeterminate (some embedded targets).
    {
        const char* base = SDL_GetBasePath();
        if (base && base[0] != '\0') {
            // Read-only mount: asset files must not be mutated at runtime.
            // A UnionMount could layer DLC on top here in future.
            fs_->mount("asset", std::filesystem::path(base));
            std::cout << "[OS] vfs: asset://  →  " << base << "\n";
        } else {
            std::cerr << "[OS] vfs: SDL_GetBasePath() unavailable — "
                         "'asset://' scheme not registered\n";
        }
    }

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
