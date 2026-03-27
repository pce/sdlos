#pragma once

// App.h — Application descriptor and execution context.
//
// Two focused types replace the old Application God Object:
//
//   AppBundle — plain value type carrying app metadata only.
//               Copyable. No threads, no SDL, no side-effects.
//               Think of it as the entry in an app catalogue.
//
//   Process   — owns the execution thread for one running app instance.
//               Non-copyable. Created by the OS after a Window has been
//               opened for the app. Destroyed (joining the thread) when
//               the app exits or is terminated.
//               The window_id stored here is the application-level ID
//               returned by Desktop::open(); the OS uses it to close the
//               window when the process finishes.
//
// Neither type knows about SDL. All native window/GPU handles live inside
// the Window instances managed by Desktop.

#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace pce::sdlos {

// ---------------------------------------------------------------------------
// AppBundle — installed application descriptor (value type)
//
// Intentionally a plain struct so it can be freely copied, stored in
// containers, and serialised without worrying about ownership semantics.
// ---------------------------------------------------------------------------
struct AppBundle {
    std::string              name;
    std::string              executable_path;
    std::string              icon_path;
    std::string              category;
    std::vector<std::string> dependencies;

    /// Returns true when the bundle has the minimum fields required to launch.
    [[nodiscard]] bool valid() const
    {
        return !name.empty() && !executable_path.empty();
    }
};

// ---------------------------------------------------------------------------
// Process — one running instance of an AppBundle
//
// Lifecycle (driven by OS):
//   1. OS calls Desktop::open()     → gets window_id.
//   2. OS constructs Process(name, window_id).
//   3. OS calls process.start(task) → spawns execution thread.
//   4. Task finishes / stop() called → isRunning() returns false.
//   5. OS detects !isRunning()      → Desktop::close(window_id),
//                                     then destroys the Process (joins).
//
// The execution task receives no arguments. If it needs to communicate with
// the outside world it should capture what it needs by value at the call site.
// ---------------------------------------------------------------------------
class Process {
public:
    Process(std::string app_name, int window_id);

    // Joins the thread on destruction — safe even if start() was never called.
    ~Process();

    // Non-copyable: owns a std::thread.
    Process(const Process&)            = delete;
    Process& operator=(const Process&) = delete;

    // ---- Execution -------------------------------------------------------

    /// Spawn the execution thread running `task`.
    /// No-op if the process is already running.
    void start(std::function<void()> task);

    /// Signal the thread to stop. Non-blocking.
    /// The execution task must observe isRunning() or use its own
    /// cancellation mechanism to exit in a timely manner.
    void stop();

    /// Block until the thread exits. Called implicitly by the destructor.
    void join();

    // ---- Queries ---------------------------------------------------------

    [[nodiscard]] bool               isRunning() const { return running_.load(); }
    [[nodiscard]] int                windowId()  const { return window_id_;      }
    [[nodiscard]] const std::string& appName()   const { return name_;           }

private:
    std::string       name_;
    int               window_id_;
    std::atomic<bool> running_{false};
    std::thread       thread_;
};

} // namespace pce::sdlos
