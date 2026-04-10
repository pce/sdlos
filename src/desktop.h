#pragma once

#include "debug/layout_debug.h"
#include "i_window.h"
#include "render_tree.h"
#include "sdl_handle.h"
#include "widgets/input_text_box.h"
#include "widgets/widget.h"

#include <SDL3/SDL.h>

#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace pce::sdlos {

class SDLRenderer;

class Window : public IWindow {
  public:
    [[nodiscard]]
    static std::expected<std::unique_ptr<Window>, std::string>
    create(int id, const std::string &title, int x, int y, int w, int h, SDL_WindowFlags flags);

    /**
     * @brief ~window
     */
    ~Window() override;

    // Non-copyable / non-movable: owns GPU resources tied to a specific device.
    /**
     * @brief Window
     *
     * @param param0  Red channel component [0, 1]
     */
    Window(const Window &)            = delete;
    Window &operator=(const Window &) = delete;
    /**
     * @brief Window
     *
     * @param param0  Red channel component [0, 1]
     */
    Window(Window &&)            = delete;
    Window &operator=(Window &&) = delete;

    /**
     * @brief Shows
     */
    void show() override;
    /**
     * @brief Hides
     */
    void hide() override;
    /**
     * @brief Resizes
     *
     * @param w  Width in logical pixels
     * @param h  Opaque resource handle
     */
    void resize(int w, int h) override;
    /**
     * @brief Moves
     *
     * @param x  Horizontal coordinate in logical pixels
     * @param y  Vertical coordinate in logical pixels
     */
    void move(int x, int y) override;
    /**
     * @brief Focus
     */
    void focus() override;
    /**
     * @brief Minimize
     */
    void minimize() override;
    /**
     * @brief Maximize
     */
    void maximize() override;
    /**
     * @brief Restore
     */
    void restore() override;

    /**
     * @brief Checks whether minimized
     *
     * @return true on success, false on failure
     */
    bool isFocused() const override { return is_focused_; }
    /**
     * @brief Checks whether minimized
     *
     * @return true on success, false on failure
     */
    bool isMinimized() const override { return is_minimized_; }
    /**
     * @brief Checks whether maximized
     *
     * @return true on success, false on failure
     */
    bool isMaximized() const override { return is_maximized_; }
    /**
     * @brief Returns title
     *
     * @return Integer result; negative values indicate an error code
     */
    std::string getTitle() const override { return title_; }
    /**
     * @brief Returns id
     *
     * @return Integer result; negative values indicate an error code
     */
    int getId() const override { return id_; }

    /// Skips rendering while the window is minimised.
    void render(double timeSeconds);

    /**
     * @brief Handles event
     *
     * @param e  SDL3 input or window event
     */
    void handleEvent(const SDL_Event &e);

    /// Non-null after successful construction.
    /// Lifetime is tied to this Window — do not call SDL_DestroyGPUDevice()
    /// on the returned pointer.
    SDL_GPUDevice *getGPUDevice() const;

    /// Used by Desktop to route SDL_PollEvent results without exposing
    /// the raw SDL_Window*.
    SDL_WindowID sdlWindowId() const;

    /// Attach a RenderTree scene to be rendered on top of the wallpaper.
    /// Both pointers are non-owning; the scene must outlive the Window.
    void setScene(RenderTree *tree, NodeHandle root);

  private:
    /**
     * @brief Window
     *
     * @param id     Unique object identifier
     * @param title  Iterator position
     * @param w      Width in logical pixels
     * @param h      Opaque resource handle
     */
    Window(int id, const std::string &title, int w, int h);

    int id_;
    std::string title_;
    int width_{0};
    int height_{0};

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

class Desktop {
  public:
    /**
     * @brief Desktop
     */
    Desktop() = default;
    /**
     * @brief ~desktop
     */
    ~Desktop() = default;

    // Non-copyable / non-movable: owns a RenderTree with live node captures.
    /**
     * @brief Desktop
     *
     * @param param0  Red channel component [0, 1]
     */
    Desktop(const Desktop &)            = delete;
    Desktop &operator=(const Desktop &) = delete;
    /**
     * @brief Desktop
     *
     * @param param0  Red channel component [0, 1]
     */
    Desktop(Desktop &&)            = delete;
    Desktop &operator=(Desktop &&) = delete;

    /// Open a new isolated window (SDL_Window + independent GPU context).
    /// Returns the application-level window ID (≥ 1) on success, or -1 on
    /// any failure.  Never throws; errors are printed to stderr.
    int open(
        const std::string &title,
        int x,
        int y,
        int w,
        int h,
        SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE);

    /// Close a window and free all its SDL + GPU resources immediately.
    /// Silently ignores unknown IDs.
    void close(int id);

    /// Keyboard shortcuts:
    ///   Cmd/Ctrl+Space → toggle search overlay
    ///   Escape         → close search overlay
    ///   F1             → toggle layout debug overlay
    /// Text/key events are forwarded to the focused overlay widget when the
    /// search overlay is open.
    void handleEvent(SDL_Event *event);

    /**
     * @brief Ticks one simulation frame for
     */
    void tick();
    /**
     * @brief Renders
     */
    void render();

    /// Show the search overlay and give focus to the input box.
    void showSearchOverlay();

    /// Hide the search overlay and stop text input.
    void hideSearchOverlay();

    /**
     * @brief Toggle search overlay
     */
    void toggleSearchOverlay();

    /**
     * @brief Searches for overlay visible
     *
     * @return true on success, false on failure
     */
    [[nodiscard]]
    bool searchOverlayVisible() const noexcept {
        return search_visible_.get();
    }

    /// Toggle the F1 layout debug overlay on / off.
    /// When active, every RenderNode with a non-None LayoutKind is outlined
    /// in a color that matches its layout kind, with a label showing the kind
    /// abbreviation and computed pixel dimensions.
    void toggleLayoutDebug() noexcept { debug_layout_ = !debug_layout_; }

    /**
     * @brief Performs layout for debug visible
     *
     * @return true on success, false on failure
     */
    [[nodiscard]]
    bool layoutDebugVisible() const noexcept {
        return debug_layout_;
    }

    // Z-order / focus

    /**
     * @brief Raise
     *
     * @param id  Unique object identifier
     */
    void raise(int id);
    /**
     * @brief Focus
     *
     * @param id  Unique object identifier
     */
    void focus(int id);

    /**
     * @brief Returns
     *
     * @param id  Unique object identifier
     *
     * @return Integer result; negative values indicate an error code
     */
    std::shared_ptr<Window> get(int id) const;
    /**
     * @brief Ids
     *
     * @return Integer result; negative values indicate an error code
     */
    std::vector<int> ids() const;
    /**
     * @brief Empty
     *
     * @return true on success, false on failure
     */
    bool empty() const { return windows_.empty(); }
    /**
     * @brief Count
     *
     * @return Integer result; negative values indicate an error code
     */
    std::size_t count() const { return windows_.size(); }

    /**
     * @brief Scene tree
     *
     * @return Reference to the result
     */
    RenderTree &sceneTree() noexcept { return scene_tree_; }

  private:
    /// Build the desktop UI scene graph inside scene_tree_.
    /// Called automatically the first time open() succeeds (desktop window).
    void buildDesktopScene();

    /**
     * @brief Route overlay event
     *
     * @param event  Interpolation parameter in [0, 1]
     */
    void routeOverlayEvent(const SDL_Event &event);

    std::unordered_map<int, std::shared_ptr<Window>> windows_;
    std::unordered_map<SDL_WindowID, int> sdl_to_id_;

    int next_id_{1};

    RenderTree scene_tree_;
    NodeHandle scene_root_;
    NodeHandle search_overlay_node_;
    std::optional<widgets::TextBox> search_input_box_;
    NodeHandle layout_debug_node_;

    Signal<bool> search_visible_{false};
    Signal<std::string> search_query_{std::string{}};

    bool debug_layout_ = false;  // not reactive — just a plain bool
};

}  // namespace pce::sdlos
