#pragma once

#include "i_window.hh"
#include "sdl_handle.hh"
#include "render_tree.hh"
#include "widgets/widget.hh"
#include "widgets/input_text_box.hh"
#include "debug/layout_debug.hh"

#include <SDL3/SDL.h>

#include <expected>
#include <memory>
#include <string>
#include <unordered_map>
#include <optional>
#include <vector>

namespace pce::sdlos {

class SDLRenderer;

// ---------------------------------------------------------------------------
// Window
// ---------------------------------------------------------------------------

class Window : public IWindow {
public:

    [[nodiscard]]
    static std::expected<std::unique_ptr<Window>, std::string>
    create(int id, const std::string& title,
           int x, int y, int w, int h,
           SDL_WindowFlags flags);

    ~Window() override;

    // Non-copyable / non-movable: owns GPU resources tied to a specific device.
    Window(const Window&)            = delete;
    Window& operator=(const Window&) = delete;
    Window(Window&&)                 = delete;
    Window& operator=(Window&&)      = delete;

    // ---- IWindow interface -----------------------------------------------

    void show()               override;
    void hide()               override;
    void resize(int w, int h) override;
    void move(int x, int y)   override;
    void focus()              override;
    void minimize()           override;
    void maximize()           override;
    void restore()            override;

    [[nodiscard]] bool        isFocused()   const override { return is_focused_;   }
    [[nodiscard]] bool        isMinimized() const override { return is_minimized_; }
    [[nodiscard]] bool        isMaximized() const override { return is_maximized_; }
    [[nodiscard]] std::string getTitle()    const override { return title_;        }
    [[nodiscard]] int         getId()       const override { return id_;           }

    // ---- Per-frame -------------------------------------------------------

    /// Skips rendering while the window is minimised.
    void render(double timeSeconds);

    void handleEvent(const SDL_Event& e);

    // ---- GPU context access ----------------------------------------------

    /// Non-null after successful construction.
    /// Lifetime is tied to this Window — do not call SDL_DestroyGPUDevice()
    /// on the returned pointer.
    [[nodiscard]] SDL_GPUDevice* getGPUDevice() const;

    // ---- Desktop-internal ------------------------------------------------

    /// Used by Desktop to route SDL_PollEvent results without exposing
    /// the raw SDL_Window*.
    [[nodiscard]] SDL_WindowID sdlWindowId() const;

    /// Attach a RenderTree scene to be rendered on top of the wallpaper.
    /// Both pointers are non-owning; the scene must outlive the Window.
    void setScene(RenderTree* tree, NodeHandle root);

private:
    // Private constructor — only called by Window::create().
    Window(int id, const std::string& title, int w, int h);

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

    // Non-copyable / non-movable: owns a RenderTree with live node captures.
    Desktop(const Desktop&)            = delete;
    Desktop& operator=(const Desktop&) = delete;
    Desktop(Desktop&&)                 = delete;
    Desktop& operator=(Desktop&&)      = delete;

    // ---- Window lifecycle ------------------------------------------------

    /// Open a new isolated window (SDL_Window + independent GPU context).
    /// Returns the application-level window ID (≥ 1) on success, or -1 on
    /// any failure.  Never throws; errors are printed to stderr.
    int open(const std::string& title,
             int x, int y, int w, int h,
             SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE);

    /// Close a window and free all its SDL + GPU resources immediately.
    /// Silently ignores unknown IDs.
    void close(int id);

    // ---- Main-loop hooks -------------------------------------------------

    /// Keyboard shortcuts:
    ///   Cmd/Ctrl+Space → toggle search overlay
    ///   Escape         → close search overlay
    ///   F1             → toggle layout debug overlay
    /// Text/key events are forwarded to the focused overlay widget when the
    /// search overlay is open.
    void handleEvent(SDL_Event* event);

    void tick();
    void render();

    // ---- Search overlay --------------------------------------------------

    /// Show the search overlay and give focus to the input box.
    void showSearchOverlay();

    /// Hide the search overlay and stop text input.
    void hideSearchOverlay();

    void toggleSearchOverlay();

    [[nodiscard]] bool searchOverlayVisible() const noexcept
    {
        return search_visible_.get();
    }

    // ---- Layout debug overlay --------------------------------------------

    /// Toggle the F1 layout debug overlay on / off.
    /// When active, every RenderNode with a non-None LayoutKind is outlined
    /// in a color that matches its layout kind, with a label showing the kind
    /// abbreviation and computed pixel dimensions.
    void toggleLayoutDebug() noexcept { debug_layout_ = !debug_layout_; }

    [[nodiscard]] bool layoutDebugVisible() const noexcept { return debug_layout_; }

    // ---- Z-order / focus -------------------------------------------------

    void raise(int id);
    void focus(int id);

    // ---- Queries ---------------------------------------------------------

    [[nodiscard]] std::shared_ptr<Window> get(int id) const;
    [[nodiscard]] std::vector<int>        ids()       const;
    [[nodiscard]] bool                    empty()     const { return windows_.empty(); }
    [[nodiscard]] std::size_t             count()     const { return windows_.size();  }

    // ---- Scene access (for OS / tests) -----------------------------------

    [[nodiscard]] RenderTree& sceneTree() noexcept { return scene_tree_; }

private:
    // ---- Scene construction ----------------------------------------------

    /// Build the desktop UI scene graph inside scene_tree_.
    /// Called automatically the first time open() succeeds (desktop window).
    void buildDesktopScene();

    void routeOverlayEvent(const SDL_Event& event);

    // ---- Window storage --------------------------------------------------

    std::unordered_map<int, std::shared_ptr<Window>> windows_;
    std::unordered_map<SDL_WindowID, int>             sdl_to_id_;

    int next_id_{1};

    // ---- Desktop UI scene ------------------------------------------------

    RenderTree   scene_tree_;
    NodeHandle   scene_root_;
    NodeHandle   search_overlay_node_;
    std::optional<widgets::TextBox> search_input_box_;
    NodeHandle   layout_debug_node_;

    Signal<bool>        search_visible_{false};
    Signal<std::string> search_query_{std::string{}};

    bool debug_layout_ = false;  // not reactive — just a plain bool
};

} // namespace pce::sdlos
