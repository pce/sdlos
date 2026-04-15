#pragma once

#include <string>
#include <vector>
#include <functional>

namespace pce::sdlos {

template <typename T>
class Signal {
public:
    using Callback = std::function<void(const T&)>;

    Signal() = default;
    explicit Signal(const T& initial_value) : value_(initial_value) {}

    const T& get() const { return value_; }

    void set(const T& value) {
        if (value_ != value) {
            value_ = value;
            for (const auto& cb : callbacks_) {
                cb(value_);
            }
        }
    }

    void connect(Callback cb) {
        callbacks_.push_back(std::move(cb));
    }

private:
    T value_;
    std::vector<Callback> callbacks_;
};

} // namespace pce::sdlos

