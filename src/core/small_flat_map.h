#pragma once
#include <algorithm>
#include <array>
#include <vector>


namespace pce::sdlos::core {

template<typename K, typename V, size_t N = 8, typename Compare = std::less<K>>
class small_flat_map {
    using value_type = std::pair<K, V>;

    std::array<value_type, N> stack_data;
    std::vector<value_type> heap_data;

    size_t sz = 0;
    Compare comp;

    bool using_heap() const { return sz > N; }

    value_type* data_ptr() {
        return using_heap() ? heap_data.data() : stack_data.data();
    }

    const value_type* data_ptr() const {
        return using_heap() ? heap_data.data() : stack_data.data();
    }

    auto begin_ptr() { return data_ptr(); }
    auto end_ptr() { return data_ptr() + sz; }

    auto lower_bound(const K& key) {
        return std::lower_bound(begin_ptr(), end_ptr(), key,
            [this](const value_type& a, const K& b) {
                return comp(a.first, b);
            });
    }

public:
    using iterator = value_type*;

    iterator begin() { return begin_ptr(); }
    iterator end() { return end_ptr(); }

    size_t size() const { return sz; }

    std::pair<iterator, bool> insert(value_type v) {
        auto it = lower_bound(v.first);

        if (it != end_ptr() && !comp(v.first, it->first)) {
            return {it, false};
        }

        auto index = static_cast<std::ptrdiff_t>(it - begin_ptr());

        if (sz < N) {
            std::move_backward(begin_ptr() + index, end_ptr(), end_ptr() + 1);
            stack_data[static_cast<std::size_t>(index)] = std::move(v);
        } else {
            if (sz == N) {
                heap_data.assign(stack_data.begin(), stack_data.begin() + N);
            }
            heap_data.insert(heap_data.begin() + index, std::move(v));
        }

        ++sz;
        return {begin_ptr() + index, true};
    }

    V& operator[](const K& key) {
        auto it = lower_bound(key);

        if (it == end_ptr() || comp(key, it->first)) {
            return insert({key, V{}}).first->second;
        }
        return it->second;
    }
};

} // namespace pce::sdlos::core
