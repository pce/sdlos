#include "audio/sfx_player.h"
#include "gltf/gltf_scene.h"
#include "core/animated.h"
#include "core/easing.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace {

// Preset definitions
// Maps a display name → shader file name (without extension/path).
// "none" means clear _shader so the engine uses plain drawImage.

struct Preset {
    std::string display;  // data-value in jade
    std::string shader;   // _shader attr value (empty = no shader)
    float       param0;   // blurScale or equivalent
    float       param1;
    float       param2;
};

const Preset kPresets[] = {
    { "perlin_noise",  "perlin_noise", 50.f, 0.02f, 4.f },
    { "tilt_blur",     "tiltblur",     50.f,  0.f,  0.f },
    { "contrast",      "contrast",      1.f,  0.f,  0.f },
    { "vignette",      "vignette",      1.f,  0.f,  0.f },
    { "none",          "",              0.f,  0.f,  0.f },
};


struct Param {
    std::string            style_key;  // _shader_param0 / _shader_param1 / _shader_param2
    std::string            val_id;     // node id of the display div in jade
    float                  value;
    float                  step;
    float                  min_val;
    float                  max_val;
    pce::sdlos::NodeHandle display_h = pce::sdlos::k_null_handle;
};

struct ShadeState {
    std::string active_preset = "tilt_blur";

    std::vector<Param> params = {
        { "_shader_param0", "val-scale", 50.f, 5.f,   0.f, 200.f },
        { "_shader_param1", "val-freq",  0.f,  0.005f, 0.f, 1.f  },
        { "_shader_param2", "val-oct",   0.f,  1.f,   0.f, 8.f   },
    };

    pce::sdlos::NodeHandle canvas_h      = pce::sdlos::k_null_handle;
    pce::sdlos::NodeHandle active_name_h = pce::sdlos::k_null_handle;

    pce::sdlos::SfxPlayer sfx;

    // Animal state
    pce::sdlos::gltf::GltfScene* scene3d      = nullptr;
    pce::sdlos::NodeHandle      scene_node_h = pce::sdlos::k_null_handle;
    pce::sdlos::NodeHandle      animal_h     = pce::sdlos::k_null_handle;
    bool                        show_animal  = false;

    // Pirate state
    pce::sdlos::gltf::GltfScene* pirate_scene  = nullptr;
    pce::sdlos::NodeHandle      pirate_node_h = pce::sdlos::k_null_handle;
    pce::sdlos::NodeHandle      ship_h        = pce::sdlos::k_null_handle;
    bool                        show_pirate   = false;

    ~ShadeState() {
        delete scene3d;
        delete pirate_scene;
    }

    // Procedural animation for walking (hopping)
    pce::sdlos::Animated<float> hop_y;
    pce::sdlos::Animated<float> roll_x;
    float                       step_timer = 0.f;

    // Pirate ship animation
    pce::sdlos::Animated<float> ship_x;
    pce::sdlos::Animated<float> ship_tilt;
};


[[nodiscard]] static std::string fmtParam(float v) noexcept
{
    char buf[32];
    if (v == std::floor(v) && v >= -9999.f && v <= 9999.f)
        std::snprintf(buf, sizeof(buf), "%.0f", static_cast<double>(v));
    else
        std::snprintf(buf, sizeof(buf), "%.3f", static_cast<double>(v));
    return buf;
}

static void setCanvasAttr(pce::sdlos::RenderTree& tree,
                           pce::sdlos::NodeHandle canvas_h,
                           const std::string& key,
                           const std::string& val)
{
    if (pce::sdlos::RenderNode* n = tree.node(canvas_h)) {
        n->setStyle(key, val);
        n->dirty_render = true;
    }
}

static void refreshParam(pce::sdlos::RenderTree& tree,
                          ShadeState& s,
                          std::size_t idx)
{
    auto& p = s.params[idx];
    // Update display div
    if (p.display_h.valid()) {
        if (pce::sdlos::RenderNode* n = tree.node(p.display_h)) {
            n->setStyle("text", fmtParam(p.value));
            n->dirty_render = true;
        }
    }
    // Push value to canvas shader attr
    if (s.canvas_h.valid())
        setCanvasAttr(tree, s.canvas_h, p.style_key, fmtParam(p.value));
}

static void applyPreset(pce::sdlos::RenderTree& tree,
                         ShadeState& s,
                         const std::string& name,
                         const std::vector<pce::sdlos::NodeHandle>& presets)
{
    // Find preset definition
    const Preset* p = nullptr;
    for (const auto& pr : kPresets)
        if (pr.display == name) { p = &pr; break; }

    if (p) {
        s.active_preset  = name;
        s.params[0].value = p->param0;
        s.params[1].value = p->param1;
        s.params[2].value = p->param2;

        // Update canvas _shader and params
        if (s.canvas_h.valid()) {
            setCanvasAttr(tree, s.canvas_h, "_shader", p->shader);
            setCanvasAttr(tree, s.canvas_h, "_shader_param0", fmtParam(p->param0));
            setCanvasAttr(tree, s.canvas_h, "_shader_param1", fmtParam(p->param1));
            setCanvasAttr(tree, s.canvas_h, "_shader_param2", fmtParam(p->param2));
        }
    }

    // Active highlight is now handled by CSS :active + toggle-group.
    // jade_host calls css_sheet.activateNode() before publishing the bus
    // event, so by the time this handler runs the visual state is already
    // correct.  No manual backgroundColor manipulation needed here.

    // Refresh all param display divs
    for (std::size_t i = 0; i < s.params.size(); ++i)
        refreshParam(tree, s, i);

    // Update active name label
    if (s.active_name_h.valid()) {
        if (pce::sdlos::RenderNode* n = tree.node(s.active_name_h)) {
            n->setStyle("text", name);
            n->dirty_render = true;
        }
    }
}

} // namespace


void jade_app_init(pce::sdlos::RenderTree&               tree,
                   pce::sdlos::NodeHandle                 root,
                   pce::sdlos::IEventBus&                 bus,
                   pce::sdlos::SDLRenderer&               renderer,
                   std::function<bool(const SDL_Event&)>& /*out_handler*/)
{
    // Audio subsystem — SDL reference-counts so safe to call multiple times.
    SDL_InitSubSystem(SDL_INIT_AUDIO);

    auto state = std::make_shared<ShadeState>();

    const auto presets = tree.findByClass(root, "preset");

    state->scene3d = new pce::sdlos::gltf::GltfScene();
    state->pirate_scene = new pce::sdlos::gltf::GltfScene();

    sdlos_log(std::string("[shade] canvas=")
              + (state->canvas_h.valid() ? "ok" : "MISSING")
              + "  presets=" + std::to_string(presets.size()));

    // Load UI sound effects via VFS
    // data/sounds/ is copied next to the binary by CMake (DATA_DIR).
    // The host VFS mounts the binary dir as "asset://".
    // When no VFS is attached, SfxPlayer falls back to SDL_IOFromFile.
    {
        const char* base = SDL_GetBasePath();
        const std::string snd = base ? std::string(base) + "data/sounds/" : "data/sounds/";

        // "click" — short percussive hit for button presses
        state->sfx.load("click",  snd + "nf-shot-01.wav");
        // "select" — slightly longer tone for preset selection
        state->sfx.load("select", snd + "neueis_13.wav");
        // "tick" — subtle tick for +/- steppers
        state->sfx.load("tick",   snd + "neueis_14.wav");

        // Snow/Step sounds for animal
        state->sfx.load("step1", snd + "neueis_15.wav");
        state->sfx.load("step2", snd + "neueis_16.wav");
    }

    // Initialize 3D Scene
    const char* base_path = SDL_GetBasePath();
    state->scene3d->init(renderer.GetDevice(),
                        renderer.GetShaderFormat(),
                        base_path ? base_path : "",
                        SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM);

    state->pirate_scene->init(renderer.GetDevice(),
                             renderer.GetShaderFormat(),
                             base_path ? base_path : "",
                             SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM);


    // Register 3D render hook
    renderer.setScene3DHook([state](SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* swap, float vw, float vh) {
        if (state->show_animal) state->scene3d->render(cmd, swap, vw, vh);
        if (state->show_pirate) state->pirate_scene->render(cmd, swap, vw, vh);
    });

    // Register cleanup hook
    renderer.setGpuPreShutdownHook([state]() {
        state->scene3d->shutdown();
        state->pirate_scene->shutdown();
    });

    // Locate nodes
    state->canvas_h      = tree.findById(root, "shader-canvas");
    state->active_name_h = tree.findById(root, "active-name");
    state->scene_node_h  = tree.findById(root, "animal-scene");
    state->pirate_node_h = tree.findById(root, "pirate-scene");

    // Attach animal to scene3d
    if (state->scene_node_h.valid()) {
        state->scene3d->attach(tree, state->scene_node_h, base_path ? base_path : "");
        state->animal_h = tree.findById(root, "animal");
        if (state->animal_h.valid()) {
            pce::sdlos::RenderNode* n = tree.node(state->animal_h);
            n->setStyle("--scale", "3.0");
        }
    }

    // Attach pirate ship
    if (state->pirate_node_h.valid()) {
        state->pirate_scene->attach(tree, state->pirate_node_h, base_path ? base_path : "");
        state->ship_h = tree.findById(root, "pirate-ship");
        if (state->ship_h.valid()) {
            pce::sdlos::RenderNode* n = tree.node(state->ship_h);
            n->setStyle("--scale", "0.5");
        }
    }

    // Set up cameras
    state->scene3d->camera().perspective(45.f, 16.f/9.f);
    state->scene3d->camera().lookAt(0, 2, 5, 0, 0, 0);

    state->pirate_scene->camera().perspective(45.f, 16.f/9.f);
    state->pirate_scene->camera().lookAt(5, 5, 12, 0, 0, 0);

    //  shade:animal-show
    bus.subscribe("shade:animal-show", [state, &tree](const std::string&) {
        state->show_animal = !state->show_animal;
        state->show_pirate = false;
        if (pce::sdlos::RenderNode* n = tree.node(state->scene_node_h)) {
            n->setStyle("display", state->show_animal ? "block" : "none");
            n->dirty_render = true;
        }
        if (pce::sdlos::RenderNode* p = tree.node(state->pirate_node_h)) {
            p->setStyle("display", "none");
            p->dirty_render = true;
        }
        if (pce::sdlos::RenderNode* c = tree.node(state->canvas_h)) {
            c->setStyle("display", state->show_animal ? "none" : "block");
            c->dirty_render = true;
        }
        state->sfx.play("select");
    });

    // shade:pirate-show
    bus.subscribe("shade:pirate-show", [state, &tree](const std::string&) {
        state->show_pirate = !state->show_pirate;
        state->show_animal = false;
        if (pce::sdlos::RenderNode* p = tree.node(state->pirate_node_h)) {
            p->setStyle("display", state->show_pirate ? "block" : "none");
            p->dirty_render = true;
        }
        if (pce::sdlos::RenderNode* n = tree.node(state->scene_node_h)) {
            n->setStyle("display", "none");
            n->dirty_render = true;
        }
        if (pce::sdlos::RenderNode* c = tree.node(state->canvas_h)) {
            c->setStyle("display", state->show_pirate ? "none" : "block");
            c->dirty_render = true;
        }
        state->sfx.play("select");

        if (state->show_pirate) {
            state->ship_x.transition(10.f, 5000.f, pce::sdlos::easing::easeInOut);
        }
    });

    //  shade:preset
    bus.subscribe("shade:preset",
        [&tree, state, presets](const std::string& name) {
            sdlos_log("[shade:preset] " + name);
            state->sfx.play("select");          // ← UI sound on preset click
            applyPreset(tree, *state, name, presets);
        });

    // shade:inc / shade:dec
    // data-value on the +/- buttons is "scale", "freq", or "oct" which maps
    // to param indices 0, 1, 2 respectively.
    auto paramIdx = [](const std::string& key) -> int {
        if (key == "scale") return 0;
        if (key == "freq")  return 1;
        if (key == "oct")   return 2;
        return -1;
    };

    bus.subscribe("shade:inc",
        [&tree, state, paramIdx](const std::string& key) {
            const int i = paramIdx(key);
            if (i < 0) { sdlos_log("[shade] unknown param: " + key); return; }
            auto& p = state->params[static_cast<std::size_t>(i)];
            p.value = std::clamp(p.value + p.step, p.min_val, p.max_val);
            refreshParam(tree, *state, static_cast<std::size_t>(i));
            state->sfx.play("tick");            // ← UI sound on increment
            sdlos_log("[shade:inc] " + key + " = " + fmtParam(p.value));
        });

    bus.subscribe("shade:dec",
        [&tree, state, paramIdx](const std::string& key) {
            const int i = paramIdx(key);
            if (i < 0) { sdlos_log("[shade] unknown param: " + key); return; }
            auto& p = state->params[static_cast<std::size_t>(i)];
            p.value = std::clamp(p.value - p.step, p.min_val, p.max_val);
            refreshParam(tree, *state, static_cast<std::size_t>(i));
            state->sfx.play("tick");            // ← UI sound on decrement
            sdlos_log("[shade:dec] " + key + " = " + fmtParam(p.value));
        });

    // Apply initial preset state so the canvas starts with the right shader
    applyPreset(tree, *state, state->active_preset, presets);

    // Per-frame update for animations
    if (pce::sdlos::RenderNode* rn = tree.node(root)) {
        static uint64_t last_tick = SDL_GetTicks();
        rn->update = [state, &tree]() {
            uint64_t now = SDL_GetTicks();
            float dt = (now - last_tick) / 1000.f;
            last_tick = now;

            if (state->show_animal && state->animal_h.valid()) {
                state->step_timer += dt;

                if (state->step_timer >= 0.5f) {
                    state->step_timer = 0.f;
                    // Using the new Bouncy Spring for the hop to give it more "weight"
                    state->hop_y.transition(0.4f, 150.f, pce::sdlos::easing::easeOutSpringBouncy);
                    // Using Snappy Spring for the roll to make it quick and reactive
                    state->roll_x.transition(10.f, 150.f, pce::sdlos::easing::easeOutSpringSnappy);
                    state->sfx.play("step1");
                }

                if (state->hop_y.finished() && state->hop_y.to > 0.f) {
                    // Settle with a standard spring
                    state->hop_y.transition(0.f, 250.f, pce::sdlos::easing::easeOutSpring);
                    state->roll_x.transition(0.f, 250.f, pce::sdlos::easing::easeInOut);
                }

                if (pce::sdlos::RenderNode* n = tree.node(state->animal_h)) {
                    char buf_y[32], buf_r[32];
                    std::snprintf(buf_y, sizeof(buf_y), "%.3f", state->hop_y.current());
                    std::snprintf(buf_r, sizeof(buf_r), "%.1f", state->roll_x.current());
                    n->setStyle("--translate-y", buf_y);
                    n->setStyle("--rotation-x", buf_r);
                    n->dirty_render = true;
                    tree.markDirty(state->animal_h);
                }
                state->scene3d->tick(tree, 1280.f, 720.f);
            }

            if (state->show_pirate && state->ship_h.valid()) {
                // Loop ship movement
                if (state->ship_x.finished()) {
                    float target = (state->ship_x.to > 0.f) ? -10.f : 10.f;
                    state->ship_x.transition(target, 8000.f, pce::sdlos::easing::easeInOut);
                }

                // Periodic bobbing/rocking
                float time = now / 1000.f;
                float bob = std::sin(time * 2.f) * 0.2f;
                float tilt = std::cos(time * 1.5f) * 3.f;

                if (pce::sdlos::RenderNode* n = tree.node(state->ship_h)) {
                    char buf_x[32], buf_y[32], buf_r[32];
                    std::snprintf(buf_x, sizeof(buf_x), "%.3f", state->ship_x.current());
                    std::snprintf(buf_y, sizeof(buf_y), "%.3f", bob);
                    std::snprintf(buf_r, sizeof(buf_r), "%.1f", tilt);
                    n->setStyle("--translate-x", buf_x);
                    n->setStyle("--translate-y", buf_y);
                    n->setStyle("--rotation-z", buf_r);
                    n->dirty_render = true;
                    tree.markDirty(state->ship_h);
                }
                state->pirate_scene->tick(tree, 1280.f, 720.f);
            }
        };
    }
}
