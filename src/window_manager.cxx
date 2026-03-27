// WindowManager.cxx
// Implements Window (self-contained SDL_Window + isolated SDL_GPUDevice) and
// WindowManager (lifecycle owner / event router for all live windows).
//
// Isolation guarantee
// -------------------
// Every Window constructor:
//   1. Calls SDL_CreateWindow  — allocates the native OS window.
//   2. Calls SDLRenderer::Initialize(sdl_window) — creates a fresh
//      SDL_GPUDevice, claims the window for it, and builds the GPU pipeline.
//
// Every Window destructor:
//   1. Resets renderer_   — SDLRenderer::Shutdown() releases the GPU device
//      and un-claims the window before the window handle is destroyed.
//   2. Resets sdl_window_ — SDL_DestroyWindow() is called via sdl::handle.
//
// This means tearing down one Window is guaranteed not to touch any other
// Window's GPU context.
//
// Event routing
// -------------
// SDL_PollEvent returns events tagged with SDL's own SDL_WindowID, which is
// *not* our sequential application ID. WindowManager maintains a second map
// (sdl_to_app_id_) so handleEvent() routes in O(1) without iteration.

#include "window_manager.hh"
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

    // Position is set separately in SDL3 (CreateWindow no longer takes x/y).
    SDL_SetWindowPosition(raw, x, y);

    // Reflect the actual size that SDL ended up with (may differ for HiDPI).
    SDL_GetWindowSize(raw, &width_, &height_);

    // Make the window visible immediately.
    SDL_ShowWindow(raw);

    // ------------------------------------------------------------------
    // 2. Create an isolated GPU context for this window.
    //    SDLRenderer::Initialize() calls SDL_CreateGPUDevice internally,
    //    then SDL_ClaimWindowForGPUDevice, then builds the shader pipeline.
    //    If it fails we throw so the Window is never in a half-initialised
    //    state; the sdl::handle destructor will clean up sdl_window_.
    // ------------------------------------------------------------------
    renderer_ = std::make_unique<SDLRenderer>();
    if (!renderer_->Initialize(raw)) {
        // renderer_ holds no GPU resources on failure, safe to reset.
        renderer_.reset();
        throw std::runtime_error(
            std::string("[Window] SDLRenderer::Initialize failed for '") +
            title + "'");
    }

    std::cerr << "[Window] id=" << id_
              << " title='" << title_ << "'"
              << " size=" << width_ << "x" << height_
              << " sdl_window_id=" << SDL_GetWindowID(raw)
              << "\n";
}

Window::~Window()
{
    // Release the GPU context before the window surface it was bound to.
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
    if (is_minimized_) return;   // nothing to present when minimised
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
// GPU context / WindowManager internal
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
// WindowManager
// ===========================================================================

int WindowManager::createWindow(const std::string& title,
                                int x, int y, int w, int h,
                                SDL_WindowFlags flags)
{
    const int app_id = next_id_++;

    // Window constructor throws on failure — let it propagate so the caller
    // knows the window was not created.
    auto win = std::make_shared<Window>(app_id, title, x, y, w, h, flags);

    const SDL_WindowID sdl_id = win->sdlWindowId();

    windows_[app_id]      = win;
    sdl_to_app_id_[sdl_id] = app_id;

    return app_id;
}

void WindowManager::destroyWindow(int id)
{
    auto it = windows_.find(id);
    if (it == windows_.end()) return;

    // Remove the SDL_WindowID → app_id mapping first.
    sdl_to_app_id_.erase(it->second->sdlWindowId());

    // Erase the Window; shared_ptr refcount drop triggers destructor which
    // releases GPU context then the SDL window in the correct order.
    windows_.erase(it);
}

// ---------------------------------------------------------------------------
// Main-loop hooks
// ---------------------------------------------------------------------------

void WindowManager::handleEvent(SDL_Event* event)
{
    if (!event) return;

    // Only handle SDL window events; everything else is for the OS event bus.
    if (event->type < SDL_EVENT_WINDOW_FIRST ||
        event->type > SDL_EVENT_WINDOW_LAST)
        return;

    // Route using the SDL_WindowID → app_id map — O(1), no iteration.
    auto sdlIt = sdl_to_app_id_.find(event->window.windowID);
    if (sdlIt == sdl_to_app_id_.end()) return;

    auto winIt = windows_.find(sdlIt->second);
    if (winIt != windows_.end()) {
        winIt->second->handleEvent(*event);
    }
}

void WindowManager::update()
{
    // Reserved for future per-window tick logic (animations, dirty-rect
    // tracking, etc.). Currently a no-op.
}

void WindowManager::render()
{
    const double t = static_cast<double>(SDL_GetTicks()) * 0.001;
    for (auto& [id, win] : windows_) {
        win->render(t);
    }
}

// ---------------------------------------------------------------------------
// Z-order / focus
// ---------------------------------------------------------------------------

void WindowManager::bringToFront(int id)
{
    if (auto w = getWindow(id)) w->focus();
}

void WindowManager::sendToBack(int /*id*/)
{
    // SDL3 does not expose a "lower window" API. Z-order management would
    // require maintaining an explicit draw-order list and relying on the
    // compositor; left as a future enhancement.
}

void WindowManager::setFocus(int id)
{
    if (auto w = getWindow(id)) w->focus();
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

std::shared_ptr<Window> WindowManager::getWindow(int id) const
{
    auto it = windows_.find(id);
    return it != windows_.end() ? it->second : nullptr;
}

std::vector<int> WindowManager::getAllWindowIds() const
{
    std::vector<int> ids;
    ids.reserve(windows_.size());
    for (const auto& [id, _] : windows_) {
        ids.push_back(id);
    }
    return ids;
}

} // namespace pce::sdlos
