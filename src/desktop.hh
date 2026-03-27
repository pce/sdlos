#pragma once

// Desktop.h — Window system for the pce::sdlos desktop environment.
//
// Two focused types:
//
//   Window  — self-contained "process window".
//             One Window == one SDL_Window == one SDL_GPUDevice (via SDLRenderer).
//             Constructing a Window allocates the native OS window and an
//             independent GPU context. Destroying it releases both with no
//             side-effects on any other Window. This mirrors OS-level process
//             isolation: each app window owns its own rendering stack.
//
//   Desktop — lifecycle owner for all live Window instances.
//             Responsibilities:
//               • open() / close() windows.
//               • Route SDL window events to the correct Window via a dual-ID
//                 map (SDL assigns its own opaque SDL_WindowID; Desktop keeps a
//                 second map from that to our sequential int so handleEvent()
//                 is O(1) with no iteration).
//               • Drive the per-frame render loop.
//             The Desktop does NOT own an SDL_Window directly — all native
//             handles live inside individual Window instances.
//
// SDL members of Window are intentionally private. External code interacts
// through IWindow and the render/event API below.

#include "i_window.hh"
#include "sdl_handle.hh"

#include <SDL3/SDL.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace pce::sdlos {

class SDLRenderer;

// ---------------------------------------------------------------------------
// Window
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

    [[nodiscard]] bool        isFocused()   const override { return is_focused_;   }
    [[nodiscard]] bool        isMinimized() const override { return is_minimized_; }
    [[nodiscard]] bool        isMaximized() const override { return is_maximized_; }
    [[nodiscard]] std::string getTitle()    const override { return title_;        }
    [[nodiscard]] int         getId()       const override { return id_;           }

    // ---- Per-frame -------------------------------------------------------

    /// Render one frame. `timeSeconds` is forwarded to the GPU shader uniform.
    /// Skips rendering while the window is minimised.
    void render(double timeSeconds);

    /// Route an SDL window event that belongs to this window.
    void handleEvent(const SDL_Event& e);

    // ---- GPU context access ----------------------------------------------

    /// The SDL_GPUDevice owned by this window's renderer.
    /// Non-null after successful construction.
    /// Lifetime is tied to this Window — do not call SDL_DestroyGPUDevice()
    /// on the returned pointer.
    [[nodiscard]] SDL_GPUDevice* getGPUDevice() const;

    // ---- Desktop-internal ------------------------------------------------

    /// SDL's own window ID for this window.
    /// Used by Desktop to route SDL_PollEvent results without exposing
    /// the raw SDL_Window*.
    [[nodiscard]] SDL_WindowID sdlWindowId() const;

private:
    int         id_;
    std::string title_;
    int         width_{0};
    int         height_{0};

    bool is_focused_{false};
    bool is_minimized_{false};
    bool is_maximized_{false};

    // Owned SDL window. Destroyed after renderer_ so the GPU device is
    // released before the surface it was presenting to.
    sdl::handle<SDL_Window> sdl_window_;

    // Isolated GPU context: owns SDL_GPUDevice + shaders + pipeline.
    // Destroyed before sdl_window_ — SDLRenderer::Shutdown() un-claims
    // the window from the GPU device before SDL_DestroyWindow is called.
    std::unique_ptr<SDLRenderer> renderer_;
};

// ---------------------------------------------------------------------------
// Desktop
// ---------------------------------------------------------------------------

class Desktop {
public:
    Desktop()  = default;
    ~Desktop() = default;

    // Non-copyable.
    Desktop(const Desktop&)            = delete;
    Desktop& operator=(const Desktop&) = delete;

    // ---- Window lifecycle ------------------------------------------------

    /// Open a new isolated window (SDL_Window + independent GPU context).
    /// Returns the application-level window ID on success.
    /// Throws std::runtime_error if SDL or GPU initialisation fails.
    int open(const std::string& title,
             int x, int y, int w, int h,
             SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE);

    /// Close a window and free all its SDL + GPU resources immediately.
    /// Silently ignores unknown IDs.
    void close(int id);

    // ---- Main-loop hooks -------------------------------------------------

    /// Route a raw SDL event to the window it belongs to.
    /// Non-window events are silently ignored.
    void handleEvent(SDL_Event* event);

    /// Per-frame update tick (reserved for future per-window logic).
    void tick();

    /// Render all live windows.
    void render();

    // ---- Z-order / focus -------------------------------------------------

    void raise(int id);
    void focus(int id);

    // ---- Queries ---------------------------------------------------------

    [[nodiscard]] std::shared_ptr<Window> get(int id) const;
    [[nodiscard]] std::vector<int>        ids()       const;
    [[nodiscard]] bool                    empty()     const { return windows_.empty(); }
    [[nodiscard]] std::size_t             count()     const { return windows_.size();  }

private:
    // Application-level id → Window.
    std::unordered_map<int, std::shared_ptr<Window>> windows_;

    // SDL_WindowID → application-level id.
    // SDL assigns its own opaque IDs; this map lets handleEvent() find the
    // right Window in O(1) without iterating over windows_.
    std::unordered_map<SDL_WindowID, int> sdl_to_id_;

    int next_id_{1};
};

} // namespace pce::sdlos
