#pragma once
#include <cstddef>
#include <vector>

namespace sdlos::core {

class frame_arena {
    std::vector<std::byte> buffer;
    size_t offset = 0;

public:
    frame_arena(size_t size = 1 << 20) {
        buffer.resize(size);
    }

    void* alloc(size_t size, size_t align = alignof(std::max_align_t)) {
        size_t ptr = (offset + align - 1) & ~(align - 1);

        if (ptr + size > buffer.size())
            throw std::bad_alloc();

        offset = ptr + size;
        return buffer.data() + ptr;
    }

    void reset() { offset = 0; }
};
} // namespace sdlos::core
