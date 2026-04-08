// Altar sequential composition demo
//
// Included into the host via:
//   -DSDLOS_APP_BEHAVIOR="<abs-path>/automat.cc"
//
// Architecture
//   1. GltfScene::attach() scans the RenderTree for all tag="scene3d" nodes
//      and loads their GLB files into GPU memory at startup.  All eight
//      scene3d nodes start with display="none" so nothing is rendered yet.
//
//   2. scene.css is loaded explicitly after attach() and sets world-space
//      transform properties (--scale, --translate-x/y/z, --rotation-x/y/z)
//      and PBR material tints on the proxy RenderNodes.  GltfScene reads
//      these style props in buildModelMatrix() and drawEntry() every frame.
//
//   3. automat.css is loaded for the 2D overlay (HUD, modal, controls).
//
//   4. A per-frame timer in the root update() callback reveals models one
//      by one (every k_reveal_delay seconds) by setting display="block" on
//      the scene3d RenderNode.  GltfScene::tick() and drawEntry() both check
//      the scene3d parent's display style before processing an entry.
//
//   5. Right-mouse drag orbits the camera; scroll wheel zooms.
//
//   6. Clicking a visible mesh proxy publishes "hotspot:select" which
//      populates and reveals the info modal.  × dismisses it.
//
// CSS split
// ─────────
//   scene.css   — 3D layout (transform + material properties per mesh id)
//   automat.css — 2D UI  (HUD, modal, button styles)
//
// Both are loaded manually after attach() so the injected proxy nodes
// are already in the RenderTree when applyTo() runs.

#include "gltf/gltf_scene.h"
#include "css_loader.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>

namespace {

// Sequential reveal descriptor
// Each stage reveals one model.  scene3d_id is the RenderNode id set on
// the scene3d container (e.g. "scene-floor").  label is shown in the HUD.
struct StageDesc {
    const char* scene3d_id;   ///< id on the scene3d RenderNode  (display toggle)
    const char* label;        ///< human-readable name shown in #stage-label
    const char* info;         ///< text shown when the mesh is clicked
};

constexpr StageDesc k_stages[] = {
    { "scene-floor",
      "Floor Tiles  (1/8)",
      "Ancient floor tiles, worn smooth by centuries of foot traffic." },

    { "scene-dune",
      "Desert Sand  (2/8)",
      "Dunes of fine golden sand that drift silently around the altar." },

    { "scene-altar",
      "Flower Altar  (3/8)",
      "The centrepiece: a flowering altar that pulses with warm light." },

    { "scene-base",
      "Glowing Base (4/8)",
      "A luminous pedestal, source of the altar's azure glow." },

    { "scene-crystal-cluster",
      "Cluster  (5/8)",
      "Amethyst cluster grown over aeons, crackling with stored energy." },

    { "scene-crystal-small",
      "Fragment  (6/8)",
      "A small shard that fell from the main cluster; still warm to touch." },

    { "scene-arc",
      "Ancient Arc  (7/8)",
      "A stone archway framing the altar, inscribed with lost runes." },

    { "scene-screen",
      "Sci-Fi Screen  (8/8)",
      "An anachronistic display panel — someone left it here long ago." },
};

constexpr int   k_stage_count  = static_cast<int>(
    sizeof(k_stages) / sizeof(k_stages[0]));
constexpr float k_reveal_delay = 2.5f;   // seconds between each reveal

} // namespace

// Module-level state
//
// Held at namespace scope so lambdas stored in update() / out_handler never
// capture dangling stack references after jade_app_init returns.

namespace {

pce::sdlos::gltf::GltfScene  g_scene;
pce::sdlos::RenderTree*      g_tree    = nullptr;
pce::sdlos::NodeHandle       g_root;

// CSS kept at module scope so buildHover entries survive and tickHover()
// can be called from the mouse-motion handler each frame.
pce::sdlos::css::StyleSheet  g_scene_css;   // scene.css  (3D layout)
pce::sdlos::css::StyleSheet  g_ui_css;      // automat.css (2D overlay)

// Cached handles — looked up once at init.
pce::sdlos::NodeHandle  g_h_stage_label;   // #stage-label HUD text
pce::sdlos::NodeHandle  g_h_modal;         // #modal container
pce::sdlos::NodeHandle  g_h_modal_body;    // #modal-body text node

// Orbit camera
float  g_yaw_deg   =   0.f;
float  g_pitch_deg =  15.f;
float  g_dist      =   6.f;
bool   g_dragging  = false;
float  g_last_mx   =   0.f;
float  g_last_my   =   0.f;

// Sequential reveal
int    g_reveal_stage = 0;
float  g_reveal_timer = 0.f;   // fires immediately on first frame
Uint64 g_last_tick_ns = 0;

} // namespace


namespace {

static void updateOrbitCamera() noexcept
{
    constexpr float kD2R = 3.14159265f / 180.f;
    const float pitch_r  = g_pitch_deg * kD2R;
    const float yaw_r    = g_yaw_deg   * kD2R;

    const float ex = g_dist * std::cos(pitch_r) * std::sin(yaw_r);
    const float ey = g_dist * std::sin(pitch_r);
    const float ez = g_dist * std::cos(pitch_r) * std::cos(yaw_r);

    // Pivot ~1.5 m above the floor — the altar's visual centre of mass.
    g_scene.camera().lookAt(ex,   ey + 1.5f, ez,
                            0.f,  1.5f,      0.f);
}

static void showModal(pce::sdlos::RenderTree& tree,
                      pce::sdlos::NodeHandle  modal) noexcept
{
    if (!modal.valid()) return;
    pce::sdlos::RenderNode* m = tree.node(modal);
    if (!m) return;
    m->setStyle("height",   "240");
    m->setStyle("overflow", "visible");
    tree.markLayoutDirty(modal);
    tree.markDirty(modal);
}

static void hideModal(pce::sdlos::RenderTree& tree,
                      pce::sdlos::NodeHandle  modal) noexcept
{
    if (!modal.valid()) return;
    pce::sdlos::RenderNode* m = tree.node(modal);
    if (!m) return;
    m->setStyle("height",   "0");
    m->setStyle("overflow", "hidden");
    tree.markLayoutDirty(modal);
    tree.markDirty(modal);
}

// Wire a proxy node as a clickable hotspot.  The data-info text is shown in
// the modal when the user clicks it; onclick routes through the EventBus.
static void wireHotspot(pce::sdlos::RenderTree& tree,
                         pce::sdlos::NodeHandle  root,
                         const char*             mesh_id,
                         const char*             info) noexcept
{
    using namespace pce::sdlos;
    const NodeHandle h = tree.findById(root, mesh_id);
    if (!h.valid()) return;
    RenderNode* n = tree.node(h);
    if (!n) return;

    n->setStyle("onclick",    "hotspot:select");
    n->setStyle("data-value", mesh_id);
    n->setStyle("data-info",  info);

    // Append "hotspot" to the existing class list.
    const std::string cls = std::string(n->style("class"));
    n->setStyle("class", cls.empty() ? "hotspot" : cls + " hotspot");
}

} // namespace                              ><


void jade_app_init(pce::sdlos::RenderTree&               tree,
                   pce::sdlos::NodeHandle                 root,
                   pce::sdlos::IEventBus&                  bus,
                   pce::sdlos::SDLRenderer&               renderer,
                   std::function<bool(const SDL_Event&)>& out_handler)
{
    using namespace pce::sdlos;
    namespace fs = std::filesystem;

    //  0. Reset module state
    g_tree        = &tree;
    g_root        = root;
    g_dragging    = false;
    g_yaw_deg     =   0.f;
    g_pitch_deg   =  15.f;
    g_dist        =   6.f;
    g_reveal_stage = 0;
    g_reveal_timer = 0.f;   // fires on the very first update tick
    g_last_tick_ns = SDL_GetTicksNS();

    //  1. GltfScene init
    const char* bp = SDL_GetBasePath();
    const std::string base_path = bp ? bp : "";

    const bool init_ok = g_scene.init(
        renderer.GetDevice(),
        renderer.GetShaderFormat(),
        base_path,
        SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM);

    if (!init_ok) {
        sdlos_log("[automat] GltfScene::init() failed — 3D rendering disabled");
    }

    // 1b. Wire 3D hooks
    if (init_ok) {
        renderer.setScene3DHook(
            [](SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* swap,
               float vw, float vh) noexcept {
                g_scene.render(cmd, swap, vw, vh);
            });
        renderer.setGpuPreShutdownHook([]() noexcept { g_scene.shutdown(); });
    }

    // 2. attach() — load all eight GLBs into GPU memory
    //
    // All scene3d nodes start with display="none" (set in the source).
    // attach() still loads them all; they just won't be drawn until
    // the reveal timer sets display="block" on their scene3d container.
    const int mesh_count = g_scene.attach(tree, root, base_path);
    if (mesh_count <= 0) {
        sdlos_log("[automat] attach(): no meshes loaded — check data/models/");
    } else {
        sdlos_log("[automat] attached " + std::to_string(mesh_count)
                  + " mesh primitive(s) across all scene3d nodes");
    }

    // 3. Load scene.css — 3D layout (transforms + PBR tints)
    //
    // scene.css targets proxy ids (#floor, #altar, #crystal-base …) and
    // sets --scale, --translate-x/y/z, --rotation-x/y/z so that
    // buildModelMatrix() positions each model correctly in world space.
    //
    // It is loaded after attach() so the proxy nodes are already in the
    // RenderTree when applyTo() runs.
    {
        const fs::path scene_css_path = fs::path(base_path) / "scene.css";
        if (fs::exists(scene_css_path)) {
            g_scene_css = pce::sdlos::css::load(scene_css_path.string());
            if (!g_scene_css.empty()) {
                g_scene_css.applyTo(tree, root);
                g_scene_css.buildHover(tree, root);
                sdlos_log("[automat] scene.css: "
                          + std::to_string(g_scene_css.size()) + " rules");
            }
        } else {
            sdlos_log("[automat] WARNING: scene.css not found at "
                      + scene_css_path.string());
        }
    }

    // 4. Load automat.css — 2D overlay styles
    {
        const fs::path ui_css_path = fs::path(base_path) / "automat.css";
        if (fs::exists(ui_css_path)) {
            g_ui_css = pce::sdlos::css::load(ui_css_path.string());
            if (!g_ui_css.empty()) {
                g_ui_css.applyTo(tree, root);
                g_ui_css.buildHover(tree, root);   // required for :hover rules on buttons
                sdlos_log("[automat] automat.css: "
                          + std::to_string(g_ui_css.size()) + " rules");
            }
        } else {
            sdlos_log("[automat] WARNING: automat.css not found at "
                      + ui_css_path.string());
        }
    }

    // Seed GPU material uniforms from freshly-applied styles.
    g_scene.applyCSS(tree);

    //  5. Wire hotspots for the interactive objects
    //
    // Each stage's scene3d_id is "scene-<mesh-id>" (e.g. "scene-altar").
    // Strip the "scene-" prefix (6 chars) to get the proxy mesh id.
    // The loop wires all 8 objects; no extra explicit calls needed.
    static constexpr std::string_view kPrefix = "scene-";
    for (const auto& s : k_stages) {
        const std::string_view sv(s.scene3d_id);
        if (sv.starts_with(kPrefix))
            wireHotspot(tree, root,
                        std::string(sv.substr(kPrefix.size())).c_str(),
                        s.info);
    }

    // 6. Perspective camera — initial orbital position
    g_scene.camera().perspective(
        45.f,
        static_cast<float>(SDLOS_WIN_W) / static_cast<float>(SDLOS_WIN_H));
    updateOrbitCamera();

    // Cache stable handles used by closures below.
    g_h_stage_label = tree.findById(root, "stage-label");
    g_h_modal       = tree.findById(root, "modal");
    g_h_modal_body  = tree.findById(root, "modal-body");

    // 7. Per-frame update: reveal timer + 3D tick
    if (RenderNode* rn = tree.node(root)) {
        rn->update = []() noexcept {
            if (!g_tree) return;

            // Delta time, capped at 50 ms.
            const Uint64 now_ns = SDL_GetTicksNS();
            const float  dt     = std::min(
                static_cast<float>(now_ns - g_last_tick_ns) * 1.0e-9f,
                0.050f);
            g_last_tick_ns = now_ns;

            const float vw = static_cast<float>(SDLOS_WIN_W);
            const float vh = static_cast<float>(SDLOS_WIN_H);

            // 3D AABB projection (used by tickHover / hitTest).
            g_scene.tick(*g_tree, vw, vh);

            // Debug: print bounds (per-frame projection check)
            if (g_reveal_stage > 0) {
                const StageDesc& stage = k_stages[g_reveal_stage - 1];

                // stage.scene3d_id is already const char*, e.g., "scene-altar"
                const std::string_view scene3d_view(stage.scene3d_id);

                // Strip the "scene-" prefix (6 characters) to get mesh id
                // "scene-altar" → "altar"
                constexpr std::string_view kPrefix = "scene-";
                if (scene3d_view.starts_with(kPrefix)) {
                    const std::string_view mesh_id_view =
                        scene3d_view.substr(kPrefix.size());

                    // Convert to C-string for findById()
                    const std::string mesh_id_str(mesh_id_view);
                    const NodeHandle scene_h = g_tree->findById(g_root,
                                                                mesh_id_str.c_str());

                    if (scene_h.valid()) {
                        if (RenderNode* sn = g_tree->node(scene_h)) {
                            // Ensure proxy is visible
                            sn->setStyle("display", "block");
                            g_tree->markDirty(scene_h);

                            // Log the projected bounds and size
                            sdlos_log("[BOUNDS] " + std::string(mesh_id_view) +
                                " size: " + std::to_string(mesh_id_view.size()) +
                                " → (" + std::to_string((int)sn->x) + ", " +
                                std::to_string((int)sn->y) + ", " +
                                std::to_string((int)sn->w) + " × " +
                                std::to_string((int)sn->h) + ")");
                        }
                    } else {
                        sdlos_log("[WARNING] Proxy node not found: " +
                                  std::string(mesh_id_view));
                    }
                }

            }

            // Sequential reveal
            if (g_reveal_stage >= k_stage_count) return;

            g_reveal_timer -= dt;
            if (g_reveal_timer > 0.f) return;
            g_reveal_timer = k_reveal_delay;

            const StageDesc& stage = k_stages[g_reveal_stage];

            // Show the scene3d container — GltfScene will start rendering
            // its proxy children from the next drawEntry() call.
            const NodeHandle scene_h = g_tree->findById(g_root,
                                                         stage.scene3d_id);
            if (scene_h.valid()) {
                if (RenderNode* sn = g_tree->node(scene_h)) {
                    sn->setStyle("display", "block");
                    g_tree->markDirty(scene_h);
                }
            }

           // Update the stage progress label — use cached handle.
            if (g_h_stage_label.valid()) {
                if (RenderNode* ln = g_tree->node(g_h_stage_label)) {
                    ln->setStyle("text", stage.label);
                    g_tree->markDirty(g_h_stage_label);
                }
            }

            ++g_reveal_stage;
        };
    }

    //  8. hotspot:select → populate and show the info modal
    bus.subscribe("hotspot:select", [](const std::string& payload) {
        if (!g_tree) return;

        std::string info = "Click an object to learn more.";

        const NodeHandle clicked = g_tree->findById(g_root, payload);
        if (clicked.valid()) {
            const auto sv = g_tree->node(clicked)->style("data-info");
            if (!sv.empty()) info = std::string(sv);
        }

        const NodeHandle body = g_tree->findById(g_root, "modal-body");
        if (body.valid()) {
            g_tree->node(body)->setStyle("text", info);
            g_tree->markDirty(body);
        }

        showModal(*g_tree, g_tree->findById(g_root, "modal"));
    });

    //  9. modal:close → hide the info modal
    bus.subscribe("modal:close", [](const std::string& /*unused*/) {
        if (!g_tree) return;
        hideModal(*g_tree, g_tree->findById(g_root, "modal"));
    });

    // 10. Raw event hook: orbit, zoom, hover
    out_handler = [](const SDL_Event& ev) -> bool {

        switch (ev.type) {

        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (ev.button.button == SDL_BUTTON_RIGHT) {
                g_dragging = true;
                g_last_mx  = ev.button.x;
                g_last_my  = ev.button.y;
            }
            return false;

        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (ev.button.button == SDL_BUTTON_RIGHT)
                g_dragging = false;
            return false;

        case SDL_EVENT_MOUSE_MOTION:
            // Hover ticks every motion event so :hover rules update in CSS.
            if (g_tree) {
                if (!g_scene_css.hover.empty())
                    g_scene_css.tickHover(*g_tree,
                                          ev.motion.x, ev.motion.y);
                if (!g_ui_css.hover.empty())
                    g_ui_css.tickHover(*g_tree,
                                       ev.motion.x, ev.motion.y);
            }
            if (g_dragging) {
                g_yaw_deg   += ev.motion.xrel * 0.45f;
                g_pitch_deg  = std::clamp(
                    g_pitch_deg - ev.motion.yrel * 0.45f, -60.f, 80.f);
                updateOrbitCamera();
            }
            return false;

        case SDL_EVENT_MOUSE_WHEEL:
            g_dist = std::clamp(g_dist - ev.wheel.y * 0.3f, 2.f, 20.f);
            updateOrbitCamera();
            return false;

        default:
            return false;
        }
    };

    sdlos_log("[automat] init complete — "
              + std::to_string(tree.nodeCount()) + " nodes, "
              + std::to_string(k_stage_count) + " reveal stages");
}
