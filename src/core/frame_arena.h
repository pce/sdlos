#pragma once
#include <cstddef>
#include <span>
#include <stdexcept>
#include <vector>

namespace pce::sdlos::core {

class frame_arena {
    std::vector<std::byte> buffer;
    size_t offset = 0;

public:
    frame_arena(size_t size = 1 << 20) {
        buffer.resize(size);
    }

    //  Raw allocation

    /// Allocate `size` bytes aligned to `align`.
    /// Throws std::bad_alloc when the arena is exhausted.
    /// All memory is reclaimed at once by reset() — no individual frees.
    void* alloc(size_t size, size_t align = alignof(std::max_align_t)) {
        size_t ptr = (offset + align - 1) & ~(align - 1);

        if (ptr + size > buffer.size())
            throw std::bad_alloc();

        offset = ptr + size;
        return buffer.data() + ptr;
    }

    //  Typed allocation

    /// Allocate storage for `count` objects of type T.
    ///
    /// T must be trivially destructible — the arena never calls destructors.
    /// The returned pointer is valid until the next reset().
    ///
    /// Usage:
    ///   RenderNode** children = arena.alloc<RenderNode*>(child_count);
    template<typename T>
    [[nodiscard]] T* alloc(std::size_t count = 1)
    {
        static_assert(std::is_trivially_destructible_v<T>,
                      "frame_arena only holds trivially destructible types "
                      "(dtors are never called on arena-allocated objects).");
        return static_cast<T*>(alloc(sizeof(T) * count, alignof(T)));
    }

    /// Allocate `count` Ts and return a std::span over them.
    /// Returns an empty span when count == 0.
    /// The span is valid until the next reset().
    ///
    /// Usage:
    ///   std::span<RenderNode*> children = arena.allocSpan<RenderNode*>(n);
    template<typename T>
    [[nodiscard]] std::span<T> allocSpan(std::size_t count)
    {
        if (count == 0) return {};
        return { alloc<T>(count), count };
    }

    //  Lifecycle

    void reset() { offset = 0; }

    /// Current number of allocated bytes (rounded up for alignment).
    [[nodiscard]] std::size_t used()     const noexcept { return offset;          }
    /// Total capacity in bytes.
    [[nodiscard]] std::size_t capacity() const noexcept { return buffer.size();   }
    /// Remaining bytes before the arena is exhausted.
    [[nodiscard]] std::size_t remaining() const noexcept { return buffer.size() - offset; }
};
} // namespace pce::sdlos::core
