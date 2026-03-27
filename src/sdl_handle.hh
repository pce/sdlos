#pragma once

#include <utility>
#include <SDL3/SDL_video.h>
#include <SDL3/SDL_render.h>

namespace sdl {

template<typename T>
struct traits;

template<>
struct traits<SDL_Window> {
    static void destroy(SDL_Window* p) { SDL_DestroyWindow(p); }
};

template<>
struct traits<SDL_Renderer> {
    static void destroy(SDL_Renderer* p) { SDL_DestroyRenderer(p); }
};

template<typename T>
class handle {
public:
    handle() = default;
    explicit handle(T* p) : ptr(p) {}
    ~handle() { reset(); }

    handle(const handle&)            = delete;
    handle& operator=(const handle&) = delete;

    handle(handle&& other) noexcept : ptr(other.ptr) {
        other.ptr = nullptr;
    }

    handle& operator=(handle&& other) noexcept {
        if (this != &other) {
            reset();
            ptr = other.ptr;
            other.ptr = nullptr;
        }
        return *this;
    }

    T*   get()  const { return ptr; }
    explicit operator bool() const { return ptr != nullptr; }

    void reset(T* p = nullptr) {
        if (ptr) traits<T>::destroy(ptr);
        ptr = p;
    }

private:
    T* ptr = nullptr;
};

} // namespace sdl
