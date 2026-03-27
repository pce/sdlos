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

#include <expected>
#include <format>
#include <iostream>
#include <string>

#include <SDL3/SDL.h>

namespace pce::sdlos {




// ===========================================================================
// Window
// ===========================================================================

// Private minimal constructor — only called by Window::create().
// Does not touch SDL or the GPU; all real initialisation happens in create().
Window::Window(int id, const std::string& title, int w, int h)
    : id_(id), title_(title), width_(w), height_(h)
{}

// ---------------------------------------------------------------------------
// Window::create — the only public way to construct a Window.
//
// Returns a fully initialised unique_ptr<Window> on success, or an error
// string wrapped in std::unexpected on failure.  No exception is ever raised.
// ---------------------------------------------------------------------------
std::expected<std::unique_ptr<Window>, std::string>
Window::create(int id, const std::string& title,
               int x, int y, int w, int h,
               SDL_WindowFlags flags)
{
    // ── 1. Native OS window ───────────────────────────────────────────────
    SDL_Window* raw = SDL_CreateWindow(title.c_str(), w, h, flags);
    if (!raw)
        return std::unexpected(
            std::format("[Window::create] SDL_CreateWindow failed: {}", SDL_GetError()));

    // SDL3 removed x/y from SDL_CreateWindow; set position explicitly.
    SDL_SetWindowPosition(raw, x, y);

    // Reflect the actual size the OS allocated (may differ on HiDPI).
    int actual_w = w, actual_h = h;
    SDL_GetWindowSize(raw, &actual_w, &actual_h);
    SDL_ShowWindow(raw);

    // ── 2. Construct the Window value, attach the SDL handle ──────────────
    // Direct new via private constructor — std::make_unique can't reach it.
    auto win = std::unique_ptr<Window>(new Window(id, title, actual_w, actual_h));
    win->sdl_window_.reset(raw);

    // ── 3. Isolated GPU context ───────────────────────────────────────────
    win->renderer_ = std::make_unique<SDLRenderer>();
    if (!win->renderer_->Initialize(raw))
        // ~Window() cleans up sdl_window_ and renderer_ on unwind.
        return std::unexpected(
            std::format("[Window::create] SDLRenderer::Initialize failed for '{}'", title));

    std::cerr << "[Window] opened"
              << "  id="     << id
              << "  title='" << title << "'"
              << "  size="   << actual_w << "x" << actual_h
              << "  sdl_id=" << SDL_GetWindowID(raw)
              << "\n";

    return win;
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

void Window::setScene(sdlos::RenderTree* tree, sdlos::NodeHandle root)
{
    if (renderer_) renderer_->SetScene(tree, root);
}

// ===========================================================================
// Desktop — private helpers
// ===========================================================================

namespace {

/// Mark every node in the subtree rooted at `h` as render-dirty (preorder DFS).
///
/// The wallpaper pipeline uses SDL_GPU_LOADOP_CLEAR each frame, so the screen
/// is wiped before any UI draw call.  Every visible overlay node must therefore
/// re-emit its draw commands every frame regardless of whether its content
/// changed.  RenderTree::markDirty() propagates dirty *upward* (to ancestors)
/// which is the wrong direction here; this helper propagates it *downward* into
/// the subtree so all children are redrawn this frame.
static void markSubtreeDirty(RenderTree& tree, NodeHandle h)
{
    RenderNode* n = tree.node(h);
    if (!n) return;
    n->dirty_render = true;
    for (NodeHandle c = n->child; c.valid(); ) {
        const RenderNode* cn   = tree.node(c);
        const NodeHandle  next = cn ? cn->sibling : k_null_handle;
        markSubtreeDirty(tree, c);
        c = next;
    }
}

} // anonymous namespace

// ===========================================================================
// Desktop
// ===========================================================================

int Desktop::open(const std::string& title,
                  int x, int y, int w, int h,
                  SDL_WindowFlags flags)
{
    const int app_id = next_id_;   // do not increment yet — only on success

    auto result = Window::create(app_id, title, x, y, w, h, flags);
    if (!result) {
        std::cerr << "[Desktop::open] " << result.error() << "\n";
        return -1;
    }

    ++next_id_;
    const SDL_WindowID sdl_id = (*result)->sdlWindowId();
    windows_[app_id]   = std::move(*result);
    sdl_to_id_[sdl_id] = app_id;

    // id=1 is always the desktop window; subsequent opens are app windows.
    if (app_id == 1) {
        buildDesktopScene();
    }

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

    // ---- Global keyboard shortcuts ---------------------------------------
    //
    //   Cmd+Space  (macOS)  or  Ctrl+Space  (Linux / Windows)
    //     → toggle the search overlay.
    //   Escape (while overlay is open)
    //     → dismiss the search overlay.
    //
    // These are consumed here (early return) so they do NOT reach individual
    // window event handlers.
    if (event->type == SDL_EVENT_KEY_DOWN) {
        const SDL_Keycode key     = event->key.key;
        const SDL_Keymod  mod     = event->key.mod;
        const bool        cmd_ctrl =
            (mod & SDL_KMOD_GUI) || (mod & SDL_KMOD_CTRL);

        if (cmd_ctrl && event->key.scancode == SDL_SCANCODE_SPACE) {
            toggleSearchOverlay();
            return;
        }
        if (key == SDLK_ESCAPE && search_visible_.get()) {
            hideSearchOverlay();
            return;
        }
    }

    // ---- Route input events to the active overlay widget -----------------
    //
    // When the search overlay is open, key presses, text input, and mouse
    // clicks are forwarded to the InputBox before falling through to the
    // regular window-event routing below.
    if (search_visible_.get() &&
        (event->type == SDL_EVENT_KEY_DOWN      ||
         event->type == SDL_EVENT_TEXT_INPUT    ||
         event->type == SDL_EVENT_MOUSE_BUTTON_DOWN)) {
        routeOverlayEvent(*event);
        // Fall through: window events (resize, focus, etc.) still need routing.
    }

    // ---- Route SDL window events to individual Window instances ----------
    if (event->type < SDL_EVENT_WINDOW_FIRST ||
        event->type > SDL_EVENT_WINDOW_LAST)
        return;

    const auto sdlIt = sdl_to_id_.find(event->window.windowID);
    if (sdlIt == sdl_to_id_.end()) return;

    const auto winIt = windows_.find(sdlIt->second);
    if (winIt != windows_.end()) {
        winIt->second->handleEvent(*event);
    }
}

void Desktop::tick()
{
    // Re-dirty the search overlay subtree every frame while it is visible.
    // The wallpaper pipeline uses SDL_GPU_LOADOP_CLEAR each frame, so all
    // overlay nodes must re-emit their GPU draw commands or they will
    // disappear after the clear.  markSubtreeDirty() propagates downward
    // into children (the opposite of RenderTree::markDirty, which propagates
    // upward to ancestors).
    if (search_visible_.get() && search_overlay_node_.valid()) {
        markSubtreeDirty(scene_tree_, search_overlay_node_);
    }
}

void Desktop::render()
{
    const double t = static_cast<double>(SDL_GetTicks()) * 0.001;
    // Re-attach the desktop scene to window 1 on every frame.
    // SetScene stores non-owning pointers so this is cheap.  It also ensures
    // that any window opened after buildDesktopScene() picks up the scene
    // without an explicit second call.
    if (auto w = get(1); w) {
        w->setScene(&scene_tree_, scene_root_);
    }
    for (auto& [id, win] : windows_) {
        win->render(t);
    }
}

// ---------------------------------------------------------------------------
// Search overlay — show / hide / toggle
// ---------------------------------------------------------------------------

void Desktop::showSearchOverlay()
{
    if (search_visible_.get()) return;
    search_visible_.set(true);
    if (search_input_node_.valid()) {
        widgets::inputBoxFocus(scene_tree_, search_input_node_);
    }
}

void Desktop::hideSearchOverlay()
{
    if (!search_visible_.get()) return;
    search_visible_.set(false);
    if (search_input_node_.valid()) {
        widgets::inputBoxUnfocus(scene_tree_, search_input_node_);
    }
}

void Desktop::toggleSearchOverlay()
{
    if (search_visible_.get()) hideSearchOverlay();
    else                       showSearchOverlay();
}

// ---------------------------------------------------------------------------
// routeOverlayEvent
// ---------------------------------------------------------------------------

void Desktop::routeOverlayEvent(const SDL_Event& event)
{
    if (!search_input_node_.valid()) return;
    widgets::inputBoxHandleEvent(scene_tree_, search_input_node_, event);
}

// ---------------------------------------------------------------------------
// buildDesktopScene
//
// Called automatically from Desktop::open() when the first window (id=1,
// the desktop window) has been successfully constructed.  At that point
// the SDLRenderer is ready so SetScene() can be called immediately.
//
// Scene structure:
//
//   scene_root_
//   └── search_overlay_node_          (hidden by default)
//       └── search_input_node_        (InputBox, "Search…")
//
// The search overlay draws a full-screen dim backdrop + centred frosted
// panel on every frame while search_visible_ is true.  It marks itself
// dirty at the end of its draw() so it is always re-rendered the following
// frame — necessary because the wallpaper pipeline CLEARs the framebuffer
// each frame.
// ---------------------------------------------------------------------------

void Desktop::buildDesktopScene()
{
    // ── Root ─────────────────────────────────────────────────────────────
    scene_root_ = scene_tree_.alloc();
    scene_tree_.setRoot(scene_root_);
    scene_tree_.node(scene_root_)->dirty_render = false;  // container only

    // ── Overlay ──────────────────────────────────────────────────────────
    search_overlay_node_ = scene_tree_.alloc();
    {
        RenderNode* ov   = scene_tree_.node(search_overlay_node_);
        ov->dirty_render = false;
        // draw() takes only RenderContext& — re-dirtying is done by tick()
        // calling markSubtreeDirty() each frame while the overlay is visible.
        ov->draw = [vis = &search_visible_](RenderContext& ctx) {
            if (!vis->get()) return;

            // Full-screen dim backdrop.
            ctx.drawRect(0.f, 0.f, ctx.viewport_w, ctx.viewport_h,
                         0.f, 0.f, 0.f, 0.50f);

            // Centred panel.
            constexpr float kPW = 600.f;
            constexpr float kPH = 68.f;
            const float px = (ctx.viewport_w - kPW) * 0.5f;
            const float py = ctx.viewport_h  * 0.25f;

            // 1 px border ring.
            ctx.drawRect(px - 13.f, py - 13.f, kPW + 26.f, kPH + 26.f,
                         0.30f, 0.30f, 0.32f, 0.85f);
            // Panel fill.
            ctx.drawRect(px - 12.f, py - 12.f, kPW + 24.f, kPH + 24.f,
                         0.12f, 0.12f, 0.14f, 0.95f);
        };
    }
    scene_tree_.appendChild(scene_root_, search_overlay_node_);

    // ── InputBox ─────────────────────────────────────────────────────────
    //
    // Layout is fixed against a 1280 × 800 reference viewport.
    // The flex layout pass (Phase 1) will make this respond to resize events.

    constexpr float kInputW = 560.f;
    constexpr float kInputH = 44.f;
    constexpr float kRefW   = 1280.f;
    constexpr float kRefH   = 800.f;
    const float     kPanelY = kRefH * 0.25f;

    search_input_node_ = widgets::inputBox(scene_tree_, {
        .placeholder = "Search\xe2\x80\xa6",   // UTF-8 for "Search…"
        .value       = &search_query_,
        .x           = (kRefW - kInputW) * 0.5f,
        .y           = kPanelY + (68.f - kInputH) * 0.5f,
        .w           = kInputW,
        .h           = kInputH,
    });
    scene_tree_.appendChild(search_overlay_node_, search_input_node_);

    // Bind: when search_visible_ changes, mark the overlay (and input)
    // render-dirty so the change is visible on the very next frame.
    scene_tree_.bind(search_visible_, search_overlay_node_);
    scene_tree_.bind(search_visible_, search_input_node_);

    // Attach to the desktop window that was just opened.
    if (auto w = get(1)) {
        w->setScene(&scene_tree_, scene_root_);
    }

    std::cout << "[Desktop] scene built\n";
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
