#pragma once

namespace pce::sdlos {

class INotification {
public:
    virtual ~INotification() = default;
    virtual void show() = 0;
    virtual void hide() = 0;
    virtual std::string getMessage() const = 0;
    virtual std::chrono::steady_clock::time_point getTimestamp() const = 0;
};

}
