#pragma once
#include <vector>
#include <cstdint>

namespace sdlos::core {


struct SlotID {
    uint32_t index      = ~uint32_t{0};
    uint32_t generation = ~uint32_t{0};

    [[nodiscard]] bool operator==(const SlotID&) const noexcept = default;
    [[nodiscard]] bool operator!=(const SlotID&) const noexcept = default;

    /// Returns true when this is a real (non-null) handle.
    [[nodiscard]] bool valid() const noexcept { return index != ~uint32_t{0}; }
};

/// Canonical null sentinel — both fields set to max value.
inline constexpr SlotID k_null_slot = {};

template<typename T>
class slot_map {
    struct Slot {
        T value;
        uint32_t generation = 0;
        bool alive = false;
    };

    std::vector<Slot> slots;
    std::vector<uint32_t> free_list;

public:
    SlotID insert(T v) {
        if (!free_list.empty()) {
            uint32_t i = free_list.back();
            free_list.pop_back();

            slots[i].value = std::move(v);
            slots[i].alive = true;

            return {i, slots[i].generation};
        }

        slots.push_back({std::move(v), 0, true});
        return {uint32_t(slots.size() - 1), 0};
    }

    void erase(SlotID id) {
        auto& s = slots[id.index];
        if (s.generation != id.generation) return;

        s.alive = false;
        ++s.generation;
        free_list.push_back(id.index);
    }

    T* get(SlotID id) {
        if (id.index >= slots.size()) return nullptr;
        auto& s = slots[id.index];
        if (s.generation != id.generation || !s.alive)
            return nullptr;
        return &s.value;
    }

    const T* get(SlotID id) const {
        if (id.index >= slots.size()) return nullptr;
        const auto& s = slots[id.index];
        if (s.generation != id.generation || !s.alive)
            return nullptr;
        return &s.value;
    }

    /// Number of live (non-erased) slots.
    [[nodiscard]] std::size_t size() const noexcept {
        return slots.size() - free_list.size();
    }

    /// Total allocated slots (live + free).
    [[nodiscard]] std::size_t capacity() const noexcept {
        return slots.size();
    }
};

} // namespace sdlos::core
