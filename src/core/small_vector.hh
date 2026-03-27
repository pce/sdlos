#pragma once
#include <cstddef>
#include <memory>
#include <algorithm>

namespace sdlos::core {

template<typename T, size_t N>
class small_vector {
    alignas(T) unsigned char stack[N * sizeof(T)];
    T* heap = nullptr;
    size_t sz = 0;
    size_t cap = N;

    T* stack_ptr() { return reinterpret_cast<T*>(stack); }
    const T* stack_ptr() const { return reinterpret_cast<const T*>(stack); }

    bool using_heap() const { return heap != nullptr; }
    T* data_ptr() { return using_heap() ? heap : stack_ptr(); }

    void grow() {
        size_t new_cap = cap * 2;
        T* new_mem = static_cast<T*>(::operator new(sizeof(T) * new_cap));

        std::uninitialized_move(begin(), end(), new_mem);

        if (using_heap()) ::operator delete(heap);

        heap = new_mem;
        cap = new_cap;
    }

public:
    using iterator = T*;

    ~small_vector() {
        std::destroy(begin(), end());
        if (using_heap()) ::operator delete(heap);
    }

    iterator begin() { return data_ptr(); }
    iterator end() { return data_ptr() + sz; }

    size_t size() const { return sz; }

    void push_back(const T& v) {
        if (sz == cap) grow();
        new (data_ptr() + sz) T(v);
        ++sz;
    }

    void push_back(T&& v) {
        if (sz == cap) grow();
        new (data_ptr() + sz) T(std::move(v));
        ++sz;
    }
};

} // namespace sdlos::core
