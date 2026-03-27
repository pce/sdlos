// App.cxx — Process execution context implementation.

#include "app.hh"

#include <iostream>

namespace pce::sdlos {

// ---------------------------------------------------------------------------
// Process
// ---------------------------------------------------------------------------

Process::Process(std::string app_name, int window_id)
    : name_(std::move(app_name)), window_id_(window_id)
{
}

Process::~Process()
{
    // Signal the task to stop, then wait for the thread to exit.
    // This guarantees the thread is never left running past the object's
    // lifetime regardless of how the caller cleans up.
    stop();
    join();
}

void Process::start(std::function<void()> task)
{
    if (running_.load()) {
        std::cerr << "[Process] '" << name_ << "' is already running — ignoring start()\n";
        return;
    }

    if (!task) {
        std::cerr << "[Process] '" << name_ << "' received a null task — ignoring start()\n";
        return;
    }

    running_.store(true);

    thread_ = std::thread([this, task = std::move(task)]() {
        std::cout << "[Process] '" << name_ << "' started  (window_id=" << window_id_ << ")\n";

        try {
            task();
        } catch (const std::exception& e) {
            std::cerr << "[Process] '" << name_ << "' threw: " << e.what() << "\n";
        } catch (...) {
            std::cerr << "[Process] '" << name_ << "' threw an unknown exception\n";
        }

        running_.store(false);
        std::cout << "[Process] '" << name_ << "' exited\n";
    });
}

void Process::stop()
{
    // Flip the flag so the running task can observe it via isRunning().
    // We do not forcibly terminate the thread — cooperative cancellation only.
    running_.store(false);
}

void Process::join()
{
    if (thread_.joinable()) {
        thread_.join();
    }
}

} // namespace pce::sdlos
