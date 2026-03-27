#pragma once


namespace pce::sdlos {

class IEventBus {
public:
    virtual ~IEventBus() = default;
    virtual void subscribe(const std::string& event_type,
                          std::function<void(const std::string&)> callback) = 0;
    virtual void publish(const std::string& event_type,
                        const std::string& data = "") = 0;
};
}
