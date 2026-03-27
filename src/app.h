#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace pce::sdlos {

struct AppBundle {
    std::string              name;
    std::string              jade_path;        // .jade source — primary launch format
    std::string              executable_path;  // native binary — future subprocess path
    std::string              icon_path;
    std::string              category;
    std::vector<std::string> dependencies;

    /// Valid when name is set and at least one launch path is provided.
    [[nodiscard]] bool valid() const
    {
        return !name.empty() && (!jade_path.empty() || !executable_path.empty());
    }

    [[nodiscard]] bool isJadeApp()   const noexcept { return !jade_path.empty();       }
    [[nodiscard]] bool isNativeApp() const noexcept { return !executable_path.empty(); }
};

// Lifecycle (driven by OS):
//   1. OS calls Desktop::open()     → gets window_id.
//   2. OS constructs Process(name, window_id).
//   3. OS calls process.start(task) → spawns execution thread.
//   4. Task finishes / stop() called → isRunning() returns false.
//   5. OS detects !isRunning()      → Desktop::close(window_id),
//                                     then destroys the Process (joins).
//
// The execution task receives no arguments; capture needed state by value at the call site.
class Process {
public:
    Process(std::string app_name, int window_id);

    // Joins the thread on destruction — safe even if start() was never called.
    ~Process();

    // Non-copyable: owns a std::thread.
    Process(const Process&)            = delete;
    Process& operator=(const Process&) = delete;

    /// Spawn the execution thread running `task`.
    /// No-op if the process is already running.
    void start(std::function<void()> task);

    /// Signal the thread to stop. Non-blocking.
    /// The execution task must observe isRunning() or use its own
    /// cancellation mechanism to exit in a timely manner.
    void stop();

    /// Block until the thread exits. Called implicitly by the destructor.
    void join();

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
