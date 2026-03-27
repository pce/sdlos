#include "notification_system.hh"

#include <algorithm>
#include <iostream>
#include <sstream>

namespace pce::sdlos {

// ===========================================================================
// Notification — factory helpers
// ===========================================================================

Notification Notification::make(const std::string& title,
                                 const std::string& message,
                                 int duration_secs)
{
    Notification n;
    // ID is assigned by NotificationCenter::post(); leave empty here so the
    // struct remains a plain value type with no external dependencies.
    n.title        = title;
    n.message      = message;
    n.timestamp    = std::chrono::steady_clock::now();
    n.duration_secs = duration_secs;
    return n;
}

Notification Notification::system(const std::string& message)
{
    return make("System", message);
}

Notification Notification::error(const std::string& message)
{
    return make("Error", message);
}

Notification Notification::success(const std::string& message)
{
    return make("Success", message);
}

// ===========================================================================
// NotificationCenter
// ===========================================================================

NotificationCenter::NotificationCenter()
    : last_tick_(std::chrono::steady_clock::now())
{
}

// ---------------------------------------------------------------------------
// Post
// ---------------------------------------------------------------------------

void NotificationCenter::post(Notification notification)
{
    // Assign a unique ID if the caller left it empty.
    if (notification.id.empty()) {
        std::ostringstream ss;
        ss << "notif-" << next_id_++;
        notification.id = ss.str();
    }

    // Make sure the timestamp reflects when the notification was actually
    // submitted rather than when the struct was constructed.
    notification.timestamp = std::chrono::steady_clock::now();

    pending_.push(std::move(notification));
}

void NotificationCenter::post(const std::string& title,
                               const std::string& message,
                               int duration_secs)
{
    post(Notification::make(title, message, duration_secs));
}

// ---------------------------------------------------------------------------
// Dismiss
// ---------------------------------------------------------------------------

void NotificationCenter::dismiss(const std::string& id)
{
    const auto it = std::remove_if(
        active_.begin(), active_.end(),
        [&id](const Notification& n) { return n.id == id; });

    if (it != active_.end()) {
        // Fire the on_close callback if one was set.
        if (it->on_close) {
            it->on_close();
        }
        active_.erase(it, active_.end());
    }
}

// ---------------------------------------------------------------------------
// Main-loop hooks
// ---------------------------------------------------------------------------

void NotificationCenter::tick()
{
    const auto now = std::chrono::steady_clock::now();

    // Throttle to once per second — fine-grained enough for notification UX
    // without spending time in remove_if on every frame.
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::seconds>(now - last_tick_).count();

    if (elapsed < 1) return;
    last_tick_ = now;

    // --- Expire active notifications whose duration has elapsed ------------
    const auto expired = std::remove_if(
        active_.begin(), active_.end(),
        [&now](const Notification& n) {
            const auto age =
                std::chrono::duration_cast<std::chrono::seconds>(
                    now - n.timestamp).count();
            return age >= n.duration_secs;
        });

    for (auto it = expired; it != active_.end(); ++it) {
        if (it->on_close) it->on_close();
    }
    active_.erase(expired, active_.end());

    // --- Promote pending notifications into the active list ----------------
    while (!pending_.empty() && active_.size() < k_max_active) {
        active_.push_back(std::move(pending_.front()));
        pending_.pop();
    }
}

void NotificationCenter::render() const
{
    // Stub: write to stdout until a real on-screen overlay (ImGui / GPU quad)
    // is wired up. Each call prints only the active list so the output stays
    // stable between ticks.
    for (const auto& n : active_) {
        std::cout << "[Notification] [" << n.id << "] "
                  << n.title << ": " << n.message << "\n";
    }
}

} // namespace pce::sdlos
