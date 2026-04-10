#include "app.h"

#include <iostream>

namespace pce::sdlos {

/**
 * @brief Processes
 *
 * @param app_name   Human-readable name or identifier string
 * @param window_id  Unique object identifier
 */
Process::Process(std::string app_name, int window_id)
    : name_(std::move(app_name))
    , window_id_(window_id) {}

/**
 * @brief ~process
 */
Process::~Process() {
    // Signal the task to stop, then wait for the thread to exit.
    // Guarantees the thread is never left running past the object's lifetime
    // regardless of how the caller cleans up.
    stop();
    join();
}

void Process::start(std::function<void()> task) {
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
        } catch (const std::exception &e) {
            std::cerr << "[Process] '" << name_ << "' threw: " << e.what() << "\n";
        } catch (...) {
            std::cerr << "[Process] '" << name_ << "' threw an unknown exception\n";
        }

        running_.store(false);
        std::cout << "[Process] '" << name_ << "' exited\n";
    });
}

/**
 * @brief Stop
 */
void Process::stop() {
    // Cooperative cancellation only — we do not forcibly terminate the thread.
    // The running task must observe isRunning() to exit in a timely manner.
    running_.store(false);
}

/**
 * @brief Join
 */
void Process::join() {
    if (thread_.joinable()) {
        thread_.join();
    }
}

}  // namespace pce::sdlos
