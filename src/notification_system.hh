#pragma once

// NotificationSystem.h — Notification value type and display queue.
//
// Two focused types:
//
//   Notification      — plain struct carrying one notification's data.
//                       Copyable, no virtual methods, no side-effects.
//                       Build one with the factory helpers below.
//
//   NotificationCenter — owns the pending and active queues.
//                        Single responsibility: move notifications from the
//                        pending queue into the active list, expire old ones,
//                        and render the active list to stdout (until a real
//                        on-screen overlay is implemented).

#include <chrono>
#include <functional>
#include <queue>
#include <string>
#include <vector>

namespace pce::sdlos {

// ---------------------------------------------------------------------------
// Notification — value type (plain struct, freely copyable)
// ---------------------------------------------------------------------------

struct Notification {
    std::string id;
    std::string title;
    std::string message;
    std::chrono::steady_clock::time_point timestamp;
    int  duration_secs{5};

    // Optional callbacks — set by the caller, not touched by NotificationCenter.
    std::function<void()> on_click;
    std::function<void()> on_close;

    // ---- Factory helpers -------------------------------------------------

    /// General-purpose notification.
    static Notification make(const std::string& title,
                             const std::string& message,
                             int duration_secs = 5);

    /// Convenience constructors for common severity levels.
    static Notification system (const std::string& message);
    static Notification error  (const std::string& message);
    static Notification success(const std::string& message);
};

// ---------------------------------------------------------------------------
// NotificationCenter — pending queue + active list + expiry
// ---------------------------------------------------------------------------

class NotificationCenter {
public:
    NotificationCenter();
    ~NotificationCenter() = default;

    // Non-copyable (owns mutable queues).
    NotificationCenter(const NotificationCenter&)            = delete;
    NotificationCenter& operator=(const NotificationCenter&) = delete;

    // ---- Post notifications ----------------------------------------------

    /// Enqueue a pre-built notification.
    void post(Notification notification);

    /// Convenience: build and enqueue in one call.
    void post(const std::string& title,
              const std::string& message,
              int duration_secs = 5);

    // ---- Dismiss ---------------------------------------------------------

    /// Remove an active notification by its ID. No-op if not found.
    void dismiss(const std::string& id);

    // ---- Main-loop hooks -------------------------------------------------

    /// Promote pending notifications into the active list (up to max_active)
    /// and expire notifications whose duration has elapsed.
    void tick();

    /// Write active notifications to stdout.
    /// Replace with a real GPU/ImGui overlay once the render pipeline is ready.
    void render() const;

    // ---- Queries ---------------------------------------------------------

    [[nodiscard]] bool        hasActive()    const { return !active_.empty();  }
    [[nodiscard]] std::size_t activeCount()  const { return active_.size();    }
    [[nodiscard]] std::size_t pendingCount() const { return pending_.size();   }

private:
    static constexpr std::size_t k_max_active{5};

    std::queue<Notification>  pending_;
    std::vector<Notification> active_;

    std::chrono::steady_clock::time_point last_tick_;

    // Generate a unique ID for each notification (simple counter).
    int next_id_{1};

    // Set by tick() whenever the active list changes; cleared by render().
    // mutable so render() can clear it despite being const.
    mutable bool dirty_{false};
};

} // namespace pce::sdlos
