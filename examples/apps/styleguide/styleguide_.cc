//  Style Guide Keynote-style presentation behavior
//
// Included into the host via:
//   -DSDLOS_APP_BEHAVIOR="<abs-path>/styleguide.cc"
//
// Architecture
//   4 pages, each loaded on-demand from data/pages/*.jade at runtime.
//   A floating nav bar auto-shows on interaction, auto-hides after 3 s.
//   Page 3 (scene3d) activates a Crystal Cluster GLB in the 3-D pre-pass.
//
// Page loading
//   loadPage(n) reads data/pages/<file>.jade, calls jade::parse() and
//   bindDrawCallbacks() on the result, replaces #page-container children,
//   then calls forceAllDirty() for a full re-render.
//
// Nav bar show/hide
//   The nav bar is col#nav-bar.sg-nav with jade attr height="0" overflow="hidden".
//   StyleApplier sets layout_props.height = 0 at parse time.  To show it we
//   directly assign layout_props.height = kNavH on the nav node AND reset the
//   page-container's n.h = 0 so the parent flex redistributes cleanly, then
//   call markLayoutDirty(nav) which propagates the dirty flag up to sg-root.
//   The next update() pass runs flexLayout on sg-main and correctly sizes
//   page-container to (window_h - kNavH) and nav bar to kNavH.
//
// 3-D scene
//   GltfScene is initialised once at startup.  The scene3d#sg-3d node in the
//   tree carries the Crystal Cluster path; we override its src attribute
//   before calling GltfScene::attach() so it resolves to the binary-dir copy.
//   On page 3 (scene3d.jade) we toggle display="block" on the scene3d node.
//   On all other pages display="none" so GltfScene::drawEntry() skips it.
//
// Nav events
//   sg:prev      — previous page
//   sg:next      — next page
//   sg:playpause — toggle auto-advance (5 s per slide in play mode)

#include "gltf/gltf_scene.h"
#include "css_loader.h"
#include "jade/jade_parser.h"
#include "style_draw.h"
#include "i_event_bus.h"
#include "sdl_renderer.h"
#include "core/animated.h"
#include "core/easing.h"

#include "vfs/vfs.h"

// Function provided by the host for logging.
extern void sdlos_log(std::string_view msg);

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>


// Constants

namespace {

// Crystal animation state — tracks rotation and other animated properties
struct CrystalAnimState {
    pce::sdlos::Animated<float> rotation_z;  // Z-axis rotation (spin)
    pce::sdlos::Animated<float> rotation_y;  // Y-axis rotation (tilt)
    bool auto_spin = true;                    // toggle continuous spin
    bool boost_pending = false;               // flag for click-based speed boost
};

struct AnimalAnimState {
    pce::sdlos::Animated<float> hop_y;
    pce::sdlos::Animated<float> roll_x;
    pce::sdlos::NodeHandle      parrot_h = pce::sdlos::k_null_handle;

    int current_animal_idx = 0;
    bool name_revealed = false;
    pce::sdlos::Animated<float> reveal_y; // 0 to 1 for name reveal animation
};

struct PageDesc {
    const char* file;    ///< relative to binary-dir (SDL_GetBasePath())
    const char* title;   ///< human-readable name for log messages
};

constexpr PageDesc k_pages[] = {
    { "data/pages/index.jade",   "Welcome"    },
    { "data/pages/style.jade",   "Styles"     },
    { "data/pages/layout.jade",  "Layout"     },
    { "data/pages/scene3d.jade", "3D Scene"   },
    { "data/pages/animal_farm.jade", "Animal Farm" },
};

const char* k_animal_models[] = {
    "data/models/animal_farm/animal-beaver.glb",
    "data/models/animal_farm/animal-bee.glb",
    "data/models/animal_farm/animal-bunny.glb",
    "data/models/animal_farm/animal-cat.glb",
    "data/models/animal_farm/animal-caterpillar.glb",
    "data/models/animal_farm/animal-chick.glb",
    "data/models/animal_farm/animal-cow.glb",
    "data/models/animal_farm/animal-crab.glb",
    "data/models/animal_farm/animal-deer.glb",
    "data/models/animal_farm/animal-dog.glb",
    "data/models/animal_farm/animal-elephant.glb",
    "data/models/animal_farm/animal-fish.glb",
    "data/models/animal_farm/animal-fox.glb",
    "data/models/animal_farm/animal-giraffe.glb",
    "data/models/animal_farm/animal-hog.glb",
    "data/models/animal_farm/animal-koala.glb",
    "data/models/animal_farm/animal-lion.glb",
    "data/models/animal_farm/animal-monkey.glb",
    "data/models/animal_farm/animal-panda.glb",
    "data/models/animal_farm/animal-parrot.glb",
    "data/models/animal_farm/animal-penguin.glb",
    "data/models/animal_farm/animal-pig.glb",
    "data/models/animal_farm/animal-polar.glb",
    "data/models/animal_farm/animal-tiger.glb"
};

const char* k_animal_names[] = {
    "Beaver", "Bee", "Bunny", "Cat", "Caterpillar", "Chick", "Cow", "Crab", "Deer", "Dog",
    "Elephant", "Fish", "Fox", "Giraffe", "Hog", "Koala", "Lion", "Monkey", "Panda", "Parrot",
    "Penguin", "Pig", "Polar Bear", "Tiger"
};

constexpr int k_animal_count = static_cast<int>(sizeof(k_animal_models) / sizeof(k_animal_models[0]));

constexpr int   k_page_count    = static_cast<int>(sizeof(k_pages) / sizeof(k_pages[0]));
constexpr float k_nav_h         = 72.f;   ///< nav bar pixel height when shown
constexpr float k_nav_hide_time = 3.f;    ///< seconds until auto-hide
constexpr float k_auto_advance  = 5.f;    ///< seconds per slide in play mode
constexpr int   k_3d_page_idx   = k_page_count - 2;  ///< index of scene3d page
constexpr int   k_animal_page_idx = k_page_count - 1;

// Page-transition fade durations (seconds).
// k_fade_out: old page fades to black.  k_fade_in: new page revealed.
constexpr float k_fade_out_dur  = 0.22f;
constexpr float k_fade_in_dur   = 0.35f;


// Module-level state


pce::sdlos::gltf::GltfScene  g_scene;
pce::sdlos::RenderTree*      g_tree         = nullptr;
pce::sdlos::NodeHandle       g_root;
pce::sdlos::NodeHandle       g_page_container;  ///< #page-container
pce::sdlos::NodeHandle       g_nav_bar;          ///< #nav-bar
pce::sdlos::NodeHandle       g_fade_overlay_h;  ///< full-screen LayoutKind::None overlay
pce::sdlos::NodeHandle       g_crystal_h;       ///< #sg-crystal mesh proxy handle

pce::vfs::Vfs* g_vfs = nullptr;

int    g_page       = 0;
bool   g_playing    = false;
float  g_nav_timer  = 0.f;    ///< counts down; <= 0 → nav hidden
float  g_play_timer = 0.f;    ///< counts down to next auto-advance
Uint64 g_last_ns    = 0;

std::string g_base_path;

std::unique_ptr<CrystalAnimState> g_crystal_state;
std::unique_ptr<AnimalAnimState>  g_animal_state;

// Page-transition fade state machine
// FadingOut: old page disappears behind a black overlay (k_fade_out_dur s).
//            When complete, loadPage() swaps the content and FadingIn starts.
// FadingIn:  black overlay dissolves to reveal the new page (k_fade_in_dur s).
// Idle:      no transition running; overlay is fully transparent.
enum class FadeState { Idle, FadingOut, FadingIn };

FadeState g_fade_state       = FadeState::Idle;
float     g_fade_t           = 0.f;    ///< elapsed time inside current fade phase
int       g_fade_target_page = -1;     ///< page queued during FadingOut

// Orbit camera state (used on the scene3d page)
float  g_yaw_deg   =  20.f;
float  g_pitch_deg =  18.f;
float  g_dist      =   5.f;
bool   g_dragging  = false;
float  g_drag_mx   =  0.f;
float  g_drag_my   =  0.f;

pce::sdlos::css::StyleSheet g_scene_css;

/// updateCrystalRotation  —  apply current animated rotation values to the crystal node
///
// Reads g_crystal_state->rotation_z and rotation_y, writes them to the node
// style attributes where the draw callback can read them.
static void updateCrystalRotation()
{
    using namespace pce::sdlos;
    if (!g_tree || !g_crystal_h.valid() || !g_crystal_state) return;

    RenderNode* n = g_tree->node(g_crystal_h);
    if (!n) return;

    // Get current animated rotation values.
    const float rot_z = g_crystal_state->rotation_z.current();
    const float rot_y = g_crystal_state->rotation_y.current();

    // Encode rotations as style attributes (comma-separated or separate).
    // The draw callback can read these with n->style("rotate-z") etc.
    char buf_z[32];
    char buf_y[32];
    std::snprintf(buf_z, sizeof(buf_z), "%.1f", rot_z);
    std::snprintf(buf_y, sizeof(buf_y), "%.1f", rot_y);

    n->setStyle("rotate-z", buf_z);
    n->setStyle("rotate-y", buf_y);
    n->dirty_render = true;
}

static bool loadSceneCss(const std::filesystem::path& path,
                         pce::sdlos::RenderTree&      tree,
                         pce::sdlos::NodeHandle       root,
                         pce::sdlos::SDLRenderer&     renderer)
{
    if (std::filesystem::exists(path)) {
        g_scene_css = pce::sdlos::css::load(path.string());
        if (!g_scene_css.empty()) {
            g_scene_css.applyTo(tree, root, renderer.pixelScaleX());
           g_scene_css.buildHover(tree, root, renderer.pixelScaleX());
            sdlos_log("[styleguide] scene.css: "
                                  + std::to_string(g_scene_css.size()) + " rules");
            return true;
        }
    }

    return false;
}


/// setOverlayAlpha  —  drive the full-screen fade overlay
///
// `a` is [0, 1].  0 = fully transparent (normal view).  1 = fully opaque black.
//
// The overlay is a LayoutKind::None div that was injected into the root by
// jade_app_init.  It sits on top of all page content and behind nothing.
// Its x/y/w/h are refreshed here every call so window resizes are handled
// automatically.
//
// Opacity is encoded as the alpha byte of a "#000000XX" hex colour so the
// existing draw callback (which re-reads backgroundColor each frame) picks
// it up with no engine changes.
static void setOverlayAlpha(float a)
{
    using namespace pce::sdlos;
    if (!g_tree || !g_fade_overlay_h.valid()) return;

    RenderNode* n = g_tree->node(g_fade_overlay_h);
    if (!n) return;

    // Keep the overlay sized to the current logical viewport.
    if (const RenderNode* root = g_tree->node(g_root)) {
        n->x = 0.f;  n->y = 0.f;
        n->w = root->w;
        n->h = root->h;
    }

    const float clamped = a < 0.f ? 0.f : (a > 1.f ? 1.f : a);
    const unsigned ab   = static_cast<unsigned>(clamped * 255.f + 0.5f);
    char buf[12];
    std::snprintf(buf, sizeof(buf), "#000000%02x", ab);
    n->setStyle("backgroundColor", buf);
    n->dirty_render = true;
}


/// navigatePage  —  animated page change
//  Drop-in replacement for bare loadPage() calls.  When not already in a
//  transition it kicks off a FadingOut phase; when a transition is in flight
//  the target page is updated so the new destination takes effect as soon as
//  the current fade-out completes.
static void navigatePage(int target)
{
    target = (target % k_page_count + k_page_count) % k_page_count;

    if (g_fade_state == FadeState::FadingOut) {
        // Already fading out — just redirect to the new target.
        g_fade_target_page = target;
        return;
    }

    // If a fade-in is running, skip to its end first (instant cut is better
    // than a half-revealed page vanishing into another fade-out).
    if (g_fade_state == FadeState::FadingIn) {
        setOverlayAlpha(0.f);
        g_fade_state = FadeState::Idle;
    }

    g_fade_target_page = target;
    g_fade_state       = FadeState::FadingOut;
    g_fade_t           = 0.f;
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
            const char* width_val = active ? "24" : "8";
            n->setStyle("width",           width_val);
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

static void update3DVisibility()
{
    using namespace pce::sdlos;
    if (!g_tree) return;

    const NodeHandle sg3d = g_tree->findById(g_root, "sg-3d");
    if (sg3d.valid()) {
        if (RenderNode* n = g_tree->node(sg3d)) {
            const bool show = (g_page == k_3d_page_idx);
            n->setStyle("display", show ? "block" : "none");
            g_tree->markDirty(sg3d);
        }
    }

    const NodeHandle animal_scene = g_tree->findById(g_root, "animal-scene");
    if (animal_scene.valid()) {
        if (RenderNode* n = g_tree->node(animal_scene)) {
            const bool show = (g_page == k_animal_page_idx);
            n->setStyle("display", show ? "block" : "none");
            g_tree->markDirty(animal_scene);
        }
    }
}

static void updateAnimalScene()
{
    using namespace pce::sdlos;
    if (!g_tree || !g_animal_state) return;

    const NodeHandle scene_h = g_tree->findById(g_root, "animal-scene");
    if (!scene_h.valid()) return;

    RenderNode* n = g_tree->node(scene_h);
    if (!n) return;

    const int idx = g_animal_state->current_animal_idx;
    n->setStyle("src", k_animal_models[idx]);
    g_tree->markDirty(scene_h);

    // Update UI text (hidden by default)
    const NodeHandle name_h = g_tree->findById(g_root, "animal-name-text");
    if (name_h.valid()) {
        if (RenderNode* nn = g_tree->node(name_h)) {
            nn->setStyle("text", k_animal_names[idx]);
            nn->dirty_render = true;
        }
    }

    // Reset reveal state
    g_animal_state->name_revealed = false;
    g_animal_state->reveal_y.set(0.f);

    sdlos_log("[styleguide] animal changed to: " + std::string(k_animal_names[idx]));
}

static void loadPage(int page)
{
    using namespace pce::sdlos;
    if (!g_tree || !g_page_container.valid() || !g_vfs) return;

    g_page = std::clamp(page, 0, k_page_count - 1);

    // 1. Remove previous page content.
    clearChildren(*g_tree, g_page_container);

    // 2. Load jade source from asset:// (binary data directory).
    const std::string path = std::string("asset://") + k_pages[g_page].file;
    const auto result = g_vfs->read_text(path);

    if (result) {
        const std::string& src = *result;
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
    g_tree->markLayoutDirty(g_page_container);
    g_tree->forceAllDirty(g_tree->root());


    // 4. Update chrome that reflects page state.
    update3DVisibility();
    updateDots();

    // Reset animal handle on page load
    if (g_animal_state) {
        // g_animal_state->parrot_h = k_null_handle; // Removed to avoid resetting on simple page loads
    }

    if (g_page == k_animal_page_idx) {
        updateAnimalScene();
    }

    sdlos_log("[styleguide] page " + std::to_string(g_page)
              + " → " + k_pages[g_page].title
              + "  (" + path + ")");
}

/// tickFade  —  called every frame from the root update callback
/// Drives g_fade_state forward, writing to the overlay each tick.
/// Returns true while a transition is active so the caller knows to keep
/// setting dirty_render.
static bool tickFade(float dt)
{
    switch (g_fade_state) {

    case FadeState::FadingOut: {
        g_fade_t += dt;
        const float raw = g_fade_t / k_fade_out_dur;
        const float t   = raw < 1.f ? raw : 1.f;
        // ease-in quadratic: slow start → quick blackout
        setOverlayAlpha(t * t);

        if (g_fade_t >= k_fade_out_dur) {
            // Screen is now black — swap the page content invisibly.
            loadPage(g_fade_target_page);
            // Keep overlay fully opaque so the new page is hidden on first draw.
            setOverlayAlpha(1.f);
            g_fade_state = FadeState::FadingIn;
            g_fade_t     = 0.f;
        }
        return true;
    }

    case FadeState::FadingIn: {
        g_fade_t += dt;
        const float raw = g_fade_t / k_fade_in_dur;
        const float t   = raw < 1.f ? raw : 1.f;
        // ease-out quadratic: quick reveal → slow settle
        const float inv = 1.f - t;
        setOverlayAlpha(inv * inv);

        if (g_fade_t >= k_fade_in_dur) {
            setOverlayAlpha(0.f);
            g_fade_state = FadeState::Idle;
        }
        return true;
    }

    default:
        return false;
    }
}

} // anonymous namespace


/// jade_app_vfs_init
#define SDLOS_BEHAVIOR_VFS_INIT
void jade_app_vfs_init(pce::vfs::Vfs& vfs)
{
    g_vfs = &vfs;
}

/// jade_app_init                        \               /
void jade_app_init(pce::sdlos::RenderTree&               tree,
                   pce::sdlos::NodeHandle     /*    */   root,
                   pce::sdlos::IEventBus&                 bus,
                   pce::sdlos::SDLRenderer&               renderer,
                   std::function<bool(const SDL_Event&)>& out_handler)
{
    using namespace pce::sdlos;
    namespace fs = std::filesystem;

    //  0. Reset module state
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

    // Fade state — always start clean so a hot-reload or sdlos:navigate
    // never leaves the overlay stuck in a mid-transition state.
    g_fade_state       = FadeState::Idle;
    g_fade_t           = 0.f;
    g_fade_target_page = -1;
    g_fade_overlay_h   = pce::sdlos::k_null_handle;

    const char* bp = SDL_GetBasePath();
    g_base_path    = bp ? bp : "";

    // 1. Cache frequently-used node handles
    g_page_container = tree.findById(root, "page-container");
    g_nav_bar        = tree.findById(root, "nav-bar");

    if (!g_page_container.valid())
        sdlos_log("[styleguide] WARNING: #page-container not found — check jade");
    if (!g_nav_bar.valid())
        sdlos_log("[styleguide] WARNING: #nav-bar not found — check jade");

    // 2. GltfScene init
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

    // Allocate animal state early
    g_animal_state = std::make_unique<AnimalAnimState>();

    // 3. Override scene3d src before attach()
    //
    // jade_host's resolveAssetPaths() already ran and resolved the jade's
    // src="data/models/Crystal_Small_03.glb" relative to the JADE SOURCE
    // directory.  Override it here to point at the binary-dir copy that
    // CMake placed next to the executable.


    // 5. Load scene.css — 3-D material overrides
    if (init_ok) {
        const fs::path scene_css_path = fs::path(g_base_path) / "scene.css";

        // Try filesystem.
        if (fs::exists(scene_css_path)) {
            g_scene_css = pce::sdlos::css::load(scene_css_path.string());
            if (!g_scene_css.empty()) {
                g_scene_css.applyTo(tree, root, renderer.pixelScaleX());
                g_scene_css.buildHover(tree, root, renderer.pixelScaleX());
                sdlos_log("[styleguide] scene.css: "
                          + std::to_string(g_scene_css.size()) + " rules");
            }
        } else {
            sdlos_log("[styleguide] WARNING: scene.css not found at "
                      + scene_css_path.string());
        }

        // Seed GPU material uniforms from freshly-applied CSS.
        g_scene.applyCSS(tree);

        // Wire sg-crystal as a clickable hotspot.
        // After attach() the proxy has id="sg-crystal" (from mesh-id attr).
        {
            const pce::sdlos::NodeHandle crystal_h = tree.findById(root, "sg-crystal");
            if (crystal_h.valid()) {
                // Cache the crystal handle globally for animation updates
                g_crystal_h = crystal_h;

                // Initialize animation state
                g_crystal_state = std::make_unique<CrystalAnimState>();

                // Start continuous rotation: one full 360° spin every 4 seconds
                // Using easing::linear for smooth constant-speed rotation
                g_crystal_state->rotation_z.transition(
                    360.f,                           // target: one full revolution
                    4000.f,                          // 4000 ms = 4 seconds
                    pce::sdlos::easing::linear       // constant speed (no ease)
                );

                if (pce::sdlos::RenderNode* cn = tree.node(crystal_h)) {
                    cn->setStyle("onclick",    "hotspot:select");
                    cn->setStyle("data-value", "sg-crystal");
                    cn->setStyle("data-info",
                        "Crystal_Small_03 — amethyst shard formed in a floating\n"
                        "crystal cave. Low roughness, soft violet emissive glow.\n"
                        "Rendered via PBR metallic-roughness on SDL3 GPU.");
                    const auto cls = cn->style("class");
                    cn->setStyle("class",
                        cls.empty() ? "hotspot" : std::string(cls) + " hotspot");
                }
                sdlos_log("[styleguide] sg-crystal hotspot wired + animation initialized");
            } else {
                sdlos_log("[styleguide] WARNING: sg-crystal proxy not found — "
                          "check attach() and mesh-id attr");
            }
        }

        // Initialize Animal handle if found
        const NodeHandle parrot_h = tree.findById(root, "parrot-mesh");
        if (parrot_h.valid()) {
            g_animal_state->parrot_h = parrot_h;
            if (RenderNode* n = tree.node(parrot_h)) {
                n->setStyle("--scale", "3.0");
                n->setStyle("--translate-y", "0");
                n->setStyle("--rotation-x", "0");
            }
        }

        // Perspective camera — initial orbital position.
        g_scene.camera().perspective(
            45.f,
            static_cast<float>(SDLOS_WIN_W) / static_cast<float>(SDLOS_WIN_H));
        updateOrbitCamera();
    }

    // 6. Load first page into #page-container
    loadPage(0);

    //7. Show nav bar on startup (auto-hides after k_nav_hide_time)
    showNav();
    updatePlayBtn();

     // 8. Per-frame update: timers + 3D tick + fade
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

             // Crystal animation tick — update rotation and keep node dirty while animating
             if (g_crystal_state && g_crystal_state->auto_spin && g_crystal_h.valid()) {
                 // Check if rotation animation has completed
                 if (g_crystal_state->rotation_z.finished()) {
                     // Loop: start a new 360° rotation
                     g_crystal_state->rotation_z.transition(
                         360.f,
                         4000.f,
                         pce::sdlos::easing::linear
                     );
                 }

                 // Apply current rotation values to the crystal node
                 updateCrystalRotation();

                 // Keep the crystal node dirty while animating so it re-renders
                 g_tree->markDirty(g_crystal_h);
             }

             // Nav bar auto-hide timer.
             if (g_nav_timer > 0.f) {
                 g_nav_timer -= dt;
                 if (g_nav_timer <= 0.f)
                     hideNav();
             }

             // Page-transition fade (fade-to-black between pages).
             tickFade(dt);

             // Animal animation update
             if (g_animal_state && g_animal_state->parrot_h.valid() && g_page == k_animal_page_idx) {
                 if (RenderNode* n = g_tree->node(g_animal_state->parrot_h)) {
                     char buf_y[32], buf_r[32];
                     std::snprintf(buf_y, sizeof(buf_y), "%.3f", g_animal_state->hop_y.current());
                     std::snprintf(buf_r, sizeof(buf_r), "%.1f", g_animal_state->roll_x.current());
                     n->setStyle("--translate-y", buf_y);
                     n->setStyle("--rotation-x", buf_r);
                     n->dirty_render = true;
                     g_tree->markDirty(g_animal_state->parrot_h);
                 }

                 // Update name reveal UI animation
                 const NodeHandle reveal_h = g_tree->findById(g_root, "animal-name-container");
                 if (reveal_h.valid()) {
                     if (RenderNode* rn = g_tree->node(reveal_h)) {
                         const float t = g_animal_state->reveal_y.current();
                         // Animate background alpha: hidden=fully transparent, revealed=semi-opaque
                         const unsigned ab = static_cast<unsigned>(t * 153.f + 0.5f); // 0→0, 1→99 (60%)
                         char buf[12];
                         std::snprintf(buf, sizeof(buf), "#000000%02x", ab);
                         rn->setStyle("backgroundColor", buf);
                         rn->dirty_render = true;
                     }
                 }
                 // Update name text visibility
                 const NodeHandle name_h = g_tree->findById(g_root, "animal-name-text");
                 if (name_h.valid()) {
                     if (RenderNode* rn = g_tree->node(name_h)) {
                         const float t = g_animal_state->reveal_y.current();
                         // Color alpha: fully transparent when hidden, white when revealed
                         const unsigned ab = static_cast<unsigned>(t * 255.f + 0.5f);
                         char buf[12];
                         std::snprintf(buf, sizeof(buf), "#ffffff%02x", ab);
                         rn->setStyle("color", buf);
                         rn->dirty_render = true;
                     }
                 }
             }

             // Auto-advance slideshow.
             if (g_playing) {
                 g_play_timer -= dt;
                 if (g_play_timer <= 0.f) {
                     g_play_timer = k_auto_advance;
                     navigatePage((g_page + 1) % k_page_count);
                     showNav();
                 }
             }
         };
     }

    // 9. Event bus subscriptions

    bus.subscribe("sg:prev", [](const std::string&) {
        navigatePage(g_page - 1);
        showNav();
    });

    bus.subscribe("sg:next", [](const std::string&) {
        navigatePage(g_page + 1);
        showNav();
    });

    bus.subscribe("sg:playpause", [](const std::string&) {
        g_playing = !g_playing;
        if (g_playing) g_play_timer = k_auto_advance;
        updatePlayBtn();
        showNav();
    });

    bus.subscribe("animal:hop", [](const std::string&) {
        if (!g_animal_state) return;
        g_animal_state->hop_y.transition(0.4f, 250.f, pce::sdlos::easing::easeOutSpringBouncy);
    });

    bus.subscribe("animal:roll", [](const std::string&) {
        if (!g_animal_state) return;
        g_animal_state->roll_x.transition(25.f, 300.f, pce::sdlos::easing::easeOutSpringSnappy);
    });

    bus.subscribe("animal:next", [](const std::string&) {
        if (!g_animal_state) return;
        g_animal_state->current_animal_idx = (g_animal_state->current_animal_idx + 1) % k_animal_count;
        updateAnimalScene();
    });

    bus.subscribe("animal:prev", [](const std::string&) {
        if (!g_animal_state) return;
        g_animal_state->current_animal_idx = (g_animal_state->current_animal_idx + k_animal_count - 1) % k_animal_count;
        updateAnimalScene();
    });

    bus.subscribe("animal:reveal", [](const std::string&) {
        if (!g_animal_state) return;
        g_animal_state->name_revealed = !g_animal_state->name_revealed;
        // Wrap easeOutBack to match EasingFn signature (float->float)
        auto ease_fn = [](float t) -> float { return pce::sdlos::easing::easeOutBack(t); };
        g_animal_state->reveal_y.transition(g_animal_state->name_revealed ? 1.0f : 0.0f, 400.f, ease_fn);
    });

    bus.subscribe("animal:sound", [](const std::string&) {
        sdlos_log("[styleguide] Mooo! Baaa! (Sound system placeholder triggered)");
        // Trigger a tiny hop to acknowledge
        if (g_animal_state)
            g_animal_state->hop_y.transition(0.2f, 150.f, pce::sdlos::easing::easeOutSpringBouncy);
    });

     // hotspot:select — update the info line on the scene3d page (if loaded)
     bus.subscribe("hotspot:select", [](const std::string& payload) {
         if (!g_tree) return;

         // Special handling for crystal: boost the spin speed on click
         if (payload == "sg-crystal" && g_crystal_state) {
             // Accelerate: complete one more full rotation at faster speed
             const float current_rot = g_crystal_state->rotation_z.current();
             g_crystal_state->rotation_z.transition(
                 current_rot + 360.f,         // one extra revolution from current position
                 2000.f,                      // faster: 2 seconds instead of 4
                 pce::sdlos::easing::easeOut  // smooth deceleration at the end
             );
             sdlos_log("[styleguide] crystal boost spin triggered");
         }

         // The info node lives in the dynamically-loaded scene3d.jade page.
         // It is only present in the tree when page 3 is active; the
         // findById will silently return invalid on other pages — no harm.
         const pce::sdlos::NodeHandle info_h =
             g_tree->findById(g_root, "sg-hotspot-info");
         if (!info_h.valid()) return;

         // Resolve the data-info string from the clicked proxy node.
         std::string info = "No info available.";
         const pce::sdlos::NodeHandle clicked =
             g_tree->findById(g_root, payload);
         if (clicked.valid()) {
             const auto sv =
                 g_tree->node(clicked)->style("data-info");
             if (!sv.empty()) info = std::string(sv);
         }

         if (pce::sdlos::RenderNode* n = g_tree->node(info_h)) {
             n->setStyle("text",  info);
             n->setStyle("color", "#c4b5fd");
             n->dirty_render = true;
         }
     });

    const std::filesystem::path scene_css_path = std::filesystem::path(g_base_path) / "scene.css";

    bus.subscribe("styleguide:rebuild", [&tree, root, &renderer, scene_css_path](const std::string&) {
        sdlos_log("[styleguide] rebuild style tree");
        if (!loadSceneCss(scene_css_path, tree, root, renderer)) {
             sdlos_log("[styleguide] failed to load " + scene_css_path.string());
        }
        tree.markLayoutDirty(root);
    });


    // Event Handlers
    out_handler = [&bus](const SDL_Event& ev) -> bool {

        switch (ev.type) {

        // Mouse motion — always show nav; on 3D page also orbit + hover tick.
        case SDL_EVENT_MOUSE_MOTION:
            showNav();
            // Hover ticks for 3D proxy CSS hover effects.
            if (g_tree && !g_scene_css.hover.empty())
                g_scene_css.tickHover(*g_tree, ev.motion.x, ev.motion.y);
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
                if (g_page == k_animal_page_idx) {
                    g_animal_state->current_animal_idx = (g_animal_state->current_animal_idx + k_animal_count - 1) % k_animal_count;
                    updateAnimalScene();
                    return true;
                }
                case SDL_SCANCODE_UP:
                    if (g_page == k_animal_page_idx) {
                        bus.publish("animal:reveal", "");
                        return true;
                    }
                    navigatePage(g_page - 1);
                    return true;

                case SDL_SCANCODE_RIGHT:
                    if (g_page == k_animal_page_idx) {
                        g_animal_state->current_animal_idx = (g_animal_state->current_animal_idx + 1) % k_animal_count;
                        updateAnimalScene();
                        return true;
                    }
                case SDL_SCANCODE_DOWN:
                    navigatePage(g_page + 1);
                    return true;

                case SDL_SCANCODE_SPACE:
                    g_playing = !g_playing;
                    if (g_playing) g_play_timer = k_auto_advance;
                    updatePlayBtn();
                    return true;

                case SDL_SCANCODE_HOME:
                    navigatePage(0);
                    return true;

                case SDL_SCANCODE_END:
                    navigatePage(k_page_count - 1);
                    return true;

            default:
                return false;
            }

        default:
            return false;
        }
    };

    // 11. Fade overlay — full-screen LayoutKind::None black div
    //
    // Created AFTER jade_app_init wires everything up so it sits on top of
    // all page content in the draw order.  Initial alpha = 0 (invisible).
    // tickFade() animates backgroundColor alpha on every page transition.
    //
    // Render order recap (why this works as a HUD-safe cross-fade):
    //   Pass 2:  entire UI tree → ui_texture_    (independent offscreen)
    //   Pass 3a: FrameGraph / FBM → swapchain    (post-processing here)
    //   Pass 3b: GltfScene  → swapchain          (3-D pre-pass, LOADOP_LOAD)
    //   Pass 3c: ui_texture_ → swapchain         (alpha-composite, LAST)
    //
    // The FrameGraph / post-processing in Pass 3a runs on the swapchain and
    // NEVER touches ui_texture_, so the UI (including this overlay) is always
    // crisp, never blurred or colour-graded by any shader effect.
    {
        const NodeHandle overlay = tree.alloc();
        g_fade_overlay_h = overlay;
        if (RenderNode* n = tree.node(overlay)) {
            n->layout_kind = LayoutKind::None;
            n->x = 0.f;
            n->y = 0.f;
            // Size overlay to root node dimensions (which are now physical pixels)
            if (const RenderNode* root_node = tree.node(root)) {
                n->w = root_node->w;
                n->h = root_node->h;
            } else {
                n->w = static_cast<float>(SDLOS_WIN_W) * renderer.pixelScaleX();
                n->h = static_cast<float>(SDLOS_WIN_H) * renderer.pixelScaleY();
            }
            // Fully transparent to start; tickFade() writes the alpha byte.
            n->setStyle("backgroundColor", "#00000000");
            //n->setStyle("backgroundColor", "#000000FF");
            n->dirty_render = false;
        }
        bindDrawCallbacks(tree, overlay);
        tree.appendChild(root, overlay);
    }

    sdlos_log("[styleguide] init complete — "
              + std::to_string(tree.nodeCount()) + " nodes, "
              + std::to_string(k_page_count)     + " pages"
              + "  [fade overlay active]");
}


