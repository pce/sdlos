#pragma once

#include <utility> // For std::move
#include <SDL3/SDL_video.h>  // For SDL_Window and SDL_DestroyWindow
#include <SDL3/SDL_render.h> // For SDL_Renderer and SDL_DestroyRenderer

namespace sdl {

// Traits for SDL resource management
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

// Generalized handle class for SDL resources
template<typename T>
class handle {
public:
    // Default constructor
    handle() = default;

    // Constructor with raw pointer
    explicit handle(T* p) : ptr(p) {}

    // Destructor to clean up the resource
    ~handle() { reset(); }

    // Delete copy constructor and copy assignment
    handle(const handle&) = delete;
    handle& operator=(const handle&) = delete;

    // Move constructor
    handle(handle&& other) noexcept : ptr(other.ptr) {
        other.ptr = nullptr;
    }

    // Move assignment operator
    handle& operator=(handle&& other) noexcept {
        if (this != &other) {
            reset();
            ptr = other.ptr;
            other.ptr = nullptr;
        }
        return *this;
    }

    // Get the raw pointer
    T* get() const { return ptr; }

    // Implicit conversion to bool
    operator bool() const { return ptr != nullptr; }

    // Reset the handle with a new pointer
    void reset(T* p = nullptr) {
        if (ptr) traits<T>::destroy(ptr);
        ptr = p;
    }

private:
    T* ptr = nullptr; // Raw pointer to the SDL resource
};

} // namespace sdl
