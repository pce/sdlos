// styleguide.cxx — Style Guide Keynote-style presentation behavior
//
// Included into the host via:
//   -DSDLOS_APP_BEHAVIOR="<abs-path>/styleguide.cxx"
//
// Architecture
// ────────────
//   4 pages, each loaded on-demand from data/pages/*.jade at runtime.
//   A floating nav bar auto-shows on interaction, auto-hides after 3 s.
//   Page 3 (scene3d) activates a Crystal Cluster GLB in the 3-D pre-pass.
//
// Page loading
// ────────────
//   loadPage(n) reads data/pages/<file>.jade, calls jade::parse() and
//   bindDrawCallbacks() on the result, replaces #page-container children,
//   then calls forceAllDirty() for a full re-render.
//
// Nav bar show/hide
// ─────────────────
//   The nav bar is col#nav-bar.sg-nav with jade attr height="0" overflow="hidden".
//   StyleApplier sets layout_props.height = 0 at parse time.  To show it we
//   directly assign layout_props.height = kNavH on the nav node AND reset the
//   page-container's n.h = 0 so the parent flex redistributes cleanly, then
//   call markLayoutDirty(nav) which propagates the dirty flag up to sg-root.
//   The next update() pass runs flexLayout on sg-main and correctly sizes
//   page-container to (window_h - kNavH) and nav bar to kNavH.
//
// 3-D scene
// ─────────
//   GltfScene is initialised once at startup.  The scene3d#sg-3d node in the
//   tree carries the Crystal Cluster path; we override its src attribute
//   before calling GltfScene::attach() so it resolves to the binary-dir copy.
//   On page 3 (scene3d.jade) we toggle display="block" on the scene3d node.
//   On all other pages display="none" so GltfScene::drawEntry() skips it.
//
// Nav events
// ──────────
//   sg:prev      — previous page
//   sg:next      — next page
//   sg:playpause — toggle auto-advance (5 s per slide)

#include "gltf/gltf_scene.hh"
#include "css_loader.hh"
#include "jade/jade_parser.hh"
#include "style_draw.hh"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// Constants
// ─────────────────────────────────────────────────────────────────────────────

namespace {

struct PageDesc {
    const char* file;    ///< relative to binary-dir (SDL_GetBasePath())
    const char* title;   ///< human-readable name for log messages
};

constexpr PageDesc k_pages[] = {
    { "data/pages/index.jade",   "Welcome"    },
    { "data/pages/style.jade",   "Styles"     },
    { "data/pages/layout.jade",  "Layout"     },
    { "data/pages/scene3d.jade", "3D Scene"   },
};

constexpr int   k_page_count    = static_cast<int>(sizeof(k_pages) / sizeof(k_pages[0]));
constexpr float k_nav_h         = 72.f;   ///< nav bar pixel height when shown
constexpr float k_nav_hide_time = 3.f;    ///< seconds until auto-hide
constexpr float k_auto_advance  = 5.f;    ///< seconds per slide in play mode
constexpr int   k_3d_page_idx   = k_page_count - 1;  ///< index of scene3d page

// ─────────────────────────────────────────────────────────────────────────────
// Module-level state
// ─────────────────────────────────────────────────────────────────────────────

pce::sdlos::gltf::GltfScene  g_scene;
pce::sdlos::RenderTree*      g_tree         = nullptr;
pce::sdlos::NodeHandle       g_root;
pce::sdlos::NodeHandle       g_page_container;  ///< #page-container
pce::sdlos::NodeHandle       g_nav_bar;          ///< #nav-bar

pce::sdlos::css::StyleSheet  g_scene_css;        ///< scene.css (3-D materials)

int    g_page       = 0;
bool   g_playing    = false;
float  g_nav_timer  = 0.f;    ///< counts down; <= 0 → nav hidden
float  g_play_timer = 0.f;    ///< counts down to next auto-advance
Uint64 g_last_ns    = 0;

std::string g_base_path;

// Orbit camera state (used on the scene3d page)
float  g_yaw_deg   =  20.f;
float  g_pitch_deg =  18.f;
float  g_dist      =   5.f;
bool   g_dragging  = false;
float  g_drag_mx   =  0.f;
float  g_drag_my   =  0.f;


static std::string readFile(const std::string& path)
{
    std::ifstream f(path);
    if (!f.is_open()) return {};
    std::ostringstream buf;
    buf << f.rdbuf();
    return buf.str();
}

static void clearChildren(pce::sdlos::RenderTree& tree,
                           pce::sdlos::NodeHandle  parent)
{
    for (;;) {
        const pce::sdlos::RenderNode* pn = tree.node(parent);
        if (!pn || !pn->child.valid()) break;
        tree.free(pn->child);
    }
}


static void updateOrbitCamera()
{
    // recompute eye position from yaw/pitch/dist
    constexpr float kD2R = 3.14159265f / 180.f;
    const float pitch_r  = g_pitch_deg * kD2R;
    const float yaw_r    = g_yaw_deg   * kD2R;
    const float ex = g_dist * std::cos(pitch_r) * std::sin(yaw_r);
    const float ey = g_dist * std::sin(pitch_r);
    const float ez = g_dist * std::cos(pitch_r) * std::cos(yaw_r);
    g_scene.camera().lookAt(ex, ey, ez,   0.f, 1.f, 0.f);
}


static void showNav()
{
    using namespace pce::sdlos;
    if (!g_tree || !g_nav_bar.valid()) return;

    g_nav_timer = k_nav_hide_time;

    // Reset page-container height so sg-main's flexLayout redistributes cleanly.
    if (g_page_container.valid())
        if (RenderNode* pc = g_tree->node(g_page_container))
            pc->h = 0.f;

    if (RenderNode* n = g_tree->node(g_nav_bar)) {
        n->layout_props.height = k_nav_h;
        n->h                   = k_nav_h;
        n->dirty_render        = true;
        g_tree->markLayoutDirty(g_nav_bar);
    }
}

static void hideNav()
{
    using namespace pce::sdlos;
    if (!g_tree || !g_nav_bar.valid()) return;

    // Reset page-container height so it can take the full window on next layout.
    if (g_page_container.valid())
        if (RenderNode* pc = g_tree->node(g_page_container))
            pc->h = 0.f;

    if (RenderNode* n = g_tree->node(g_nav_bar)) {
        n->layout_props.height = 0.f;
        n->h                   = 0.f;
        n->dirty_render        = true;
        g_tree->markLayoutDirty(g_nav_bar);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// updateDots — set active/inactive appearance for each page-indicator dot
// ─────────────────────────────────────────────────────────────────────────────

static void updateDots()
{
    using namespace pce::sdlos;
    if (!g_tree) return;

    for (int i = 0; i < k_page_count; ++i) {
        const std::string id = "dot-" + std::to_string(i);
        const NodeHandle h = g_tree->findById(g_root, id);
        if (!h.valid()) continue;

        if (RenderNode* n = g_tree->node(h)) {
            const bool active = (i == g_page);
            // Active dot: white, wide pill.  Inactive: dim, small square.
            n->setStyle("backgroundColor", active ? "#ffffffff" : "#ffffff33");
            n->setStyle("width",           active ? "24" : "8");
            n->layout_props.width = active ? 24.f : 8.f;
            n->w                  = active ? 24.f : 8.f;
            n->dirty_render       = true;
        }
    }

    // Dirty the dots container so it re-lays out the changed widths.
    const NodeHandle dots_h = g_tree->findById(g_root, "nav-dots");
    if (dots_h.valid())
        g_tree->markLayoutDirty(dots_h);
}

// ─────────────────────────────────────────────────────────────────────────────
// updatePlayBtn — reflect play/pause state on the ▶ / ⏸ button
// ─────────────────────────────────────────────────────────────────────────────

static void updatePlayBtn()
{
    using namespace pce::sdlos;
    if (!g_tree) return;

    const NodeHandle h = g_tree->findById(g_root, "nav-play");
    if (!h.valid()) return;

    if (RenderNode* n = g_tree->node(h)) {
        // UTF-8 for ▶ (U+25B6) and ⏸ (U+23F8)
        n->setStyle("text", g_playing ? "\xe2\x8f\xb8" : "\xe2\x96\xb6");
        n->dirty_render = true;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// update3DVisibility — show scene3d node only on the scene3d page
// ─────────────────────────────────────────────────────────────────────────────

static void update3DVisibility()
{
    using namespace pce::sdlos;
    if (!g_tree) return;

    const NodeHandle sg3d = g_tree->findById(g_root, "sg-3d");
    if (!sg3d.valid()) return;

    if (RenderNode* n = g_tree->node(sg3d)) {
        const bool show = (g_page == k_3d_page_idx);
        n->setStyle("display", show ? "block" : "none");
        g_tree->markDirty(sg3d);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// loadPage — replace page-container children with parsed jade from disk
// ─────────────────────────────────────────────────────────────────────────────

static void loadPage(int page)
{
    using namespace pce::sdlos;
    if (!g_tree || !g_page_container.valid()) return;

    g_page = std::clamp(page, 0, k_page_count - 1);

    // 1. Remove previous page content.
    clearChildren(*g_tree, g_page_container);

    // 2. Load jade source from binary dir.
    const std::string path = g_base_path + k_pages[g_page].file;
    const std::string src  = readFile(path);

    if (!src.empty()) {
        const NodeHandle nr = jade::parse(src, *g_tree);
        if (nr.valid()) {
            // Give the page root flex-grow=1 so it fills page-container.
            if (RenderNode* rn = g_tree->node(nr)) {
                rn->layout_props.flex_grow = 1.f;
                rn->dirty_render           = true;
            }
            bindDrawCallbacks(*g_tree, nr);
            g_tree->appendChild(g_page_container, nr);
        }
    } else {
        // Fallback: inline error label
        const NodeHandle err = g_tree->alloc();
        if (RenderNode* n = g_tree->node(err)) {
            n->setStyle("text",            "Page not found: " + path);
            n->setStyle("color",           "#ff4444");
            n->setStyle("fontSize",        "14");
            n->setStyle("padding",         "24");
            bindDrawCallbacks(*g_tree, err);
        }
        g_tree->appendChild(g_page_container, err);
    }

    // 3. Cascade a full re-layout + re-render from the root.
    g_tree->forceAllDirty(g_tree->root());

    // 4. Update chrome that reflects page state.
    update3DVisibility();
    updateDots();

    sdlos_log("[styleguide] page " + std::to_string(g_page)
              + " → " + k_pages[g_page].title
              + "  (" + path + ")");
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// jade_app_init
// ─────────────────────────────────────────────────────────────────────────────

void jade_app_init(pce::sdlos::RenderTree&               tree,
                   pce::sdlos::NodeHandle                 root,
                   pce::sdlos::IEventBus&                 bus,
                   pce::sdlos::SDLRenderer&               renderer,
                   std::function<bool(const SDL_Event&)>& out_handler)
{
    using namespace pce::sdlos;
    namespace fs = std::filesystem;

    // ── 0. Reset module state ─────────────────────────────────────────────
    g_tree         = &tree;
    g_root         = root;
    g_page         = 0;
    g_playing      = false;
    g_nav_timer    = 0.f;
    g_play_timer   = 0.f;
    g_dragging     = false;
    g_yaw_deg      = 20.f;
    g_pitch_deg    = 18.f;
    g_dist         =  5.f;
    g_last_ns      = SDL_GetTicksNS();

    const char* bp = SDL_GetBasePath();
    g_base_path    = bp ? bp : "";

    // ── 1. Cache frequently-used node handles ─────────────────────────────
    g_page_container = tree.findById(root, "page-container");
    g_nav_bar        = tree.findById(root, "nav-bar");

    if (!g_page_container.valid())
        sdlos_log("[styleguide] WARNING: #page-container not found — check jade");
    if (!g_nav_bar.valid())
        sdlos_log("[styleguide] WARNING: #nav-bar not found — check jade");

    // ── 2. GltfScene init ─────────────────────────────────────────────────
    const bool init_ok = g_scene.init(
        renderer.GetDevice(),
        renderer.GetShaderFormat(),
        g_base_path,
        SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM);

    if (!init_ok) {
        sdlos_log("[styleguide] GltfScene::init() failed — 3D page disabled");
    }

    if (init_ok) {
        renderer.setScene3DHook(
            [](SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* swap,
               float vw, float vh) noexcept {
                g_scene.render(cmd, swap, vw, vh);
            });
        renderer.setGpuPreShutdownHook([]() noexcept { g_scene.shutdown(); });
    }

    // ── 3. Override scene3d src before attach() ───────────────────────────
    //
    // jade_host's resolveAssetPaths() already ran and resolved the jade's
    // src="data/models/Crystal_Cluster.glb" relative to the JADE SOURCE
    // directory.  Override it here to point at the binary-dir copy that
    // CMake placed next to the executable.
    if (init_ok) {
        const NodeHandle sg3d = tree.findById(root, "sg-3d");
        if (sg3d.valid()) {
            if (RenderNode* n = tree.node(sg3d)) {
                n->setStyle("src",
                    g_base_path + "data/models/Crystal_Cluster.glb");
            }
        }
    }

    // ── 4. Attach scene (scans tree for scene3d nodes) ────────────────────
    if (init_ok) {
        const int mc = g_scene.attach(tree, root, g_base_path);
        if (mc > 0)
            sdlos_log("[styleguide] attached " + std::to_string(mc)
                      + " mesh primitive(s)");
        else
            sdlos_log("[styleguide] attach(): no meshes — "
                      "check data/models/Crystal_Cluster.glb");
    }

    // ── 5. Load scene.css — 3-D material overrides ───────────────────────
    if (init_ok) {
        const fs::path scene_css_path = fs::path(g_base_path) / "scene.css";
        if (fs::exists(scene_css_path)) {
            g_scene_css = pce::sdlos::css::load(scene_css_path.string());
            if (!g_scene_css.empty()) {
                g_scene_css.applyTo(tree, root);
                sdlos_log("[styleguide] scene.css: "
                          + std::to_string(g_scene_css.size()) + " rules");
            }
        } else {
            sdlos_log("[styleguide] WARNING: scene.css not found at "
                      + scene_css_path.string());
        }

        // Seed GPU material uniforms from freshly-applied CSS.
        g_scene.applyCSS(tree);

        // Perspective camera — initial orbital position.
        g_scene.camera().perspective(
            45.f,
            static_cast<float>(SDLOS_WIN_W) / static_cast<float>(SDLOS_WIN_H));
        updateOrbitCamera();
    }

    // ── 6. Load first page into #page-container ───────────────────────────
    loadPage(0);

    // ── 7. Show nav bar on startup (auto-hides after k_nav_hide_time) ─────
    showNav();
    updatePlayBtn();

    // ── 8. Per-frame update: timers + 3D tick ─────────────────────────────
    if (RenderNode* rn = tree.node(root)) {
        rn->update = []() noexcept {
            if (!g_tree) return;

            const Uint64 now = SDL_GetTicksNS();
            const float  dt  = std::min(
                static_cast<float>(now - g_last_ns) * 1.0e-9f, 0.05f);
            g_last_ns = now;

            // 3-D AABB projection (hover / hit-test).
            g_scene.tick(*g_tree,
                         static_cast<float>(SDLOS_WIN_W),
                         static_cast<float>(SDLOS_WIN_H));

            // Nav bar auto-hide timer.
            if (g_nav_timer > 0.f) {
                g_nav_timer -= dt;
                if (g_nav_timer <= 0.f)
                    hideNav();
            }

            // Auto-advance slideshow.
            if (g_playing) {
                g_play_timer -= dt;
                if (g_play_timer <= 0.f) {
                    g_play_timer = k_auto_advance;
                    loadPage((g_page + 1) % k_page_count);
                    showNav();
                }
            }
        };
    }

    // ── 9. Event bus subscriptions ────────────────────────────────────────

    bus.subscribe("sg:prev", [](const std::string&) {
        loadPage((g_page - 1 + k_page_count) % k_page_count);
        showNav();
    });

    bus.subscribe("sg:next", [](const std::string&) {
        loadPage((g_page + 1) % k_page_count);
        showNav();
    });

    bus.subscribe("sg:playpause", [](const std::string&) {
        g_playing = !g_playing;
        if (g_playing) g_play_timer = k_auto_advance;
        updatePlayBtn();
        showNav();
    });

    // ── 10. Raw event hook — orbit + nav-show ─────────────────────────────
    out_handler = [](const SDL_Event& ev) -> bool {

        switch (ev.type) {

        // Mouse motion — always show nav; on 3D page also orbit.
        case SDL_EVENT_MOUSE_MOTION:
            showNav();
            if (g_dragging) {
                g_yaw_deg   += ev.motion.xrel * 0.45f;
                g_pitch_deg  = std::clamp(
                    g_pitch_deg - ev.motion.yrel * 0.45f, -60.f, 80.f);
                updateOrbitCamera();
            }
            return false;   // let jade_host dispatch click / tickHover

        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            showNav();
            if (ev.button.button == SDL_BUTTON_RIGHT) {
                g_dragging = true;
                g_drag_mx  = ev.button.x;
                g_drag_my  = ev.button.y;
            }
            return false;

        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (ev.button.button == SDL_BUTTON_RIGHT)
                g_dragging = false;
            return false;

        case SDL_EVENT_MOUSE_WHEEL:
            showNav();
            g_dist = std::clamp(g_dist - ev.wheel.y * 0.3f, 1.5f, 20.f);
            updateOrbitCamera();
            return false;

        case SDL_EVENT_KEY_DOWN:
            showNav();
            switch (ev.key.scancode) {

            case SDL_SCANCODE_LEFT:
            case SDL_SCANCODE_UP:
                loadPage((g_page - 1 + k_page_count) % k_page_count);
                return true;

            case SDL_SCANCODE_RIGHT:
            case SDL_SCANCODE_DOWN:
                loadPage((g_page + 1) % k_page_count);
                return true;

            case SDL_SCANCODE_SPACE:
                g_playing = !g_playing;
                if (g_playing) g_play_timer = k_auto_advance;
                updatePlayBtn();
                return true;

            case SDL_SCANCODE_HOME:
                loadPage(0);
                return true;

            case SDL_SCANCODE_END:
                loadPage(k_page_count - 1);
                return true;

            default:
                return false;
            }

        default:
            return false;
        }
    };

    sdlos_log("[styleguide] init complete — "
              + std::to_string(tree.nodeCount()) + " nodes, "
              + std::to_string(k_page_count)     + " pages");
}
