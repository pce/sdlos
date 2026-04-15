// clima.cc — Clima weather dashboard behavior
//
// Included into the sdlos host via:
//   -DSDLOS_APP_BEHAVIOR="<abs-path>/clima.cc"
//
// Architecture
//   2-D weather UI with an optional 3-D city-mesh background.
//
//   The 3D layer is lazy: GltfScene is initialised at startup but no mesh
//   is loaded until the user selects a city that has an available mesh.
//   Switching cities calls clearMeshes() → updates src= on the scene3d
//   placeholder → calls attach() to upload the new mesh to the GPU.
//   Cities without a mesh simply hide the scene3d node and keep the solid
//   weather-colour background.
//
//   Weather state is driven by two SelectBox widgets embedded in the header bar:
//     - Condition selector  (5 options)  → on_change updates g_weather_idx + updateDisplay()
//     - City selector       (25 options) → on_change updates g_city_idx, loadCityMesh(),
//                                          transitionCamera(), updateDisplay()
//   Both widgets are created in jade_app_init() and appended to #clima-cond-host /
//   #clima-city-host.  Dropdowns open downward into the open stage area.
//
//   The per-frame update() callback:
//     - refreshes the HH:MM clock every 30 s
//     - slowly rotates the city mesh yaw (0.2 deg/s)
//     - calls GltfScene::tick() to project 3D AABBs to screen
//
//   Mouse events:
//     - right-button drag → orbit camera
//     - scroll wheel      → zoom
//     - mouse motion      → CSS :hover engine

#include "core/animated.h"
#include "css_loader.h"
#include "gltf/gltf_scene.h"
#include "widgets/select_box.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <ctime>
#include <cstring>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace {

// Short alias used throughout this TU for the SelectBox widget API.
namespace widgets = pce::sdlos::widgets;

//  Weather condition descriptors

struct WeatherInfo {
    std::string_view name;
    std::string_view icon;
    std::string_view description;
    std::string_view card_bg;
    std::string_view root_bg;
    std::string_view text_color;
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

//  City descriptors
//
// mesh_path — path relative to SDL_GetBasePath() for the GLTF manifest.
//             Every city has a path; loadCityMesh() creates the directory at
//             first launch and skips loading gracefully when the file is absent.
//             Generate with: sdlos mesh generate --preset <key> --lod lowpoly
//                                                --face-count 80000 --dem --app clima

struct CityInfo {
    std::string_view name;
    std::string_view country_code;
    int base_temp_c;
    int base_humidity;
    std::optional<std::string_view> mesh_path;  ///< relative to base_path
    // Default orbital camera for this city (capture live with the C key).
    float default_yaw_deg   =  30.f;
    float default_pitch_deg =  45.f;
    float default_dist      = 2000.f;
};

constexpr CityInfo kCities[] = {
    // All entries have a 3-D mesh.  Camera defaults captured with the C key.
    // Tune remaining defaults live: orbit to taste, press C, paste here.
    { "Frankfurt",      "DE", 13, 72, "data/models/frankfurt/frankfurt.gltf",
      -113.900f,  8.200f, 1190.f },                                    //  0
    { "Amsterdam",      "NL", 13, 78, "data/models/amsterdam/amsterdam.gltf",
       -26.800f,  9.800f,  450.f },                                    //  1
    { "Kyoto",          "JP", 16, 68, "data/models/kyoto-hiei/kyoto-hiei.gltf" }, // 2 (DEM — Mt. Hiei)
    { "Paris",          "FR", 15, 72, "data/models/paris/paris.gltf",
       -20.f,    18.f,    480.f },                                      //  3
    { "Rome",           "IT", 19, 60, "data/models/rome/rome.gltf",
        10.f,    20.f,    450.f },                                      //  4
    { "Quito",          "EC", 13, 75, "data/models/quito/quito.gltf",
      -165.709f, 13.020f, 660.f },                                      //  5
    { "Barcelona",      "ES", 21, 62, "data/models/barcelona/barcelona.gltf",
        50.229f, 11.917f, 405.f },                                      //  6
    { "Madrid",         "ES", 18, 55, "data/models/madrid/madrid.gltf",
       -98.513451f, 7.382757f, 699.998352f },                           //  7
    { "London",         "GB", 12, 78, "data/models/london/london.gltf",
      -128.968f,  5.791f, 480.f },                                      //  8
    { "Chicago",        "US", 11, 65, "data/models/chicago/chicago.gltf",
       220.008f, 32.114f, 854.885f },                                   //  9
    { "Detroit",        "US", 10, 67, "data/models/detroit/detroit.gltf",
       143.836f, 26.484f, 624.302f },                                   // 10
    { "Sydney",         "AU", 18, 65, "data/models/sydney/sydney.gltf",
       -17.458f, 17.846f, 1044.616f },                                  // 11
    { "Tokyo",          "JP", 17, 70, "data/models/tokyo/tokyo.gltf",
       214.651f, 11.313f, 430.f },                                      // 12
    { "Toronto",        "CA", 10, 72, "data/models/toronto/toronto.gltf",
       -73.216f,  8.592f, 1240.f },                                     // 13
    { "Montreal",       "CA",  7, 67, "data/models/montreal/montreal.gltf",
       166.539f, 15.327f, 974.774f },                                   // 14  1976 Summer host
    { "Calgary",        "CA",  5, 57, "data/models/calgary/calgary.gltf",
      -180.864380f, 32.104721f, 1219.994385f },                         // 15  1988 Winter host
    { "Seoul",          "KR", 13, 63, "data/models/seoul/seoul.gltf",
      -167.490f, 15.000f, 870.f },                                      // 16  1988 Summer
    { "Beijing",        "CN", 13, 54, "data/models/beijing/beijing.gltf",
       -92.158f,  8.502f, 769.975f },                                   // 17  2008 / 2022
    { "Athens",         "GR", 20, 60, "data/models/athens/athens.gltf",
       109.878f, 22.141f, 330.f },                                      // 18  1896 / 2004
    { "Munich",         "DE", 10, 71, "data/models/munich/munich.gltf",
      -233.664398f, 11.420311f, 759.912903f },                          // 19  1972 Summer
    { "Mexico City",    "MX", 17, 58, "data/models/mexico-city/mexico-city.gltf",
       111.066f, 28.506f, 980.f },                                      // 20  1968 Summer
    { "Rio de Janeiro", "BR", 26, 79, "data/models/rio/rio.gltf",
      -281.580f,  5.000f, 1299.577f },                                  // 21  2016 Summer
    { "Oslo",           "NO",  7, 72, "data/models/oslo/oslo.gltf",
        99.909f, 11.158f, 1114.637f },                                  // 22  1952 Winter
    { "Austin",         "US", 22, 60, "data/models/austin/austin.gltf",
      -214.968f, 15.667f, 1029.998f },                                  // 23
    { "Nürnberg",       "DE",  9, 70, "data/models/nurnberg/nurnberg.gltf",
      -258.261f, 11.273f, 624.747f },                                   // 24
    { "Prague",         "CZ", 10, 72, "data/models/prague/prague.gltf",
      -127.279f,  6.250f, 645.f },                                      // 25
    { "Vienna",         "AT", 12, 68, "data/models/vienna/vienna.gltf",
      -360.211090f, 66.503029f, 790.000000f },                          // 26
    { "Casablanca",     "MA", 18, 72, "data/models/casablanca/casablanca.gltf",
      -129.115f, 11.966f, 544.701f },                                   // 27
    { "Marrakesh",      "MA", 19, 55, "data/models/marrakesh/marrakesh.gltf",
      -126.217f, 11.966f, 974.701f },                                   // 28
    { "Jakarta",        "ID", 28, 80, "data/models/jakarta/jakarta.gltf",
      -148.232f, 12.359f, 715.f },                                      // 29
    { "Bangkok",        "TH", 29, 75, "data/models/bangkok/bangkok.gltf",
      -133.862f, 25.961f, 1094.539f },                                  // 30
    { "Kathmandu",      "NP", 15, 65, "data/models/kathmandu/kathmandu.gltf",
      -134.058f, 16.784f, 200.f },                                      // 31
    { "Tirana",         "AL", 16, 65, "data/models/tirana/tirana.gltf",
      -148.232f, 12.359f, 715.f },                                      // 32
    { "Florence",       "IT", 16, 65, "data/models/florence/florence.gltf",
      -198.145f, 27.223f, 730.f },                                      // 33
    { "Dubrovnik",      "HR", 17, 62, "data/models/dubrovnik/dubrovnik.gltf",
      -195.510712f, 26.053471f, 375.126587f },                          // 34
    { "Kotor",          "ME", 17, 65, "data/models/kotor/kotor.gltf",
      -34.282185f, 9.118754f, 240.000000f                                    }, // 35
    { "Bruges",         "BE", 11, 78, "data/models/bruges/bruges.gltf"       },
    { "Tallinn",        "EE",  7, 75, "data/models/tallinn/tallinn.gltf"     },
    { "Kraków",         "PL", 10, 72, "data/models/krakow/krakow.gltf",
      143.286743f, 11.484391f, 559.701965f                                   }, // 38
    { "Salzburg",       "AT", 10, 70, "data/models/salzburg/salzburg.gltf",
      129.313095f, 13.435936f, 730.000000f                                   }, // 39
    { "Porto",          "PT", 15, 75, "data/models/porto/porto.gltf",
      126.002647f, 17.689060f, 760.000000f                                   },
    { "Cairo",          "EG", 22, 45, "data/models/cairo/cairo.gltf",
      -111.516f, 33.602f, 1150.f },
    { "Fez",            "MA", 17, 58, "data/models/fez/fez.gltf"                },
    { "Jerusalem",      "IL", 17, 58, "data/models/jerusalem/jerusalem.gltf"    },
    { "Hội An",         "VN", 25, 78, "data/models/hoi-an/hoi-an.gltf"          },
    { "Pingyao",        "CN", 11, 58, "data/models/pingyao/pingyao.gltf"        },
    { "Havana",         "CU", 26, 78, "data/models/havana/havana.gltf"          },
    { "Zanzibar",       "TZ", 27, 78, "data/models/zanzibar/zanzibar.gltf"      },
    { "Bern",           "CH", 10, 75, "data/models/bern/bern.gltf"              },
    { "Freiburg",       "DE", 12, 72, "data/models/freiburg/freiburg.gltf"      },
    { "Stockholm",      "SE",  8, 75, "data/models/stockholm/stockholm.gltf"    },
    { "Riga",           "LV",  7, 78, "data/models/riga/riga.gltf"              },
    { "Vilnius",        "LT",  7, 77, "data/models/vilnius/vilnius.gltf"        },
    { "Pripyat",        "UA",  9, 73, "data/models/pripyat/pripyat.gltf"        },
    { "Mainz",          "DE", 13, 72, "data/models/mainz-core/mainz-core.gltf"  },
    //  Terrain / Volcano meshes from DEM (Copernicus GLO-30 / SRTM) rather than OSM buildings.
    { "Fujihama",       "JP", 15, 70, "data/models/tokyo-fujihama/tokyo-fujihama.gltf"      },
    { "Popocatépetl",   "MX",  8, 52, "data/models/mexico-popo/mexico-popo.gltf"            },
    { "Pichincha",      "EC", 13, 75, "data/models/quito-pichincha/quito-pichincha.gltf"    },
    { "Naples",         "IT", 18, 65, "data/models/naples/naples.gltf"                      },
    { "Taranaki",       "NZ", 12, 80, "data/models/nz-taranaki/nz-taranaki.gltf"            },
};
constexpr int kCityCount = static_cast<int>(sizeof(kCities) / sizeof(kCities[0]));

// Module-level state

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

// Header SelectBox widget handles (nodes appended to #clima-cond-host / #clima-city-host)
pce::sdlos::NodeHandle g_cond_select_h;  ///< condition SelectBox node
pce::sdlos::NodeHandle g_city_select_h;  ///< city SelectBox node

// Renderer pointer — stored at init for weather uniform updates.
pce::sdlos::SDLRenderer *g_renderer = nullptr;

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

// Animated camera.
// While !finished() the per-frame update reads .current(); once settled
// the gentle auto-rotation resumes by advancing g_anim_yaw each frame.
::pce::sdlos::Animated<float> g_anim_yaw;
::pce::sdlos::Animated<float> g_anim_pitch;
::pce::sdlos::Animated<float> g_anim_dist;

// Live viewport size — updated every frame from the Scene3DHook.
// Seeded from SDLOS_WIN_W/H so the very first tick() and perspective()
// call have a sane default (BUT SDLOS_WIN_ is only the initial windowSize)
// before the first rendered frame arrives.
// TODO current viewport w, current viewport h?
float  g_vw = static_cast<float>(SDLOS_WIN_W);
float  g_vh = static_cast<float>(SDLOS_WIN_H);

}  // namespace

namespace {

static void setText(pce::sdlos::NodeHandle h, std::string_view text) noexcept {
    // write text content into a node
    if (!h.valid() || !g_tree) return;
    if (pce::sdlos::RenderNode *n = g_tree->node(h)) {
        n->setStyle("text", std::string{text});
        g_tree->markDirty(h);
    }
}

static void setStyleProp(pce::sdlos::NodeHandle h,
                         std::string_view key, std::string_view val) noexcept {
    if (!h.valid() || !g_tree) return;
    if (pce::sdlos::RenderNode *n = g_tree->node(h)) {
        n->setStyle(key, std::string{val});
        g_tree->markDirty(h);
    }
}

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

// Smoothly transition the orbit camera to a city's default position.
// Called when the user selects a different city.
// Uses .transition() so the move starts from wherever the camera is now
// (including any user-dragged position captured by the last mouse-up snap).
static void transitionCamera(int city_idx) noexcept {
    if (city_idx < 0 || city_idx >= kCityCount) return;
    const CityInfo &c = kCities[city_idx];
    constexpr float kDurMs = 1200.f;
    // Animated<T>::transition() already defaults to easeInOut — no need to
    // specify the easing function explicitly on every call site.
    g_anim_yaw.transition(c.default_yaw_deg,    kDurMs);
    g_anim_pitch.transition(c.default_pitch_deg, kDurMs);
    g_anim_dist.transition(c.default_dist,        kDurMs);
}

// Lazy city-mesh loader
//
// Called from the clima:city subscriber.
// Releases the previously loaded mesh (if any), updates src= on the scene3d
// node, and calls GltfScene::attach() to upload the new mesh to the GPU.

static void loadCityMesh(int city_idx) noexcept {
    if (!g_tree || !g_scene_ready) return;
    if (city_idx == g_loaded_city_idx) return;  // nothing to do

    const bool has_new_mesh =
        city_idx >= 0 && city_idx < kCityCount &&
        kCities[city_idx].mesh_path.has_value();

    // 1. Release existing mesh from GPU and hide the scene3d node.
    if (g_loaded_city_idx >= 0) {
        g_scene.clearMeshes(*g_tree);
    }
    setStyleProp(g_h_city_scene, "display", "none");
    g_loaded_city_idx = -1;

    if (!has_new_mesh) {
        sdlos_log(std::string{"[clima] city "} + std::string{kCities[city_idx].name}
                  + " — no mesh available");
        return;
    }

    // 2. Ensure the mesh directory exists so the mesh generator can write into it.
    //    If the .gltf file itself is missing, log and bail — the directory is
    //    created so `sdlos mesh generate --app clima` has a landing spot.
    {
        namespace fs = std::filesystem;
        const fs::path mesh_abs =
            fs::path(g_base_path) / *kCities[city_idx].mesh_path;
        const fs::path mesh_dir = mesh_abs.parent_path();

        if (!fs::exists(mesh_dir)) {
            std::error_code ec;
            fs::create_directories(mesh_dir, ec);
            if (ec) {
                sdlos_log("[clima] failed to create mesh dir: " + mesh_dir.string()
                          + " — " + ec.message());
            } else {
                sdlos_log("[clima] created mesh dir: " + mesh_dir.string());
            }
        }

        if (!fs::exists(mesh_abs)) {
            sdlos_log("[clima] mesh not yet generated for "
                      + std::string{kCities[city_idx].name}
                      + " — run: sdlos mesh generate --preset "
                      + std::string{kCities[city_idx].name}
                      + " --lod lowpoly --app clima");
            return;
        }
    }

    // 4. Update src= on the scene3d node so attach() picks it up.
    if (pce::sdlos::RenderNode *sn = g_tree->node(g_h_city_scene)) {
        sn->setStyle("src", std::string{*kCities[city_idx].mesh_path});
        g_tree->markDirty(g_h_city_scene);
    }

    // 5. Load mesh into GPU via attach().
    const int prims = g_scene.attach(*g_tree, g_root, g_base_path);
    if (prims > 0) {
        g_loaded_city_idx = city_idx;
        setStyleProp(g_h_city_scene, "display", "block");
        sdlos_log(std::string{"[clima] loaded city mesh: "}
                  + std::string{kCities[city_idx].name}
                  + " (" + std::to_string(prims) + " primitives)");
    } else {
        sdlos_log(std::string{"[clima] city mesh not found or empty: "}
                  + std::string{kCities[city_idx].mesh_path.value_or("(unknown)")});
    }
}

// Weather display update
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

    // The root node is always transparent — the 3D pre-pass (or the plain
    // window clear colour) shows through.  Only the card gets a weather tint;
    // when a mesh is active, drop the card to ~85 % opacity so the city is
    // faintly visible behind it.
    const bool mesh_active = (g_loaded_city_idx >= 0);
    const std::string card_bg = mesh_active
        ? std::string{w.card_bg} + "d8"
        : std::string{w.card_bg};

    setStyleProp(g_h_card, "backgroundColor", card_bg);

    // Keep the background sky shader in sync with the current weather state.
    if (g_renderer)
        g_renderer->setWeatherUniform(static_cast<float>(g_weather_idx));

    setText(g_h_icon,      w.icon);
    setText(g_h_temp,      std::to_string(temp_c)   + "\xC2\xB0\x43");
    setText(g_h_condition, w.name);
    setText(g_h_city_name, std::string{c.name} + ", " + std::string{c.country_code});
    setText(g_h_desc,      w.description);
    setText(g_h_feels,     std::to_string(feels_c)  + "\xC2\xB0\x43");
    setText(g_h_range,
            std::to_string(low_c) + "\xC2\xB0 \xE2\x80\x94 "
            + std::to_string(high_c) + "\xC2\xB0");
    setText(g_h_humidity,  std::to_string(humidity) + "%");

    // SelectBox widgets stay in sync automatically: their on_change callback
    // drives g_weather_idx / g_city_idx, so no explicit sync call is needed here.
}

//  Clock update

static void updateClock() noexcept {
    if (!g_h_time.valid() || !g_tree) return;
    // localtime_r (POSIX) / localtime_s (MSVC) write into a caller-supplied tm —
    // no shared static buffer, thread-safe on all target platforms.
    const std::time_t now = std::time(nullptr);
    std::tm t{};
#if defined(_WIN32)
    localtime_s(&t, &now);
#else
    localtime_r(&now, &t);
#endif
    char buf[6];
    std::snprintf(buf, sizeof(buf), "%02d:%02d", t.tm_hour, t.tm_min);
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
    g_tree              = &tree;
    g_root              = root;
    g_weather_idx       = 0;
    g_city_idx          = 0;
    g_last_clock_ns     = SDL_GetTicksNS();

    // Store renderer for live weather-uniform updates.
    g_renderer = &renderer;

    // 1. Cache 2D node handles
    g_h_root_node       = root;
    g_h_card            = tree.findById(root, "clima-card");
    g_h_icon            = tree.findById(root, "clima-icon");
    g_h_temp            = tree.findById(root, "clima-temp");
    g_h_condition       = tree.findById(root, "clima-condition");
    g_h_city_name       = tree.findById(root, "clima-city-name");
    g_h_desc            = tree.findById(root, "clima-desc");
    g_h_feels           = tree.findById(root, "clima-feels");
    g_h_range           = tree.findById(root, "clima-range");
    g_h_humidity        = tree.findById(root, "clima-humidity");
    g_h_time            = tree.findById(root, "clima-time");
    g_h_city_scene      = tree.findById(root, "clima-city-scene");

    for (const char *host_id : { "clima-cond-host", "clima-city-host" }) {
        if (RenderNode *hn = tree.node(tree.findById(root, host_id))) {
            // Set a fixed height and width for the host divs so the header flex-row can centre them.
            hn->h = 30.f;
            hn->layout_props.height = 30.f;
            hn->w = 200.0f;
            hn->layout_props.width = 200.0f;
        }
    }

    // 2. Load clima.css
    {
        const char      *bp       = SDL_GetBasePath();
        g_base_path               = bp ? bp : "";

        // Swap in the Clima weather sky shader as the fullscreen background.
        // Falls back gracefully to the chrome default if the file is missing.
        renderer.ReloadShader(g_base_path + "assets/shaders/msl/clima_sky.frag.metal");
        renderer.setWeatherUniform(static_cast<float>(g_weather_idx));

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
                // Keep viewport globals fresh; rebuild projection on resize.
                if (vw != g_vw || vh != g_vh) {
                    g_vw = vw;
                    g_vh = vh;
                    g_scene.camera().perspective(45.f, g_vw / g_vh, 1.0f, 10000.0f);
                }
                g_scene.render(cmd, swap, vw, vh);
            });
        // Release GPU resources when the window closes.
        renderer.setGpuPreShutdownHook([]() noexcept { g_scene.shutdown(); });

        // Perspective camera: bird's-eye view for city meshes.
        // g_vw/g_vh hold the default window size on first call; the
        // Scene3DHook above corrects the aspect ratio on every resize.
        // near=1 m avoids z-fighting; far=10 000 m covers city-scale scenes.
        g_scene.camera().perspective(45.f, g_vw / g_vh, 1.0f, 10000.0f);
        updateCityCamera();

        sdlos_log("[clima] GltfScene ready — city meshes will load on demand");
    } else {
        sdlos_log("[clima] WARNING: GltfScene::init() failed — 3D disabled");
    }

    // 4. Prime 2D display
    updateDisplay();
    updateClock();

    // 4b. Load the default city mesh immediately (city 0 = Mainz).
    //     This runs after GltfScene::init() so g_scene_ready is already set.
    //     Without this call the mesh would only appear after the user clicks
    //     a city button, making the initial view appear mesh-less.
    loadCityMesh(g_city_idx);

    // Snap animated camera to the initial city defaults — no transition on first load.
    {
        const CityInfo &c0 = kCities[g_city_idx];
        g_anim_yaw.set(c0.default_yaw_deg);
        g_anim_pitch.set(c0.default_pitch_deg);
        g_anim_dist.set(c0.default_dist);
        g_city_yaw_deg   = c0.default_yaw_deg;
        g_city_pitch_deg = c0.default_pitch_deg;
        g_city_dist      = c0.default_dist;
        updateCityCamera();
    }

    // 5. Header SelectBox selectors — condition (5 opts) + city (25 opts).
    //    Created programmatically and appended to their jade host divs so
    //    they participate naturally in the header's flex row layout.
    {
        namespace ws = widgets;

        // Shared glassmorphic colour theme (translucent white on any bg)
        constexpr ws::Color kBg        = ws::Color::hex(0xff,0xff,0xff,0x22);
        constexpr ws::Color kBgOpen    = ws::Color::hex(0xff,0xff,0xff,0x38);
        constexpr ws::Color kBorder    = ws::Color::hex(0xff,0xff,0xff,0x30);
        constexpr ws::Color kBordOpen  = ws::Color::hex(0xff,0xff,0xff,0x80);
        constexpr ws::Color kText      = ws::Color::hex(0xff,0xff,0xff,0xee);
        constexpr ws::Color kArrow     = ws::Color::hex(0xff,0xff,0xff,0x80);
        constexpr ws::Color kDropBg    = ws::Color::hex(0x14,0x14,0x20,0xf4);
        constexpr ws::Color kItemTxt   = ws::Color::hex(0xff,0xff,0xff,0xcc);
        constexpr ws::Color kItemSel   = ws::Color::hex(0x5a,0xd3,0xfa,0xff);
        constexpr ws::Color kItemHov   = ws::Color::hex(0xff,0xff,0xff,0x18);
        constexpr ws::Color kItemSelBg = ws::Color::hex(0x0a,0x84,0xff,0x35);

        // Condition SelectBox (5 weather types)
        std::vector<ws::SelectOption> cond_opts;
        cond_opts.reserve(kWeatherCount);
        for (int i = 0; i < kWeatherCount; ++i)
            cond_opts.push_back({ std::to_string(i),
                std::string{kWeather[i].icon} + "  " + std::string{kWeather[i].name} });

        ws::SelectBox cond_sel = ws::makeSelectBox(tree, {
            .options     = cond_opts,
            .selected    = "0",
            .w = 152.f,  .h = 30.f,  .item_h = 28.f,  .font_size = 13.f,
            .padding     = ws::Edges::horizontal(10.f),
            .bg = kBg,   .bg_open = kBgOpen,
            .border = kBorder,  .border_open = kBordOpen,
            .text_color  = kText,   .arrow_color = kArrow,
            .dropdown_bg = kDropBg,
            .item_hover_bg     = kItemHov,   .item_selected_bg  = kItemSelBg,
            .item_text         = kItemTxt,   .item_selected_text = kItemSel,
            .on_change = [](std::string_view val) noexcept {
                try {
                    const int idx = std::stoi(std::string{val});
                    if (idx >= 0 && idx < kWeatherCount) {
                        g_weather_idx = idx;
                        updateDisplay();
                    }
                } catch (...) {}
            },
        });
        const NodeHandle cond_host_h = tree.findById(root, "clima-cond-host");
        tree.appendChild(cond_host_h, cond_sel.handle);
        g_cond_select_h = cond_sel.handle;

        // City SelectBox (cities built from kCities[])
        //    item_h=22 keeps the full list visible in the ~576 px below the header.
        std::vector<ws::SelectOption> city_opts;
        city_opts.reserve(kCityCount);
        for (int i = 0; i < kCityCount; ++i)
            city_opts.push_back({ std::to_string(i),
                std::string{kCities[i].name} + ", " + std::string{kCities[i].country_code} });

        ws::SelectBox city_sel = ws::makeSelectBox(tree, {
            .options     = city_opts,
            .selected    = "0",
            .w = 210.f,  .h = 30.f,  .item_h = 22.f,  .font_size = 13.f,
            .padding     = ws::Edges::horizontal(10.f),
            .bg = kBg,   .bg_open = kBgOpen,
            .border = kBorder,  .border_open = kBordOpen,
            .text_color  = kText,   .arrow_color = kArrow,
            .dropdown_bg = kDropBg,
            .item_hover_bg     = kItemHov,   .item_selected_bg  = kItemSelBg,
            .item_text         = kItemTxt,   .item_selected_text = kItemSel,
            .on_change = [](std::string_view val) noexcept {
                try {
                    const int idx = std::stoi(std::string{val});
                    if (idx >= 0 && idx < kCityCount) {
                        g_city_idx = idx;
                        loadCityMesh(idx);
                        transitionCamera(idx);
                        updateDisplay();
                    }
                } catch (...) {}
            },
        });
        const NodeHandle city_host_h = tree.findById(root, "clima-city-host");
        tree.appendChild(city_host_h, city_sel.handle);
        g_city_select_h = city_sel.handle;
    }

    // 6. Per-frame update: clock, auto-rotate city mesh, 3D tick
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
                    const bool animating =
                        !g_anim_yaw.finished() ||
                        !g_anim_pitch.finished() ||
                        !g_anim_dist.finished();

                    if (animating) {
                        // Apply the smooth city-transition animation.
                        g_city_yaw_deg   = g_anim_yaw.current();
                        g_city_pitch_deg = g_anim_pitch.current();
                        g_city_dist      = g_anim_dist.current();
                    } else {
                        // Animation settled — resume gentle auto-rotation.
                        g_city_yaw_deg += kAutoRotateDps * dt;
                        // Keep the animated value in sync so the next
                        // city transition starts from the rotated position.
                        g_anim_yaw.set(g_city_yaw_deg);
                    }
                    updateCityCamera();
                }
                // g_vw/g_vh are kept current by the Scene3DHook each frame.
                g_scene.tick(*g_tree, g_vw, g_vh);
            }
        };
    }

    // 7. Raw event hook: SelectBox routing → orbit/zoom/hover
    out_handler = [](const SDL_Event &ev) -> bool {
        // SelectBox widgets get first priority — they need to consume mouse-down
        // (open/close dropdown), mouse-motion (hover highlight), and key events
        // (Up/Down navigate, Enter confirm, Escape close).
        if (g_cond_select_h.valid() && g_tree) {
            widgets::SelectBox sb{*g_tree, g_cond_select_h};
            if (sb.handleEvent(ev)) return true;
        }
        if (g_city_select_h.valid() && g_tree) {
            widgets::SelectBox sb{*g_tree, g_city_select_h};
            if (sb.handleEvent(ev)) return true;
        }

        switch (ev.type) {
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                if (ev.button.button == SDL_BUTTON_RIGHT)
                    g_dragging = true;
                break;
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (ev.button.button == SDL_BUTTON_RIGHT) {
                    g_dragging = false;
                    // Snap animated values to wherever the user dragged to.
                    // The next transitionCamera() call will start from here,
                    // not from the previous city default — feels natural.
                    g_anim_yaw.set(g_city_yaw_deg);
                    g_anim_pitch.set(g_city_pitch_deg);
                    g_anim_dist.set(g_city_dist);
                    // Log camera state so the user can copy good values into code.
                    sdlos_log("[clima] camera  yaw="
                        + std::to_string(static_cast<int>(g_city_yaw_deg))
                        + "  pitch=" + std::to_string(static_cast<int>(g_city_pitch_deg))
                        + "  dist="  + std::to_string(static_cast<int>(g_city_dist))
                        + "  →  g_city_yaw_deg=" + std::to_string(g_city_yaw_deg)
                        + "f, g_city_pitch_deg=" + std::to_string(g_city_pitch_deg)
                        + "f, g_city_dist="      + std::to_string(g_city_dist) + "f;");
                }
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
            case SDL_EVENT_KEY_DOWN:
                // C — dump current camera state to console at any time
                if (ev.key.scancode == SDL_SCANCODE_C && g_scene_ready) {
                    sdlos_log("[clima] camera  yaw="
                        + std::to_string(static_cast<int>(g_city_yaw_deg))
                        + "  pitch=" + std::to_string(static_cast<int>(g_city_pitch_deg))
                        + "  dist="  + std::to_string(static_cast<int>(g_city_dist))
                        + "  →  g_city_yaw_deg=" + std::to_string(g_city_yaw_deg)
                        + "f, g_city_pitch_deg=" + std::to_string(g_city_pitch_deg)
                        + "f, g_city_dist="      + std::to_string(g_city_dist) + "f;");
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
