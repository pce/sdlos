#pragma once

#include <SDL3/SDL.h>
#include "IWindow.h"
#include "sdl_handle.h"
#include <unordered_map>
#include <memory>
#include <vector>
#include <string>

namespace pce::sdlos {

class SDLRenderer;

// ---------------------------------------------------------------------------
// Window — self-contained "process window"
//
// Invariant: one Window == one SDL_Window == one SDL_GPUDevice (via SDLRenderer).
//
// Constructing a Window allocates the native OS window and an independent GPU
// context. Destroying it releases both, with no side-effects on any other
// Window. This mirrors OS-level process isolation: each app window owns its
// own rendering stack and cannot accidentally share or corrupt another
// window's GPU state.
//
// SDL members are intentionally private. Callers interact through IWindow and
// the render/event API below. Low-level SDL access is exposed only where
// WindowManager needs it for internal routing (sdlWindowId()).
// ---------------------------------------------------------------------------
class Window : public IWindow {
public:
    Window(int id, const std::string& title,
           int x, int y, int w, int h,
           SDL_WindowFlags flags);
    ~Window() override;

    // Non-copyable / non-movable: owns GPU resources tied to a specific device.
    Window(const Window&)            = delete;
    Window& operator=(const Window&) = delete;
    Window(Window&&)                 = delete;
    Window& operator=(Window&&)      = delete;

    // ---- IWindow interface -----------------------------------------------

    void show()              override;
    void hide()              override;
    void resize(int w, int h) override;
    void move(int x, int y)  override;
    void focus()             override;
    void minimize()          override;
    void maximize()          override;
    void restore()           override;

    bool        isFocused()   const override { return is_focused_;   }
    bool        isMinimized() const override { return is_minimized_; }
    bool        isMaximized() const override { return is_maximized_; }
    std::string getTitle()    const override { return title_;        }
    int         getId()       const override { return id_;           }

    // ---- Per-frame -------------------------------------------------------

    /// Render a frame. `timeSeconds` is forwarded to the GPU shader uniform.
    void render(double timeSeconds);

    /// Route an SDL window event that belongs to this window.
    void handleEvent(const SDL_Event& e);

    // ---- GPU context access ----------------------------------------------

    /// Return the SDL_GPUDevice owned by this window's renderer.
    /// Non-null after successful construction. The device lifetime is tied to
    /// this Window — callers must not destroy it independently.
    SDL_GPUDevice* getGPUDevice() const;

    // ---- WindowManager internal ------------------------------------------

    /// SDL's own window ID for this window — used to route SDL_PollEvent
    /// results back to the correct Window without exposing the SDL_Window*.
    SDL_WindowID sdlWindowId() const;

private:
    int         id_;
    std::string title_;
    int         width_{0};
    int         height_{0};

    bool is_focused_{false};
    bool is_minimized_{false};
    bool is_maximized_{false};

    // Owned SDL window handle. Destroyed after renderer_ in the destructor so
    // the GPU device is released before the window surface it presented to.
    sdl::handle<SDL_Window>      sdl_window_;

    // Isolated GPU context: owns SDL_GPUDevice + shaders + pipeline.
    // Destroyed before sdl_window_ — SDLRenderer::Shutdown() releases the
    // GPU device claim on the window before SDL_DestroyWindow is called.
    std::unique_ptr<SDLRenderer> renderer_;
};

// ---------------------------------------------------------------------------
// WindowManager — lifecycle owner for all Window instances
//
// Responsibilities:
//   • Create / destroy Window objects (and therefore SDL + GPU resources).
//   • Route SDL window events to the correct Window using a dual-ID map
//     (SDL assigns its own SDL_WindowID; we maintain our own sequential int).
//   • Drive the per-frame render loop across all live windows.
//
// The manager does NOT own an SDL_Window directly — all native handles live
// inside individual Window instances.
// ---------------------------------------------------------------------------
class WindowManager {
public:
    WindowManager()  = default;
    ~WindowManager() = default;

    // Non-copyable
    WindowManager(const WindowManager&)            = delete;
    WindowManager& operator=(const WindowManager&) = delete;

    // ---- Window lifecycle ------------------------------------------------

    /// Allocate a new isolated window (SDL_Window + GPU context).
    /// Returns the application-level window ID on success.
    /// Throws std::runtime_error if SDL or GPU initialisation fails.
    int createWindow(const std::string& title,
                     int x, int y, int w, int h,
                     SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE);

    /// Destroy a window and free all its SDL + GPU resources.
    void destroyWindow(int id);

    // ---- Main-loop hooks -------------------------------------------------

    /// Route a raw SDL event to the window it belongs to.
    /// Non-window events are silently ignored.
    void handleEvent(SDL_Event* event);

    /// Per-frame update tick (reserved for future per-window logic).
    void update();

    /// Render all live windows.
    void render();

    // ---- Z-order / focus -------------------------------------------------

    void bringToFront(int id);
    void sendToBack(int id);   ///< No-op (SDL3 has no "send to back" API).
    void setFocus(int id);

    // ---- Queries ---------------------------------------------------------

    std::shared_ptr<Window> getWindow(int id) const;
    std::vector<int>        getAllWindowIds()  const;

private:
    // Application-level id → Window
    std::unordered_map<int, std::shared_ptr<Window>> windows_;

    // SDL_WindowID → application-level id.
    // SDL assigns its own opaque IDs; this map lets handleEvent() find the
    // right Window in O(1) without iterating over windows_.
    std::unordered_map<SDL_WindowID, int> sdl_to_app_id_;

    int next_id_{1};
};

} // namespace pce::sdlos
