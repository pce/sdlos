#include "event_bus.hh"
#include <algorithm>

namespace pce::sdlos {

EventBus::EventBus() = default;

void EventBus::subscribe(const std::string& event_type, std::function<void(const std::string&)> callback) {
    std::unique_lock<std::shared_mutex> lock(mutex);
    subscribers[event_type].push_back(std::move(callback));
}

void EventBus::unsubscribe(const std::string& event_type, std::function<void(const std::string&)> callback) {
    std::unique_lock<std::shared_mutex> lock(mutex);
    auto it = subscribers.find(event_type);
    if (it != subscribers.end()) {
        auto &vec = it->second;
        vec.erase(
            std::remove_if(vec.begin(), vec.end(),
                           [&callback](const std::function<void(const std::string&)>& existing_callback) {
                               // Attempt to compare callable targets where possible.
                               // This is best-effort: comparing target pointers for function pointers.
                               auto existing_ptr = existing_callback.template target<void(*)(const std::string&)>();
                               auto remove_ptr = callback.template target<void(*)(const std::string&)>();
                               if (existing_ptr && remove_ptr) {
                                   return *existing_ptr == *remove_ptr;
                               }
                               // Fallback: compare type_info (may equate different closures of same type)
                               return existing_callback.target_type() == callback.target_type();
                           }),
            vec.end());
        if (vec.empty()) {
            subscribers.erase(it);
        }
    }
}

void EventBus::publish(const std::string& event_type, const std::string& data) {
    std::shared_lock<std::shared_mutex> lock(mutex);
    auto it = subscribers.find(event_type);
    if (it != subscribers.end()) {
        // Callbacks may modify subscribers; copy list to avoid iterator invalidation
        auto callbacks = it->second;
        lock.unlock();
        for (const auto& cb : callbacks) {
            try {
                cb(data);
            } catch (...) {
                // Swallow exceptions to avoid crashing the event loop
            }
        }
    }
}

void EventBus::onAppLaunch(std::function<void(const std::string&)> callback) {
    subscribe("app_launch", std::move(callback));
}

void EventBus::onAppTerminate(std::function<void(const std::string&)> callback) {
    subscribe("app_terminate", std::move(callback));
}

void EventBus::onWindowCreate(std::function<void(const std::string&)> callback) {
    subscribe("window_create", std::move(callback));
}

void EventBus::onNotificationShow(std::function<void(const std::string&)> callback) {
    subscribe("notification_show", std::move(callback));
}

} // namespace pce::sdlos
