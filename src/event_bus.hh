#pragma once

#include <unordered_map>
#include <functional>
#include <vector>
#include <shared_mutex>
#include <string>
#include <algorithm>
#include "i_event_bus.hh"

namespace pce::sdlos {

class EventBus : public IEventBus {
private:
    std::unordered_map<std::string, std::vector<std::function<void(const std::string&)>>> subscribers;
    mutable std::shared_mutex mutex;

public:
    EventBus();
    ~EventBus() = default;

    void subscribe(const std::string& event_type, std::function<void(const std::string&)> callback) override;
    void publish(const std::string& event_type, const std::string& data = "") override;

    /// Drop every subscription — call before loading a new scene so old
    /// behaviour callbacks cannot fire against a destroyed RenderTree.
    void reset();

    void unsubscribe(const std::string& event_type, std::function<void(const std::string&)> callback);
    void onAppLaunch(std::function<void(const std::string&)> callback);
    void onAppTerminate(std::function<void(const std::string&)> callback);
    void onWindowCreate(std::function<void(const std::string&)> callback);
    void onNotificationShow(std::function<void(const std::string&)> callback);
};

} // namespace pce::sdlos
