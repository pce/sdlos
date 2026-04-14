// clima.cc — Clima weather dashboard behavior
//
// Included into the sdlos host via:
//   -DSDLOS_APP_BEHAVIOR="<abs-path>/clima.cc"
//
// Architecture
// ────────────
//   2-D weather UI with an optional 3-D city-mesh background.
//
//   The 3D layer is lazy: GltfScene is initialised at startup but no mesh
//   is loaded until the user selects a city that has an available mesh.
//   Switching cities calls clearMeshes() → updates src= on the scene3d
//   placeholder → calls attach() to upload the new mesh to the GPU.
//   Cities without a mesh simply hide the scene3d node and keep the solid
//   weather-colour background.
//
//   Weather state is entirely event-driven:
//     • clima:condition  payload="0"…"4" → switch weather type
//     • clima:city       payload="0"…"5" → switch city
//
//   The per-frame update() callback:
//     • refreshes the HH:MM clock every 30 s
//     • slowly rotates the city mesh yaw (0.2 deg/s)
//     • calls GltfScene::tick() to project 3D AABBs to screen
//
//   Mouse events:
//     • right-button drag → orbit camera
//     • scroll wheel      → zoom
//     • mouse motion      → CSS :hover engine

#include "css_loader.h"
#include "gltf/gltf_scene.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

namespace {

// ── Weather condition descriptors ─────────────────────────────────────────────

struct WeatherInfo {
    const char *name;
    const char *icon;
    const char *description;
    const char *card_bg;
    const char *root_bg;
    const char *text_color;
    int temp_delta;
    int feels_delta;
    int humidity;
};

constexpr WeatherInfo kWeather[] = {
    { "Sunny",  "☀️",  "Clear skies, bright sunshine",
      "#C2DEFF", "#87BDDE", "#1a3a5c",  0, -2, 55 },
    { "Rainy",  "🌧️", "Steady rain, carry an umbrella",
      "#4a6a82", "#2d4a5e", "#daeaf5", -6, -3, 88 },
    { "Snowy",  "❄️",  "Snow flurries, wrap up warm",
      "#D8E8F4", "#AABFD4", "#2b4a6e", -12, -4, 72 },
    { "Hail",   "🌨️", "Hailstorm — seek shelter indoors",
      "#4a4a5c", "#323240", "#c5cae9",  -8, -5, 80 },
    { "Cloudy", "⛅",  "Overcast skies, mild breeze",
      "#8C9EAD", "#6B7F8E", "#ecf0f1",  -3, -1, 65 },
};
constexpr int kWeatherCount = static_cast<int>(sizeof(kWeather) / sizeof(kWeather[0]));

//  City descriptors ──────────────────────────────────────────────────────────
//
// mesh_path — path relative to SDL_GetBasePath() for the GLTF manifest.
//             nullptr when no mesh is available for this city.

struct CityInfo {
    const char *name;
    const char *country_code;
    int base_temp_c;
    int base_humidity;
    const char *mesh_path;  ///< relative to base_path, or nullptr
};

constexpr CityInfo kCities[] = {
    { "Mainz",     "DE", 14, 70, nullptr },
    { "Hamburg",   "DE", 12, 75, nullptr },
    { "Amsterdam", "NL", 13, 78, "data/models/cities/amsterdam/amsterdam.gltf" },
    { "Kyoto",     "JP", 16, 68, "data/models/cities/kyoto/kyoto.gltf"        },
    { "Rome",      "IT", 19, 60, nullptr },
    { "Paris",     "FR", 15, 72, nullptr },
};
constexpr int kCityCount = static_cast<int>(sizeof(kCities) / sizeof(kCities[0]));

// ── Module-level state ────────────────────────────────────────────────────────

pce::sdlos::RenderTree             *g_tree          = nullptr;
pce::sdlos::NodeHandle              g_root;
pce::sdlos::css::StyleSheet         g_css;

int    g_weather_idx      = 0;
int    g_city_idx         = 0;
Uint64 g_last_clock_ns    = 0;

// Cached 2D node handles
pce::sdlos::NodeHandle g_h_root_node;
pce::sdlos::NodeHandle g_h_card;
pce::sdlos::NodeHandle g_h_icon;
pce::sdlos::NodeHandle g_h_temp;
pce::sdlos::NodeHandle g_h_condition;
pce::sdlos::NodeHandle g_h_city_name;
pce::sdlos::NodeHandle g_h_desc;
pce::sdlos::NodeHandle g_h_feels;
pce::sdlos::NodeHandle g_h_range;
pce::sdlos::NodeHandle g_h_humidity;
pce::sdlos::NodeHandle g_h_time;

// 3D city mesh
pce::sdlos::gltf::GltfScene g_scene;
bool                         g_scene_ready    = false;
std::string                  g_base_path;        ///< SDL_GetBasePath() result
int                          g_loaded_city_idx = -1; ///< which mesh is in GPU
pce::sdlos::NodeHandle       g_h_city_scene;     ///< scene3d#clima-city-scene

// Orbit camera state
float  g_city_yaw_deg   =  30.0f;
float  g_city_pitch_deg =  55.0f;
float  g_city_dist      = 2500.0f;
bool   g_dragging       = false;

}  // namespace

namespace {

// ── Helper: write text content into a node ────────────────────────────────────

static void setText(pce::sdlos::NodeHandle h, const std::string &text) noexcept {
    if (!h.valid() || !g_tree) return;
    if (pce::sdlos::RenderNode *n = g_tree->node(h)) {
        n->setStyle("text", text);
        g_tree->markDirty(h);
    }
}

// ── Helper: set a single CSS property on a node ───────────────────────────────

static void setStyleProp(pce::sdlos::NodeHandle h,
                         const char *key, const char *val) noexcept {
    if (!h.valid() || !g_tree) return;
    if (pce::sdlos::RenderNode *n = g_tree->node(h)) {
        n->setStyle(key, val);
        g_tree->markDirty(h);
    }
}

// ── Helper: replace the class attribute (active-state toggle) ─────────────────

static void setClass(pce::sdlos::NodeHandle h, const char *cls) noexcept {
    if (!h.valid() || !g_tree) return;
    if (pce::sdlos::RenderNode *n = g_tree->node(h)) {
        n->setStyle("class", cls);
        g_tree->markDirty(h);
        g_tree->markLayoutDirty(h);
    }
}

// ── Camera update ─────────────────────────────────────────────────────────────

static void updateCityCamera() noexcept {
    if (!g_scene_ready) return;
    constexpr float kD2R = 3.14159265f / 180.f;
    const float pitch_r  = g_city_pitch_deg * kD2R;
    const float yaw_r    = g_city_yaw_deg   * kD2R;
    const float ex = g_city_dist * std::cos(pitch_r) * std::sin(yaw_r);
    const float ey = g_city_dist * std::sin(pitch_r);
    const float ez = g_city_dist * std::cos(pitch_r) * std::cos(yaw_r);
    // Look from above at the city centre.  After the UTM→Y-up centering
    // in loadFile the mesh is centred at the world origin, so target (0,0,0).
    g_scene.camera().lookAt(ex, ey, ez, 0.f, 0.f, 0.f);
}

// ── Lazy city-mesh loader ─────────────────────────────────────────────────────
//
// Called from the clima:city subscriber.
// Releases the previously loaded mesh (if any), updates src= on the scene3d
// node, and calls GltfScene::attach() to upload the new mesh to the GPU.

static void loadCityMesh(int city_idx) noexcept {
    if (!g_tree || !g_scene_ready) return;
    if (city_idx == g_loaded_city_idx) return;  // nothing to do

    const bool has_new_mesh =
        city_idx >= 0 && city_idx < kCityCount &&
        kCities[city_idx].mesh_path != nullptr;

    // 1. Release existing mesh from GPU and hide the scene3d node.
    if (g_loaded_city_idx >= 0) {
        g_scene.clearMeshes(*g_tree);
    }
    setStyleProp(g_h_city_scene, "display", "none");
    g_loaded_city_idx = -1;

    if (!has_new_mesh) {
        sdlos_log("[clima] city " + std::string(kCities[city_idx].name)
                  + " — no mesh available");
        return;
    }

    // 2. Update src= on the scene3d node so attach() picks it up.
    if (pce::sdlos::RenderNode *sn = g_tree->node(g_h_city_scene)) {
        sn->setStyle("src", std::string(kCities[city_idx].mesh_path));
        g_tree->markDirty(g_h_city_scene);
    }

    // 3. Load mesh into GPU via attach().
    const int prims = g_scene.attach(*g_tree, g_root, g_base_path);
    if (prims > 0) {
        g_loaded_city_idx = city_idx;
        setStyleProp(g_h_city_scene, "display", "block");
        sdlos_log("[clima] loaded city mesh: "
                  + std::string(kCities[city_idx].name)
                  + " (" + std::to_string(prims) + " primitives)");
    } else {
        sdlos_log("[clima] city mesh not found or empty: "
                  + std::string(kCities[city_idx].mesh_path));
    }
}

// ── Weather display update ────────────────────────────────────────────────────
//
// When a city mesh is active the root and card backgrounds gain an alpha
// component so the 3D city is visible through the 2D UI layer.

static void updateDisplay() noexcept {
    if (!g_tree) return;

    const WeatherInfo &w = kWeather[g_weather_idx];
    const CityInfo    &c = kCities[g_city_idx];

    const int temp_c   = c.base_temp_c + w.temp_delta;
    const int feels_c  = temp_c + w.feels_delta;
    const int low_c    = temp_c - 4;
    const int high_c   = temp_c + 5;
    const int humidity = std::clamp(c.base_humidity + w.humidity - 70, 0, 99);

    // When a city mesh is showing, blend the backgrounds so the 3D city
    // is visible through the 2D UI layer (GPU pre-pass composite).
    // "90" ≈ 56 % opacity for the root; "d8" ≈ 85 % for the card.
    const bool mesh_active = (g_loaded_city_idx >= 0);
    const std::string root_bg = mesh_active
        ? std::string(w.root_bg) + "90"
        : std::string(w.root_bg);
    const std::string card_bg = mesh_active
        ? std::string(w.card_bg) + "d8"
        : std::string(w.card_bg);

    setStyleProp(g_h_root_node, "backgroundColor", root_bg.c_str());
    setStyleProp(g_h_card,      "backgroundColor", card_bg.c_str());

    setText(g_h_icon,      w.icon);
    setText(g_h_temp,      std::to_string(temp_c)   + "\xC2\xB0\x43");
    setText(g_h_condition, w.name);
    setText(g_h_city_name, std::string(c.name) + ", " + c.country_code);
    setText(g_h_desc,      w.description);
    setText(g_h_feels,     std::to_string(feels_c)  + "\xC2\xB0\x43");
    setText(g_h_range,
            std::to_string(low_c) + "\xC2\xB0 \xE2\x80\x94 "
            + std::to_string(high_c) + "\xC2\xB0");
    setText(g_h_humidity,  std::to_string(humidity) + "%");

    for (int i = 0; i < kWeatherCount; ++i) {
        const std::string id = "cond-" + std::to_string(i);
        const pce::sdlos::NodeHandle h = g_tree->findById(g_root, id.c_str());
        setClass(h, i == g_weather_idx ? "cond-btn cond-active" : "cond-btn");
    }
    for (int i = 0; i < kCityCount; ++i) {
        const std::string id = "city-" + std::to_string(i);
        const pce::sdlos::NodeHandle h = g_tree->findById(g_root, id.c_str());
        setClass(h, i == g_city_idx ? "city-btn city-active" : "city-btn");
    }
}

// ── Clock update ──────────────────────────────────────────────────────────────

static void updateClock() noexcept {
    if (!g_h_time.valid() || !g_tree) return;
    const std::time_t now = std::time(nullptr);
    const std::tm    *t   = std::localtime(&now);  // NOLINT(concurrency-mt-unsafe)
    if (!t) return;
    char buf[6];
    std::snprintf(buf, sizeof(buf), "%02d:%02d", t->tm_hour, t->tm_min);
    setText(g_h_time, buf);
}

}  // namespace

void jade_app_init(
    pce::sdlos::RenderTree &tree,
    pce::sdlos::NodeHandle root,
    pce::sdlos::IEventBus &bus,
    pce::sdlos::SDLRenderer &renderer,
    std::function<bool(const SDL_Event &)> &out_handler)
{
    using namespace pce::sdlos;
    namespace fs = std::filesystem;

    // 0. Reset module state
    g_tree          = &tree;
    g_root          = root;
    g_weather_idx   = 0;
    g_city_idx      = 0;
    g_last_clock_ns = SDL_GetTicksNS();

    // 1. Cache 2D node handles
    g_h_root_node = root;
    g_h_card      = tree.findById(root, "clima-card");
    g_h_icon      = tree.findById(root, "clima-icon");
    g_h_temp      = tree.findById(root, "clima-temp");
    g_h_condition = tree.findById(root, "clima-condition");
    g_h_city_name = tree.findById(root, "clima-city-name");
    g_h_desc      = tree.findById(root, "clima-desc");
    g_h_feels     = tree.findById(root, "clima-feels");
    g_h_range     = tree.findById(root, "clima-range");
    g_h_humidity  = tree.findById(root, "clima-humidity");
    g_h_time      = tree.findById(root, "clima-time");
    g_h_city_scene = tree.findById(root, "clima-city-scene");

    // 2. Load clima.css
    {
        const char      *bp       = SDL_GetBasePath();
        g_base_path               = bp ? bp : "";
        const fs::path css_path   = fs::path(g_base_path) / "clima.css";
        if (fs::exists(css_path)) {
            g_css = css::load(css_path.string());
            if (!g_css.empty()) {
                g_css.applyTo(tree, root);
                g_css.buildHover(tree, root);
                sdlos_log("[clima] css: " + std::to_string(g_css.size()) + " rules");
            }
        } else {
            sdlos_log("[clima] WARNING: clima.css not found at " + css_path.string());
        }
    }

    // 3. Initialise GltfScene (shaders compiled once; reused for all cities)
    g_scene_ready = g_scene.init(
        renderer.GetDevice(),
        renderer.GetShaderFormat(),
        g_base_path,
        SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM);

    if (g_scene_ready) {
        // Wire the 3D pre-pass hook — called every frame before jade 2D pass.
        renderer.setScene3DHook(
            [](SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *swap,
               float vw, float vh) noexcept {
                g_scene.render(cmd, swap, vw, vh);
            });
        // Release GPU resources when the window closes.
        renderer.setGpuPreShutdownHook([]() noexcept { g_scene.shutdown(); });

        // Perspective camera — bird's-eye view for city meshes.
        // WIN_W / WIN_H are compile-time constants from clima.cmake (900 × 620).
        // near=1 m avoids z-fighting; far=10 000 m handles city-scale scenes
        // (camera at ~2500 m distance, buildings spanning ±900 m in X/Z).
        g_scene.camera().perspective(
            45.f,
            static_cast<float>(SDLOS_WIN_W) / static_cast<float>(SDLOS_WIN_H),
            1.0f,
            10000.0f);
        updateCityCamera();

        sdlos_log("[clima] GltfScene ready — city meshes will load on demand");
    } else {
        sdlos_log("[clima] WARNING: GltfScene::init() failed — 3D disabled");
    }

    // 4. Prime 2D display
    updateDisplay();
    updateClock();

    // 5. EventBus — weather condition selector
    bus.subscribe("clima:condition", [](const std::string &payload) {
        if (!g_tree) return;
        try {
            const int idx = std::stoi(payload);
            if (idx >= 0 && idx < kWeatherCount) {
                g_weather_idx = idx;
                updateDisplay();
            }
        } catch (...) {
            sdlos_log("[clima] clima:condition — bad payload: " + payload);
        }
    });

    // 6. EventBus — city selector (triggers lazy mesh load)
    bus.subscribe("clima:city", [](const std::string &payload) {
        if (!g_tree) return;
        try {
            const int idx = std::stoi(payload);
            if (idx >= 0 && idx < kCityCount) {
                g_city_idx = idx;
                loadCityMesh(idx);   // lazy: clears old, loads new if available
                updateDisplay();     // re-apply alpha-adjusted backgrounds
            }
        } catch (...) {
            sdlos_log("[clima] clima:city — bad payload: " + payload);
        }
    });

    // 7. Per-frame update: clock, auto-rotate city mesh, 3D tick
    if (RenderNode *rn = tree.node(root)) {
        rn->update = []() noexcept {
            constexpr Uint64 kClockIntervalNs = 30'000'000'000ULL;  // 30 s
            constexpr float  kAutoRotateDps   = 0.2f;               // deg/s

            const Uint64 now_ns = SDL_GetTicksNS();

            // Clock refresh
            if (now_ns - g_last_clock_ns >= kClockIntervalNs) {
                g_last_clock_ns = now_ns;
                updateClock();
            }

            // Gentle auto-rotation + 3D AABB projection
            if (g_scene_ready && g_loaded_city_idx >= 0) {
                // Delta time, capped at 50 ms to avoid large jumps.
                static Uint64 s_last_ns = 0;
                if (s_last_ns == 0) s_last_ns = now_ns;
                const float dt = std::min(
                    static_cast<float>(now_ns - s_last_ns) * 1.0e-9f, 0.050f);
                s_last_ns = now_ns;

                if (!g_dragging) {
                    g_city_yaw_deg += kAutoRotateDps * dt;
                    updateCityCamera();
                }

                const float vw = static_cast<float>(SDLOS_WIN_W);
                const float vh = static_cast<float>(SDLOS_WIN_H);
                g_scene.tick(*g_tree, vw, vh);
            }
        };
    }

    // 8. Raw event hook: orbit, zoom, hover
    out_handler = [](const SDL_Event &ev) -> bool {
        switch (ev.type) {
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                if (ev.button.button == SDL_BUTTON_RIGHT)
                    g_dragging = true;
                break;
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (ev.button.button == SDL_BUTTON_RIGHT)
                    g_dragging = false;
                break;
            case SDL_EVENT_MOUSE_MOTION:
                if (g_dragging && g_scene_ready) {
                    g_city_yaw_deg   += ev.motion.xrel * 0.4f;
                    g_city_pitch_deg  = std::clamp(
                        g_city_pitch_deg - ev.motion.yrel * 0.4f, 5.f, 85.f);
                    updateCityCamera();
                }
                if (g_tree && !g_css.hover.empty())
                    g_css.tickHover(*g_tree, ev.motion.x, ev.motion.y);
                break;
            case SDL_EVENT_MOUSE_WHEEL:
                if (g_scene_ready) {
                    g_city_dist = std::clamp(
                        g_city_dist - ev.wheel.y * 50.f, 200.f, 8000.f);
                    updateCityCamera();
                }
                break;
            default:
                break;
        }
        return false;  // never consume — let the host handle SDL_Quit etc.
    };

    sdlos_log("[clima] init complete — " + std::to_string(tree.nodeCount())
              + " nodes");
}
