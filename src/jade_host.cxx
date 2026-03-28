#ifndef SDLOS_JADE_ENTRY
#  error "SDLOS_JADE_ENTRY must be defined by the build system (path to .jade file)"
#endif

#ifndef SDLOS_APP_NAME
#  define SDLOS_APP_NAME "sdlos-app"
#endif

#ifndef SDLOS_WIN_W
#  define SDLOS_WIN_W 375
#endif
#ifndef SDLOS_WIN_H
#  define SDLOS_WIN_H 667
#endif

#include "render_tree.hh"
#include "sdl_renderer.hh"
#include "event_bus.hh"
#include "hit_test.hh"
#include "node_events.hh"
#include "jade/jade_parser.hh"
#include "style_draw.hh"
#include "i_event_bus.hh"
#include "debug/layout_debug.hh"
#include "css_loader.hh"

#include <SDL3/SDL.h>

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

// ---------------------------------------------------------------------------
// ConsoleState — in-app debug log ring buffer.
//
// Defined at file scope (before #include SDLOS_APP_BEHAVIOR) so that
// sdlos_log() is visible inside behaviour fragments at compile time.
// Toggle console visibility with ` (backtick).
// Toggle layout overlay with F1.
// ---------------------------------------------------------------------------

struct ConsoleState {
    static constexpr int   kMaxLines = 14;
    static constexpr float kLineH    = 15.f;
    static constexpr float kFontSz   = 11.f;
    static constexpr float kPad      = 5.f;
    static constexpr float kTitleH   = kLineH + kPad;
    static constexpr float kTotalH   = kTitleH + kMaxLines * kLineH + kPad;

    std::deque<std::string> lines;
    bool visible      = false;
    bool layout_debug = false;
};

static ConsoleState g_console;

// sdlos_log — callable from behaviour fragments included into this TU.
void sdlos_log(std::string_view msg)
{
    std::clog << "[sdlos] " << msg << "\n";
    g_console.lines.emplace_back(msg);
    while (static_cast<int>(g_console.lines.size()) > ConsoleState::kMaxLines)
        g_console.lines.pop_front();
}

// ---------------------------------------------------------------------------
// jade_app_init — per-app behaviour hook.
//
// Signature contract:
//   tree        — fully-parsed, style-bound RenderTree (safe to query / mutate)
//   root        — handle to the virtual root returned by jade::parse()
//   bus         — live EventBus; call bus.subscribe() here, not at global scope
//   renderer    — the active SDLRenderer; call SetFontPath() etc. when needed
//   out_handler — assign a raw-SDL-event handler here when you need to receive
//                 mouse / keyboard events directly (e.g. to drive a TextArea or
//                 NumberDragger).  The host stores it in SceneState; it is
//                 cleared automatically at the start of every loadScene() call
//                 so stale closures from the old tree can never fire.
//                 Return true from the handler to consume the event (host
//                 shortcuts are skipped); return false to let normal processing
//                 continue.
//
// Called once per scene load: on startup AND after every sdlos:navigate event.
// Do NOT call SDL_PollEvent or render from here.
//
// Manual font loading from a behaviour:
//   renderer.SetFontPath("data/fonts/Inter-Regular.ttf");
//
// Navigate to another jade file from a behaviour:
//   bus.publish("sdlos:navigate", "data/slides/slide2.jade");
// ---------------------------------------------------------------------------

#ifdef SDLOS_APP_BEHAVIOR
#  include SDLOS_APP_BEHAVIOR
#else
void jade_app_init(pce::sdlos::RenderTree&               /*tree*/,
                   pce::sdlos::NodeHandle                 /*root*/,
                   pce::sdlos::IEventBus&                 /*bus*/,
                   pce::sdlos::SDLRenderer&               /*renderer*/,
                   std::function<bool(const SDL_Event&)>& /*out_handler*/) {}
#endif

// ---------------------------------------------------------------------------
// File-local helpers
// ---------------------------------------------------------------------------

namespace {

namespace fs = std::filesystem;

std::atomic<bool> g_quit{false};

extern "C" void on_signal(int) noexcept { g_quit.store(true); }

[[nodiscard]] std::string readFile(const char* path)
{
    std::ifstream ifs(path);
    if (!ifs) {
        std::cerr << "[jade_host] cannot open: " << path << "\n";
        return {};
    }
    std::ostringstream ss;
    ss << ifs.rdbuf();
    return ss.str();
}

// Walk the tree and resolve every relative `src` attribute to an absolute
// path so ImageCache::ensureTexture() works regardless of the working dir.
static void resolveAssetPaths(pce::sdlos::RenderTree& tree,
                               pce::sdlos::NodeHandle   root,
                               const fs::path&          base_dir)
{
    if (!root.valid()) return;
    pce::sdlos::RenderNode* n = tree.node(root);
    if (!n) return;

    const auto src = n->style("src");
    if (!src.empty()) {
        const fs::path p(src);
        if (p.is_relative())
            n->setStyle("src", (base_dir / p).lexically_normal().string());
    }

    for (pce::sdlos::NodeHandle c = n->child; c.valid(); ) {
        pce::sdlos::RenderNode* cn = tree.node(c);
        if (!cn) break;
        const pce::sdlos::NodeHandle next = cn->sibling;
        resolveAssetPaths(tree, c, base_dir);
        c = next;
    }
}

// Try to load a font from <jade_dir>/data/fonts/ — the per-app font dir.
// Only apps that declare DATA_DIR in sdlos_jade_app() will have this.
// Does NOT gate on isReady(): loadFont() is what makes the renderer ready.
static void loadAppFonts(pce::sdlos::SDLRenderer& renderer,
                          const fs::path&           jade_dir)
{
    pce::sdlos::TextRenderer* tr = renderer.GetTextRenderer();
    if (!tr) return;

    const fs::path fonts_dir = jade_dir / "data" / "fonts";
    if (!fs::exists(fonts_dir)) return;

    const fs::path candidates[] = {
        fonts_dir / "InterVariable.ttf",
        fonts_dir / "Inter-Regular.ttf",
        fonts_dir / "Roboto-Regular.ttf",
        fonts_dir / "LiberationSans-Regular.ttf",
    };

    for (const auto& p : candidates) {
        if (fs::exists(p)) {
            if (tr->loadFont(p.string(), 16.f)) {
                sdlos_log("[jade_host] app font: " + p.filename().string());
                return;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// SceneState — owns the current jade scene.
//
// Lives in main() so its address (and the address of scene.tree) is stable
// for the entire process lifetime.  Overlay lambdas capture SceneState* so
// they keep working correctly after a scene reload: the pointer never changes,
// only the tree contents are replaced in-place by loadScene().
// ---------------------------------------------------------------------------

struct SceneState {
    std::unique_ptr<pce::sdlos::RenderTree> tree{
        std::make_unique<pce::sdlos::RenderTree>()};
    pce::sdlos::NodeHandle root{pce::sdlos::k_null_handle};
    pce::sdlos::NodeHandle layout_dbg_h{pce::sdlos::k_null_handle};
    pce::sdlos::NodeHandle console_h{pce::sdlos::k_null_handle};

    pce::sdlos::css::StyleSheet css_sheet;

    // Raw SDL event hook set by jade_app_init() via the out_handler parameter.
    // Stored here (not as a global) so its lifetime is exactly the scene's
    // lifetime — cleared at the top of every loadScene() before the old tree
    // is destroyed, so captured references can never dangle.
    std::function<bool(const SDL_Event&)> raw_event_handler;
};

// Attach the two host-owned overlay nodes (layout debug + console) on top
// of the scene.  Must be called after jade_app_init() so app nodes are below.
static void addOverlays(SceneState& scene)
{
    SceneState* sp = &scene;

    // ---- Layout-debug overlay -------------------------------------------
    scene.layout_dbg_h = scene.tree->alloc();
    if (pce::sdlos::RenderNode* nd = scene.tree->node(scene.layout_dbg_h)) {
        nd->draw = [sp](pce::sdlos::RenderContext& ctx) {
            if (!g_console.layout_debug) return;
            pce::sdlos::debug::LayoutDebugConfig cfg;
            cfg.show_none    = false;
            cfg.show_labels  = true;
            cfg.label_size   = 10.f;
            cfg.fill_alpha   = 0.07f;
            cfg.border_width = 1.0f;
            pce::sdlos::debug::drawLayoutDebug(ctx, *sp->tree, sp->root, cfg);
        };
        nd->update = [sp]() {
            if (!g_console.layout_debug) return;
            if (pce::sdlos::RenderNode* self = sp->tree->node(sp->layout_dbg_h))
                self->dirty_render = true;
        };
        scene.tree->appendChild(scene.root, scene.layout_dbg_h);
    }

    // ---- Console overlay ------------------------------------------------
    scene.console_h = scene.tree->alloc();
    if (pce::sdlos::RenderNode* nd = scene.tree->node(scene.console_h)) {
        nd->draw = [](pce::sdlos::RenderContext& ctx) {
            if (!g_console.visible) return;

            const float vw = ctx.viewport_w;
            const float vh = ctx.viewport_h;
            const float y0 = vh - ConsoleState::kTotalH;

            // Background + top border + title bar
            ctx.drawRect(0.f, y0, vw, ConsoleState::kTotalH,
                         0.04f, 0.07f, 0.04f, 0.92f);
            ctx.drawRect(0.f, y0, vw, 1.f,
                         0.20f, 0.85f, 0.20f, 0.70f);
            ctx.drawRect(0.f, y0, vw, ConsoleState::kTitleH,
                         0.00f, 0.30f, 0.00f, 0.80f);
            ctx.drawText("sdlos console   [` ] show/hide   [F1] layout overlay",
                         ConsoleState::kPad, y0 + 2.f,
                         ConsoleState::kFontSz,
                         0.40f, 1.00f, 0.40f, 1.00f);

            // Log lines (oldest first = top of panel)
            float ly = y0 + ConsoleState::kTitleH + 2.f;
            for (const auto& line : g_console.lines) {
                ctx.drawText(line,
                             ConsoleState::kPad, ly,
                             ConsoleState::kFontSz,
                             0.72f, 0.95f, 0.72f, 1.00f);
                ly += ConsoleState::kLineH;
            }
        };
        nd->update = [sp]() {
            // Only dirty when the console is open — keeps idle-frame skip
            // working when the panel is hidden.
            if (!g_console.visible) return;
            if (pce::sdlos::RenderNode* self = sp->tree->node(sp->console_h))
                self->dirty_render = true;
        };
        scene.tree->appendChild(scene.root, scene.console_h);
    }
}

// ---------------------------------------------------------------------------
// loadScene — parse a jade file and fully wire up the scene.
//
// Used for both the initial load and every sdlos:navigate transition.
//
//  1. Detach the old scene from the renderer (safe to call with null).
//  2. Reset the EventBus — drops all old behaviour callbacks so they cannot
//     fire against a tree that is about to be destroyed.
//  3. Re-subscribe the host's sdlos:navigate handler on the fresh bus so the
//     new behaviour can trigger further navigations.
//  4. Replace the RenderTree in-place (SceneState address stays stable so
//     overlay lambdas capturing SceneState* continue to work).
//  5. Parse jade → attach to renderer → resolve asset paths → load app fonts.
//  6. Bind draw/node callbacks → call jade_app_init (behaviour gets renderer).
//  7. Handle _font / _font_size jade attributes (behaviour has the last word).
//  8. Add host overlays on top of the app scene.
// ---------------------------------------------------------------------------
static bool loadScene(const std::string&       jade_path,
                       SceneState&              scene,
                       pce::sdlos::SDLRenderer& renderer,
                       pce::sdlos::EventBus&    events,
                       std::string&             next_jade_path)
{
    // 1. Detach old scene before destroying the tree.
    renderer.SetScene(nullptr, pce::sdlos::k_null_handle);

    // 2. Drop all subscriptions — old lambdas capture refs into the old tree.
    events.reset();

    // 2b. Clear the raw-event hook — it may capture refs into the old tree.
    scene.raw_event_handler = nullptr;

    // 2c. Clear 3D hooks — lambdas may capture old app state.
    renderer.setScene3DHook(nullptr);
    renderer.setGpuPreShutdownHook(nullptr);

    // 3. Re-subscribe the host navigation handler on the fresh bus.
    //    The new behaviour can now call:
    //      bus.publish("sdlos:navigate", "path/to/next.jade");
    events.subscribe("sdlos:navigate", [&next_jade_path](const std::string& path) {
        next_jade_path = path;
        sdlos_log("[jade_host] navigate → " + path);
    });

    // 4. Replace the tree — unique_ptr swap keeps SceneState address stable.
    scene.tree         = std::make_unique<pce::sdlos::RenderTree>();
    scene.root         = pce::sdlos::k_null_handle;
    scene.layout_dbg_h = pce::sdlos::k_null_handle;
    scene.console_h    = pce::sdlos::k_null_handle;

    // 5a. Parse jade source.
    const std::string source = readFile(jade_path.c_str());
    if (source.empty()) {
        std::cerr << "[jade_host] empty or missing jade file: " << jade_path << "\n";
        return false;
    }

    scene.root = pce::sdlos::jade::parse(source, *scene.tree);
    if (!scene.root.valid()) {
        std::cerr << "[jade_host] jade parse produced no root: " << jade_path << "\n";
        return false;
    }

    // 5b. Attach to renderer (layout sizing needs the scene to be set).
    renderer.SetScene(scene.tree.get(), scene.root);

    // 5c. Resolve relative src= attributes before bindDrawCallbacks().
    const fs::path jade_dir = fs::path(jade_path).parent_path();
    resolveAssetPaths(*scene.tree, scene.root, jade_dir);

    // 5d. Load a font from the jade app's own data/fonts/ if present.
    //     Only apps that bundle a DATA_DIR will have this directory.
    loadAppFonts(renderer, jade_dir);

    scene.css_sheet = {};
    {
        const fs::path css_path = fs::path(jade_path).replace_extension(".css");
        if (fs::exists(css_path)) {
            scene.css_sheet = pce::sdlos::css::load(css_path.string());
            if (!scene.css_sheet.empty()) {
                scene.css_sheet.applyTo(*scene.tree, scene.root);
                sdlos_log("[jade_host] css: " + css_path.filename().string()
                          + "  rules=" + std::to_string(scene.css_sheet.size()));
            }
        }
    }

    // 6. Bind draw callbacks and node-event wiring, then call the behaviour.
    pce::sdlos::bindDrawCallbacks(*scene.tree, scene.root);
    pce::sdlos::bindNodeEvents(*scene.tree, scene.root, events);
    jade_app_init(*scene.tree, scene.root, events, renderer, scene.raw_event_handler);

    if (!scene.css_sheet.empty()) {
        scene.css_sheet.buildHover(*scene.tree, scene.root);
        if (!scene.css_sheet.hover.empty())
            sdlos_log("[jade_host] css hover: "
                      + std::to_string(scene.css_sheet.hover.size()) + " entries");
        scene.css_sheet.buildActive(*scene.tree, scene.root);
        if (!scene.css_sheet.active_entries.empty())
            sdlos_log("[jade_host] css active: "
                      + std::to_string(scene.css_sheet.active_entries.size()) + " entries");
    }

    // 7. _font / _font_size jade attributes — behaviour has the last word.
    //    Declare a font in jade like a CSS hint:
    //      app(_font="data/fonts/Inter-Regular.ttf" _font_size="16")
    //    Or set it inside jade_app_init():
    //      tree.node(root)->setStyle("_font", "data/fonts/MyFont.ttf");
    if (pce::sdlos::RenderNode* rn = scene.tree->node(scene.root)) {
        const std::string fp{ rn->style("_font") };
        if (!fp.empty()) {
            float sz = 17.f;
            const std::string sz_s{ rn->style("_font_size") };
            if (!sz_s.empty()) {
                try { sz = std::stof(sz_s); } catch (...) {}
            }
            renderer.SetFontPath(fp, sz);
        }
    }

    // 8. Host overlays always render on top of the app scene.
    addOverlays(scene);

    sdlos_log(std::string("[jade_host] loaded '")
              + jade_path + "' — "
              + std::to_string(scene.tree->nodeCount()) + " nodes");
    return true;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[])
{
    // Jade file can be overridden at runtime: ./shade other.jade
    const char* jade_path = (argc > 1) ? argv[1] : SDLOS_JADE_ENTRY;
    std::cout << "[jade_host] jade: " << jade_path << "\n";

    // ---- SDL init --------------------------------------------------------
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_CAMERA)) {
        std::cerr << "[jade_host] SDL_Init failed: " << SDL_GetError() << "\n";
        return EXIT_FAILURE;
    }

    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);

    // ---- Window ----------------------------------------------------------
#ifdef BUILD_TYPE_DEBUG
    SDL_Window* window = SDL_CreateWindow(
        SDLOS_APP_NAME, SDLOS_WIN_W, SDLOS_WIN_H,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
#else
    SDL_Window* window = SDL_CreateWindow(
        SDLOS_APP_NAME, 0, 0,
        static_cast<SDL_WindowFlags>(SDL_WINDOW_FULLSCREEN));
#endif

    if (!window) {
        std::cerr << "[jade_host] SDL_CreateWindow failed: " << SDL_GetError() << "\n";
        SDL_Quit();
        return EXIT_FAILURE;
    }

    // ---- Renderer --------------------------------------------------------
    // HiDPI pixel scale is owned by SDLRenderer: computed inside Initialize()
    // and refreshed on display-change events via RefreshPixelScale().
    pce::sdlos::SDLRenderer renderer;
    if (!renderer.Initialize(window)) {
        std::cerr << "[jade_host] renderer init failed\n";
        SDL_DestroyWindow(window);
        SDL_Quit();
        return EXIT_FAILURE;
    }

    sdlos_log(std::string("[jade_host] pixel scale ")
              + std::to_string(renderer.pixelScaleX()) + "x"
              + std::to_string(renderer.pixelScaleY()));

    // ---- Data base path --------------------------------------------------
    // SDL_GetBasePath() → directory of the running binary with trailing '/'.
    // CMake copies each app's data/ folder there as a post-build step, so
    // relative src= paths in jade and data/shaders/ paths resolve correctly.
    // Stored in base_path for use in runtime navigation path resolution.
    std::string base_path;
    {
        const char* sdl_base = SDL_GetBasePath();
        base_path = sdl_base ? sdl_base : "";
        if (!base_path.empty()) {
            renderer.SetDataBasePath(base_path);
            sdlos_log("[jade_host] base: " + base_path);
        }
    }

    // ---- Scene state and EventBus ----------------------------------------
    // SceneState lives here so its address (and &scene.tree) is stable for
    // the entire process lifetime.  Overlay lambdas capture SceneState*.
    SceneState scene;
    pce::sdlos::EventBus events;

    // Set by the sdlos:navigate bus handler (subscribed inside loadScene).
    // Checked at the bottom of the main loop after every render tick.
    std::string next_jade_path;

    if (!loadScene(jade_path, scene, renderer, events, next_jade_path)) {
        renderer.Shutdown();
        SDL_DestroyWindow(window);
        SDL_Quit();
        return EXIT_FAILURE;
    }

    std::cout << "[jade_host] running '" << SDLOS_APP_NAME
              << "' — " << scene.tree->nodeCount() << " nodes"
              << "  (` = console, F1 = layout debug)\n";

    // ---- Main loop -------------------------------------------------------
    SDL_Event event;

    while (!g_quit.load()) {

        // ---- Event pump --------------------------------------------------
        while (SDL_PollEvent(&event)) {

            if (event.type == SDL_EVENT_QUIT) {
                g_quit.store(true);
                break;
            }

            // HiDPI — refresh scale when window moves to a different display.
            if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED ||
                event.type == SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED)
            {
                renderer.RefreshPixelScale();
                sdlos_log(std::string("[jade_host] scale updated to ")
                          + std::to_string(renderer.pixelScaleX()) + "x"
                          + std::to_string(renderer.pixelScaleY()));
            }

            if (event.type == SDL_EVENT_MOUSE_MOTION)
                scene.css_sheet.tickHover(*scene.tree,
                    event.motion.x * renderer.pixelScaleX(),
                    event.motion.y * renderer.pixelScaleY());

            if (event.type == SDL_EVENT_WINDOW_MOUSE_LEAVE)
                scene.css_sheet.tickHover(*scene.tree, -1.f, -1.f);

            // Behavior raw-event hook — runs before host keyboard handlers.
            // Returns true only when the behavior consumed the event (e.g. a
            // focused TextArea absorbed a KEY_DOWN).  Mouse events return false
            // so dispatchClick always runs below.
            const bool behavior_consumed =
                scene.raw_event_handler && scene.raw_event_handler(event);

            // Mouse clicks — SDL logical coords → physical layout coords.
            if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
                event.button.button == SDL_BUTTON_LEFT)
            {
                const float px = event.button.x * renderer.pixelScaleX();
                const float py = event.button.y * renderer.pixelScaleY();

                const pce::sdlos::NodeHandle hit =
                    pce::sdlos::dispatchClick(*scene.tree, scene.root,
                                              px, py, events, &scene.css_sheet);

                // When the console is open, log what was hit for debugging.
                if (g_console.visible && hit.valid()) {
                    const pce::sdlos::RenderNode* hn = scene.tree->node(hit);
                    if (hn) {
                        const auto id  = hn->style("id");
                        const auto cls = hn->style("class");
                        const auto oc  = hn->style("onclick");
                        const auto dv  = hn->style("data-value");
                        std::string msg = "[click] ";
                        msg += std::to_string(static_cast<int>(px));
                        msg += ",";
                        msg += std::to_string(static_cast<int>(py));
                        if (!id.empty())  { msg += " id=";  msg += id;  }
                        if (!cls.empty()) { msg += " cls="; msg += cls; }
                        if (!oc.empty())  { msg += " -> ";  msg += oc;
                            if (!dv.empty()) { msg += "("; msg += dv; msg += ")"; }
                        }
                        sdlos_log(msg);
                    }
                }
            }

            // Keyboard shortcuts — skipped when the behavior already consumed
            // the key (e.g. a focused TextArea editor is active).
            if (!behavior_consumed && event.type == SDL_EVENT_KEY_DOWN) {
                const SDL_Scancode sc = event.key.scancode;

                // ` — toggle console visibility.
                if (sc == SDL_SCANCODE_GRAVE) {
                    g_console.visible = !g_console.visible;
                    if (pce::sdlos::RenderNode* nd = scene.tree->node(scene.console_h))
                        nd->dirty_render = true;
                }

                // F1 — toggle layout debug overlay.
                else if (sc == SDL_SCANCODE_F1) {
                    g_console.layout_debug = !g_console.layout_debug;
                    g_console.visible      = true;
                    sdlos_log(std::string("[layout] overlay ")
                              + (g_console.layout_debug ? "ON" : "OFF"));
                    if (pce::sdlos::RenderNode* nd = scene.tree->node(scene.layout_dbg_h))
                        nd->dirty_render = true;
                    if (pce::sdlos::RenderNode* nd = scene.tree->node(scene.console_h))
                        nd->dirty_render = true;
                }
            }
        }

        // ---- Render ------------------------------------------------------
        const double t = static_cast<double>(SDL_GetTicks()) * 0.001;
        renderer.Render(t);

        // ---- Scene navigation --------------------------------------------
        // A behaviour publishes "sdlos:navigate" with a jade file path.
        // The bus handler (subscribed in loadScene) sets next_jade_path.
        // We act here — after the render pass, before the next frame — so
        // the old scene is fully done before the tree is replaced.
        //
        // Navigation in a behaviour looks like:
        //   bus.publish("sdlos:navigate", "data/slides/slide2.jade");
        //   bus.publish("sdlos:navigate", "/absolute/path/to/settings.jade");
        //
        // Relative paths are resolved against the binary directory (base_path)
        // so they follow the same convention as data/ asset paths.
        if (!next_jade_path.empty()) {
            const std::string target =
                fs::path(next_jade_path).is_absolute()
                    ? next_jade_path
                    : base_path + next_jade_path;
            next_jade_path.clear();

            sdlos_log("[jade_host] loading scene: " + target);
            if (!loadScene(target, scene, renderer, events, next_jade_path)) {
                // Target failed — reload the original so the host is never
                // left with no scene attached to the renderer.
                sdlos_log("[jade_host] scene load failed, reloading: "
                          + std::string(jade_path));
                loadScene(jade_path, scene, renderer, events, next_jade_path);
            }

            std::cout << "[jade_host] scene → " << target
                      << "  (" << scene.tree->nodeCount() << " nodes)\n";
        }

        SDL_Delay(16);   // ~60 fps; replace with vsync / frame timing later
    }

    // ---- Shutdown --------------------------------------------------------
    renderer.Shutdown();
    SDL_DestroyWindow(window);
    SDL_Quit();

    std::cout << "[jade_host] '" << SDLOS_APP_NAME << "' exited cleanly\n";
    return EXIT_SUCCESS;
}
