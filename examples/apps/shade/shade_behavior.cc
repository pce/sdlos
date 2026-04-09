
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

// ── SfxPlayer ─────────────────────────────────────────────────────────────────
//
// Fire-and-forget sound effects.  Pre-loads WAV files from disk into memory;
// play() opens a short-lived SDL3 audio stream, puts the PCM data, and lets
// it drain — no mixing library needed for simple UI bleeps.
//
// Usage:
//   sfx.load("click",  basePath + "data/sounds/nf-shot-01.wav");
//   sfx.load("select", basePath + "data/sounds/neueis_13.wav");
//   sfx.play("click");   // fire-and-forget
//
// NOTE: Each play() call opens its own audio device stream so multiple
//       overlapping sounds work.  For heavy mixing consider SDL_mixer.

struct SfxClip {
    SDL_AudioSpec  spec{};
    std::vector<Uint8> pcm;   // decoded PCM data
};

struct SfxPlayer {
    std::unordered_map<std::string, SfxClip> clips;

    /// Load a WAV file from disk and store it under `name`.
    bool load(const std::string& name, const std::string& path) {
        SDL_IOStream* io = SDL_IOFromFile(path.c_str(), "rb");
        if (!io) {
            sdlos_log("[sfx] cannot open: " + path + " — " + SDL_GetError());
            return false;
        }

        SDL_AudioSpec spec{};
        Uint8*  buf = nullptr;
        Uint32  len = 0;
        if (!SDL_LoadWAV_IO(io, true, &spec, &buf, &len)) {
            sdlos_log("[sfx] load failed: " + path + " — " + SDL_GetError());
            return false;
        }

        SfxClip clip;
        clip.spec = spec;
        clip.pcm.assign(buf, buf + len);
        SDL_free(buf);

        clips[name] = std::move(clip);
        sdlos_log("[sfx] loaded '" + name + "' ← " + path
                  + "  (" + std::to_string(len) + " bytes)");
        return true;
    }

    /// Play a previously loaded clip.  Fire-and-forget: the stream is leaked
    /// intentionally — SDL3 reclaims it when playback finishes.
    void play(const std::string& name) {
        auto it = clips.find(name);
        if (it == clips.end()) return;

        const SfxClip& clip = it->second;
        SDL_AudioStream* stream = SDL_OpenAudioDeviceStream(
            SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &clip.spec, nullptr, nullptr);
        if (!stream) return;

        SDL_PutAudioStreamData(stream,
            static_cast<const void*>(clip.pcm.data()),
            static_cast<int>(clip.pcm.size()));
        SDL_FlushAudioStream(stream);
        SDL_ResumeAudioStreamDevice(stream);
        // Stream will drain and SDL3 will reclaim it.
    }
};


// ── Preset definitions ────────────────────────────────────────────────────────
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

    SfxPlayer sfx;
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
                   pce::sdlos::SDLRenderer&               /*renderer*/,
                   std::function<bool(const SDL_Event&)>& /*out_handler*/)
{
    // Audio subsystem — SDL reference-counts so safe to call multiple times.
    SDL_InitSubSystem(SDL_INIT_AUDIO);

    auto state = std::make_shared<ShadeState>();

    // ── Load UI sound effects ─────────────────────────────────────────────────
    // data/sounds/ is copied next to the binary by CMake (DATA_DIR).
    {
        const char* base = SDL_GetBasePath();
        const std::string snd = base ? std::string(base) + "data/sounds/" : "data/sounds/";

        // "click" — short percussive hit for button presses
        state->sfx.load("click",  snd + "nf-shot-01.wav");
        // "select" — slightly longer tone for preset selection
        state->sfx.load("select", snd + "neueis_13.wav");
        // "tick" — subtle tick for +/- steppers
        state->sfx.load("tick",   snd + "neueis_14.wav");
    }

    // Locate nodes
    state->canvas_h      = tree.findById(root, "shader-canvas");
    state->active_name_h = tree.findById(root, "active-name");
    for (auto& p : state->params)
        p.display_h = tree.findById(root, p.val_id);

    const auto presets = tree.findByClass(root, "preset");

    sdlos_log(std::string("[shade] canvas=")
              + (state->canvas_h.valid() ? "ok" : "MISSING")
              + "  presets=" + std::to_string(presets.size()));

    // ── shade:preset ─────────────────────────────────────────────────────────
    bus.subscribe("shade:preset",
        [&tree, state, presets](const std::string& name) {
            sdlos_log("[shade:preset] " + name);
            state->sfx.play("select");          // ← UI sound on preset click
            applyPreset(tree, *state, name, presets);
        });

    // ── shade:inc / shade:dec ─────────────────────────────────────────────────
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
}
