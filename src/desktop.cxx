#include "desktop.hh"
#include "sdl_renderer.hh"
#include "debug/layout_debug.hh"

#include <expected>
#include <format>
#include <iostream>
#include <string>

#include <SDL3/SDL.h>

namespace pce::sdlos {

// ---------------------------------------------------------------------------
// Window
// ---------------------------------------------------------------------------

// Private constructor — only called by Window::create().
// Does not touch SDL or the GPU; all real initialisation happens in create().
Window::Window(int id, const std::string& title, int w, int h)
    : id_(id), title_(title), width_(w), height_(h)
{}

// ---------------------------------------------------------------------------
// Window::create
// ---------------------------------------------------------------------------

std::expected<std::unique_ptr<Window>, std::string>
Window::create(int id, const std::string& title,
               int x, int y, int w, int h,
               SDL_WindowFlags flags)
{
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

    // Direct new via private constructor — std::make_unique can't reach it.
    auto win = std::unique_ptr<Window>(new Window(id, title, actual_w, actual_h));
    win->sdl_window_.reset(raw);

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
    if (is_minimized_) return;
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

// ---------------------------------------------------------------------------
// Desktop — private helpers
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Desktop
// ---------------------------------------------------------------------------

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

    windows_.erase(it);
}

// ---------------------------------------------------------------------------
// Main-loop hooks
// ---------------------------------------------------------------------------

void Desktop::handleEvent(SDL_Event* event)
{
    if (!event) return;

    // Global keyboard shortcuts
    if (event->type == SDL_EVENT_KEY_DOWN) {
        const SDL_Keycode key      = event->key.key;
        const SDL_Keymod  mod      = event->key.mod;
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
        if (key == SDLK_F1) {
            toggleLayoutDebug();
            std::cout << "[Desktop] layout debug overlay: "
                      << (debug_layout_ ? "ON" : "OFF") << "\n";
            return;
        }
    }

    // When the search overlay is open, forward input events to the InputBox
    // before falling through to window-event routing below.
    if (search_visible_.get() &&
        (event->type == SDL_EVENT_KEY_DOWN      ||
         event->type == SDL_EVENT_TEXT_INPUT    ||
         event->type == SDL_EVENT_MOUSE_BUTTON_DOWN)) {
        routeOverlayEvent(*event);
    }

    if (event->type < SDL_EVENT_WINDOW_FIRST ||
        event->type > SDL_EVENT_WINDOW_LAST)
        return;

    const auto sdlIt = sdl_to_id_.find(event->window.windowID);
    if (sdlIt == sdl_to_id_.end()) return;

    const auto winIt = windows_.find(sdlIt->second);
    if (winIt != windows_.end()) {
        winIt->second->handleEvent(*event);
    }

    // Viewport resize → re-flow the scene tree
    if (event->type == SDL_EVENT_WINDOW_RESIZED &&
        sdlIt->second == 1 &&          // desktop window only
        scene_root_.valid())
    {
        scene_tree_.markLayoutDirty(scene_root_);
    }
}

void Desktop::tick()
{
    // The wallpaper pipeline uses SDL_GPU_LOADOP_CLEAR each frame — overlay
    // nodes must re-emit draw commands every frame or they disappear after the
    // clear.  markSubtreeDirty() propagates downward (opposite of
    // RenderTree::markDirty, which propagates upward to ancestors).
    if (search_visible_.get() && search_overlay_node_.valid()) {
        markSubtreeDirty(scene_tree_, search_overlay_node_);
    }
    if (debug_layout_ && layout_debug_node_.valid()) {
        markSubtreeDirty(scene_tree_, layout_debug_node_);
    }
}

void Desktop::render()
{
    const double t = static_cast<double>(SDL_GetTicks()) * 0.001;
    // Re-attach on every frame: SetScene stores non-owning pointers so this
    // is cheap, and it ensures windows opened after buildDesktopScene() pick
    // up the scene without an explicit second call.
    if (auto w = get(1); w) {
        w->setScene(&scene_tree_, scene_root_);
    }
    for (auto& [id, win] : windows_) {
        win->render(t);
    }
}

// ---------------------------------------------------------------------------
// Search overlay
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

void Desktop::routeOverlayEvent(const SDL_Event& event)
{
    if (!search_input_node_.valid()) return;
    widgets::inputBoxHandleEvent(scene_tree_, search_input_node_, event);
}

// ---------------------------------------------------------------------------
// buildDesktopScene
//
// Called automatically from Desktop::open() when the first window (id=1) is
// constructed.  Scene structure:
//
//   scene_root_
//   └── search_overlay_node_          (hidden by default)
//       └── search_input_node_        (InputBox, "Search…")
//   └── layout_debug_node_            (F1 toggle, painted last / on top)
//
// The wallpaper pipeline CLEARs the framebuffer each frame — the overlay
// re-dirties itself via tick() → markSubtreeDirty() so it is always
// re-rendered the following frame.
// ---------------------------------------------------------------------------

void Desktop::buildDesktopScene()
{
    // ── Root ─────────────────────────────────────────────────────────────
    scene_root_ = scene_tree_.alloc();
    scene_tree_.setRoot(scene_root_);
    {
        RenderNode* r = scene_tree_.node(scene_root_);
        r->dirty_render = false;          // container only
        r->layout_kind  = LayoutKind::FlexColumn;
        // w/h are intentionally left at 0 here.  SDLRenderer::Render()
        // writes the physical swapchain pixel dimensions into these fields
        // before every beginFrame() call.  That makes the swapchain the
        // single authoritative "canvas" — no hardcoded resolution anywhere.
    }

    // ── Overlay ──────────────────────────────────────────────────────────
    search_overlay_node_ = scene_tree_.alloc();
    {
        RenderNode* ov   = scene_tree_.node(search_overlay_node_);
        ov->dirty_render = false;
        // update() runs unconditionally every frame (RenderTree::update()
        // calls it for every node regardless of dirty flags).  Mirror the
        // root dimensions so layout passes and the debug overlay get accurate
        // sizes without hardcoding 1280×800.
        ov->update = [&tree  = scene_tree_,
                      ov_h   = search_overlay_node_,
                      root_h = scene_root_]()
        {
            const RenderNode* root = tree.node(root_h);
            RenderNode*       ov   = tree.node(ov_h);
            if (root && ov) {
                ov->w = root->w;
                ov->h = root->h;
            }
        };
        // draw() re-dirtying is handled by tick() → markSubtreeDirty().
        ov->draw = [vis = &search_visible_](RenderContext& ctx) {
            if (!vis->get()) return;

            ctx.drawRect(0.f, 0.f, ctx.viewport_w, ctx.viewport_h,
                         0.f, 0.f, 0.f, 0.50f);

            constexpr float kPW = 600.f;
            constexpr float kPH = 68.f;
            const float px = (ctx.viewport_w - kPW) * 0.5f;
            const float py = ctx.viewport_h  * 0.25f;

            ctx.drawRect(px - 13.f, py - 13.f, kPW + 26.f, kPH + 26.f,
                         0.30f, 0.30f, 0.32f, 0.85f);
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

    // Bind: when search_visible_ changes, mark overlay + input dirty so the
    // change is visible on the very next frame.
    scene_tree_.bind(search_visible_, search_overlay_node_);
    scene_tree_.bind(search_visible_, search_input_node_);

    if (auto w = get(1)) {
        w->setScene(&scene_tree_, scene_root_);
    }

    // ── Layout debug overlay (F1 toggle) ─────────────────────────────────
    //
    // Appended last so it paints on top of every other node.
    // The draw callback captures references; all referenced members outlive
    // the lambda (Desktop owns the tree).
    // cfg.skip = layout_debug_node_ prevents the overlay from drawing a
    // debug box around itself.
    layout_debug_node_ = scene_tree_.alloc();
    {
        RenderNode* d = scene_tree_.node(layout_debug_node_);
        d->dirty_render = false;

        d->draw = [&debug_flag = debug_layout_,
                   &tree      = scene_tree_,
                   &root      = scene_root_,
                   skip       = layout_debug_node_](RenderContext& ctx)
        {
            if (!debug_flag) return;
            // show_none = true: nodes without a LayoutKind still get a gray
            // box, making the overlay useful before the full layout migration.
            debug::drawLayoutDebug(ctx, tree, root, {
                .show_none    = true,
                .skip         = skip,
            });
        };
    }
    scene_tree_.appendChild(scene_root_, layout_debug_node_);

    std::cout << "[Desktop] scene built\n";
    std::cout << "[Desktop] - F1 toggles layout debug overlay\n";
    std::cout << "[Desktop] - CTRL+SPACE toggles Search overlay\n";
    std::cout << "[Desktop] scene_root layout_kind = FlexColumn\n";
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
