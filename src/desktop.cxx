// Desktop.cxx — Window and Desktop implementation.
//
// Isolation guarantee
// -------------------
// Every Window constructor:
//   1. Calls SDL_CreateWindow        — allocates the native OS window.
//   2. Calls SDLRenderer::Initialize — creates a fresh SDL_GPUDevice, claims
//      the window for it, and builds the GPU pipeline.
//
// Every Window destructor:
//   1. Resets renderer_   — SDLRenderer::Shutdown() releases the GPU device
//      and un-claims the window before the window handle is destroyed.
//   2. Resets sdl_window_ — SDL_DestroyWindow() fires via sdl::handle.
//
// Tearing down one Window is therefore guaranteed to leave every other
// Window's GPU context completely untouched.
//
// Event routing
// -------------
// SDL_PollEvent tags events with SDL's own SDL_WindowID, which is NOT our
// sequential application int. Desktop keeps a second map (sdl_to_id_) so
// handleEvent() resolves the right Window in O(1) with no iteration.

#include "desktop.hh"
#include "sdl_renderer.hh"

#include <iostream>
#include <stdexcept>
#include <string>

#include <SDL3/SDL.h>

namespace pce::sdlos {

// ===========================================================================
// Window
// ===========================================================================

Window::Window(int id, const std::string& title,
               int x, int y, int w, int h,
               SDL_WindowFlags flags)
    : id_(id), title_(title), width_(w), height_(h)
{
    // ------------------------------------------------------------------
    // 1. Create the native OS window.
    // ------------------------------------------------------------------
    SDL_Window* raw = SDL_CreateWindow(title.c_str(), w, h, flags);
    if (!raw) {
        throw std::runtime_error(
            std::string("[Window] SDL_CreateWindow failed: ") + SDL_GetError());
    }
    sdl_window_.reset(raw);

    // SDL3 dropped the x/y parameters from SDL_CreateWindow; set position
    // explicitly after creation.
    SDL_SetWindowPosition(raw, x, y);

    // Reflect the size SDL actually allocated (may differ on HiDPI displays).
    SDL_GetWindowSize(raw, &width_, &height_);

    // Make the window visible immediately.
    SDL_ShowWindow(raw);

    // ------------------------------------------------------------------
    // 2. Create an isolated GPU context for this window.
    //
    //    SDLRenderer::Initialize() creates its own SDL_GPUDevice, claims
    //    the window for it, then builds the shader pipeline. If it fails
    //    we throw so the Window is never left in a half-initialised state.
    //    The sdl::handle destructor will clean up sdl_window_ on unwind.
    // ------------------------------------------------------------------
    renderer_ = std::make_unique<SDLRenderer>();
    if (!renderer_->Initialize(raw)) {
        renderer_.reset();
        throw std::runtime_error(
            std::string("[Window] SDLRenderer::Initialize failed for '") +
            title + "'");
    }

    std::cerr << "[Window] opened"
              << "  id="         << id_
              << "  title='"     << title_ << "'"
              << "  size="       << width_ << "x" << height_
              << "  sdl_id="     << SDL_GetWindowID(raw)
              << "\n";
}

Window::~Window()
{
    // GPU resources must be released before the window surface they were
    // bound to. Destruction order is intentionally: renderer → sdl_window.
    renderer_.reset();
    sdl_window_.reset();
}

// ---------------------------------------------------------------------------
// IWindow — visibility
// ---------------------------------------------------------------------------

void Window::show()
{
    if (sdl_window_) SDL_ShowWindow(sdl_window_.get());
}

void Window::hide()
{
    if (sdl_window_) SDL_HideWindow(sdl_window_.get());
}

// ---------------------------------------------------------------------------
// IWindow — geometry
// ---------------------------------------------------------------------------

void Window::resize(int w, int h)
{
    width_  = w;
    height_ = h;
    if (sdl_window_) SDL_SetWindowSize(sdl_window_.get(), w, h);
}

void Window::move(int x, int y)
{
    if (sdl_window_) SDL_SetWindowPosition(sdl_window_.get(), x, y);
}

// ---------------------------------------------------------------------------
// IWindow — focus / state
// ---------------------------------------------------------------------------

void Window::focus()
{
    is_focused_ = true;
    if (sdl_window_) SDL_RaiseWindow(sdl_window_.get());
}

void Window::minimize()
{
    is_minimized_ = true;
    if (sdl_window_) SDL_MinimizeWindow(sdl_window_.get());
}

void Window::maximize()
{
    is_maximized_ = true;
    is_minimized_ = false;
    if (sdl_window_) SDL_MaximizeWindow(sdl_window_.get());
}

void Window::restore()
{
    is_minimized_ = false;
    is_maximized_ = false;
    if (sdl_window_) SDL_RestoreWindow(sdl_window_.get());
}

// ---------------------------------------------------------------------------
// Per-frame
// ---------------------------------------------------------------------------

void Window::render(double timeSeconds)
{
    if (is_minimized_) return;  // nothing to present while minimised
    if (renderer_)     renderer_->Render(timeSeconds);
}

void Window::handleEvent(const SDL_Event& e)
{
    switch (e.type) {
        case SDL_EVENT_WINDOW_RESIZED:
            width_  = e.window.data1;
            height_ = e.window.data2;
            break;

        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            is_focused_ = true;
            break;

        case SDL_EVENT_WINDOW_FOCUS_LOST:
            is_focused_ = false;
            break;

        case SDL_EVENT_WINDOW_MINIMIZED:
            is_minimized_ = true;
            break;

        case SDL_EVENT_WINDOW_MAXIMIZED:
            is_maximized_ = true;
            is_minimized_ = false;
            break;

        case SDL_EVENT_WINDOW_RESTORED:
            is_minimized_ = false;
            is_maximized_ = false;
            break;

        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// GPU context / Desktop-internal
// ---------------------------------------------------------------------------

SDL_GPUDevice* Window::getGPUDevice() const
{
    return renderer_ ? renderer_->GetDevice() : nullptr;
}

SDL_WindowID Window::sdlWindowId() const
{
    return sdl_window_ ? SDL_GetWindowID(sdl_window_.get()) : 0;
}

// ===========================================================================
// Desktop
// ===========================================================================

int Desktop::open(const std::string& title,
                  int x, int y, int w, int h,
                  SDL_WindowFlags flags)
{
    const int app_id = next_id_++;

    // Window constructor throws on failure — propagate so the caller knows
    // the window was not created and no ID was consumed.
    auto win = std::make_shared<Window>(app_id, title, x, y, w, h, flags);

    const SDL_WindowID sdl_id = win->sdlWindowId();

    windows_[app_id]   = std::move(win);
    sdl_to_id_[sdl_id] = app_id;

    return app_id;
}

void Desktop::close(int id)
{
    const auto it = windows_.find(id);
    if (it == windows_.end()) return;

    // Remove the SDL_WindowID → app_id mapping before the Window (and its
    // sdl_window_) is destroyed so the key stays valid during erase.
    sdl_to_id_.erase(it->second->sdlWindowId());

    // Erasing the shared_ptr drops the refcount to zero when this is the
    // sole owner, which triggers ~Window(): renderer_ first, then sdl_window_.
    windows_.erase(it);
}

// ---------------------------------------------------------------------------
// Main-loop hooks
// ---------------------------------------------------------------------------

void Desktop::handleEvent(SDL_Event* event)
{
    if (!event) return;

    // Only handle SDL window events; all other events belong to the OS bus.
    if (event->type < SDL_EVENT_WINDOW_FIRST ||
        event->type > SDL_EVENT_WINDOW_LAST)
        return;

    // O(1) lookup: SDL_WindowID → app_id → Window.
    const auto sdlIt = sdl_to_id_.find(event->window.windowID);
    if (sdlIt == sdl_to_id_.end()) return;

    const auto winIt = windows_.find(sdlIt->second);
    if (winIt != windows_.end()) {
        winIt->second->handleEvent(*event);
    }
}

void Desktop::tick()
{
    // Reserved for future per-window update logic such as animation ticks,
    // dirty-rect tracking, or compositor z-order updates.
    (void)this;
}

void Desktop::render()
{
    const double t = static_cast<double>(SDL_GetTicks()) * 0.001;
    for (auto& [id, win] : windows_) {
        win->render(t);
    }
}

// ---------------------------------------------------------------------------
// Z-order / focus
// ---------------------------------------------------------------------------

void Desktop::raise(int id)
{
    if (const auto w = get(id)) w->focus();
}

void Desktop::focus(int id)
{
    if (const auto w = get(id)) w->focus();
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

std::shared_ptr<Window> Desktop::get(int id) const
{
    const auto it = windows_.find(id);
    return it != windows_.end() ? it->second : nullptr;
}

std::vector<int> Desktop::ids() const
{
    std::vector<int> result;
    result.reserve(windows_.size());
    for (const auto& [id, _] : windows_) {
        result.push_back(id);
    }
    return result;
}

} // namespace pce::sdlos
