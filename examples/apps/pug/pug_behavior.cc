// =============================================================================
// pug_behavior.cc  —  [pug] behaviour for the pug app
// =============================================================================
//
// This file is #include-d directly into jade_host.cc at compile time.
// All declarations in jade_host.cc are visible here without extra includes.
// The entry point is jade_app_init() — called once after the scene is fully
// parsed, styled, and layout-bound.
//
// FrameGraph integration
// ----------------------
// The render pipeline lives in data/pipeline.pug (three passes):
//
//   pass#bg        — animated FBM noise background     → bg_color (rgba16f)
//   pass#vignette  — radial vignette + edge pulse      → vignette_buffer (rgba16f)
//   pass#grade     — colour grading (exp/gamma/sat)    → swapchain
//
// SDLRenderer::SetDataBasePath() auto-loads pipeline.pug when it is present.
// This behavior loads data/pipeline.css explicitly and applies it so that
// scoped theme rules (e.g. pipeline.night #grade { … }) take effect.
//
// Theme switching
// ---------------
// Theme chips fire pug:theme (payload = "default"|"night"|"vivid").
// The callback calls FrameGraph::add_class / remove_class, which re-runs all
// scoped CSS rules for the new class — no recompile needed for Bucket-C changes.
//
// Quality switching
// -----------------
// Quality chips fire pug:quality (payload = "normal"|"low").
// "low"    → adds "low-power" CSS class  → disables vignette pass (zero cost).
// "normal" → removes "low-power" class   → re-enables vignette.
//
// Live parameter controls
// -----------------------
// +/– steppers fire pug:inc / pug:dec (payload = "pass:key").
// The callback calls CompiledGraph::patch(pass, key, val) — one float write,
// no string map lookup in the hot path.  The GPU sees the change next frame.
//
// Lifecycle note on GetCompiledGraph()
// ------------------------------------
// CompiledGraph is built on the first Render() call (swapchain format is
// needed).  By the time any bus callback fires (user interaction), the first
// frame will already have rendered.  All callbacks guard against null anyway.
//
// EventBus topics consumed
// ────────────────────────
//   pug:theme    payload = "default" | "night" | "vivid"
//   pug:quality  payload = "normal"  | "low"
//   pug:inc      payload = "pass:key"  (e.g. "bg:speed")
//   pug:dec      payload = "pass:key"
//
// Customisation guide
// -------------------
//   1. Add rows to the params vector inside the user region — make sure the
//      pass_id and key match an attr declared in data/pipeline.pug, and add
//      a matching val node id in pug.jade.
//   2. Add more CSS class names (themes) by subscribing to additional bus
//      topics and calling fg->add_class / remove_class.
//   3. Put new Metal fragment shaders in data/shaders/msl/ and reference them
//      as shader="my_shader" on a new pass in data/pipeline.pug.
//   4. Run  sdlos pipeline pug  to validate pipeline wiring in the terminal.
//
// Regeneration
// ------------
//   sdlos create pug --overwrite
//   Code between "enter the forrest" / "back to the sea" markers is preserved.
//
// Performance notes
// -----------------
//   • CompiledGraph::patch() is O(pass_count) — called only on UI interaction.
//   • FrameGraph::add_class() / remove_class() walk CSS rules once per call —
//     never called per frame.
//   • GetCompiledGraph() returns a cached pointer; no allocation or lock.
// =============================================================================

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {

// ── Live param descriptor ─────────────────────────────────────────────────────
//
// Each entry maps a UI stepper pair to one Bucket-C float in a pipeline pass.
//
//   pass_id    — pass id string as declared in pipeline.pug  (e.g. "bg")
//   key        — param key string (alphabetical order in pug) (e.g. "speed")
//   display_id — jade node id of the value display div       (e.g. "val-bg-speed")
//   value      — current runtime value (mirrored from pipeline.pug defaults)
//   step       — amount to increment / decrement per button press
//   min_v      — clamped minimum
//   max_v      — clamped maximum
//   display_h  — resolved node handle (set in jade_app_init)

struct LiveParam {
    const char* pass_id;
    const char* key;
    const char* display_id;
    float       value;
    float       step;
    float       min_v;
    float       max_v;
    pce::sdlos::NodeHandle display_h = pce::sdlos::k_null_handle;
};

// ── App state ─────────────────────────────────────────────────────────────────

struct PugState {
    // Active CSS class on the pipeline root.
    // "" = default (no class), "night", "vivid", …
    std::string active_theme;

    // Quality mode: "normal" (full pipeline) | "low" (vignette disabled).
    std::string active_quality = "normal";

    // Pipeline CSS — loaded once from data/pipeline.css.
    // Stored here so the shared_ptr keeps it alive for the lifetime of all
    // bus callback lambdas (which outlive jade_app_init).
    pce::sdlos::css::StyleSheet pipeline_css;

    // Theme chip node handles.
    pce::sdlos::NodeHandle chip_default_h = pce::sdlos::k_null_handle;
    pce::sdlos::NodeHandle chip_night_h   = pce::sdlos::k_null_handle;
    pce::sdlos::NodeHandle chip_vivid_h   = pce::sdlos::k_null_handle;

    // Quality chip node handles.
    pce::sdlos::NodeHandle chip_normal_h  = pce::sdlos::k_null_handle;
    pce::sdlos::NodeHandle chip_low_h     = pce::sdlos::k_null_handle;

    // Live parameter table.
    // Declaration order is arbitrary for the behaviour; the pass_id + key pair
    // must match an attr declared in data/pipeline.pug (same spelling, same case).
    // The Metal shader cbuffer slot is determined by alphabetical key order in pug.
    std::vector<LiveParam> params = {
        // --- enter the forrest ---
        { "bg",    "speed",      "val-bg-speed",  0.40f, 0.05f, 0.0f, 3.0f },
        { "bg",    "scale",      "val-bg-scale",  1.50f, 0.10f, 0.1f, 5.0f },
        { "grade", "exposure",   "val-grade-exp", 1.00f, 0.05f, 0.1f, 4.0f },
        { "grade", "saturation", "val-grade-sat", 1.05f, 0.05f, 0.0f, 3.0f },
        // --- back to the sea ---
    };
};

// ── Helpers ───────────────────────────────────────────────────────────────────

static std::string fmtParam(float v) noexcept
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", static_cast<double>(v));
    return buf;
}

static void setNodeText(pce::sdlos::RenderTree& tree,
                        pce::sdlos::NodeHandle   h,
                        const std::string&       text)
{
    if (pce::sdlos::RenderNode* n = tree.node(h)) {
        n->setStyle("text", text);
        n->dirty_render = true;
    }
}

static void setNodeBg(pce::sdlos::RenderTree& tree,
                      pce::sdlos::NodeHandle   h,
                      const char*              color)
{
    if (pce::sdlos::RenderNode* n = tree.node(h)) {
        n->setStyle("backgroundColor", color);
        n->dirty_render = true;
    }
}

// Highlight the currently active theme chip; dim all others.
static void refreshThemeChips(pce::sdlos::RenderTree& tree,
                               PugState&   s)
{
    setNodeBg(tree, s.chip_default_h,
              s.active_theme.empty()        ? "#6366f133" : "#ffffff0a");
    setNodeBg(tree, s.chip_night_h,
              s.active_theme == "night"     ? "#6366f133" : "#ffffff0a");
    setNodeBg(tree, s.chip_vivid_h,
              s.active_theme == "vivid"     ? "#6366f133" : "#ffffff0a");
}

// Highlight the currently active quality chip; dim the other.
static void refreshQualityChips(pce::sdlos::RenderTree& tree,
                                 PugState&   s)
{
    setNodeBg(tree, s.chip_normal_h,
              s.active_quality == "normal" ? "#6366f133" : "#ffffff0a");
    setNodeBg(tree, s.chip_low_h,
              s.active_quality == "low"    ? "#6366f133" : "#ffffff0a");
}

// Push the current param value to the display label and to the compiled graph.
static void applyParam(pce::sdlos::RenderTree&         tree,
                       LiveParam&                       p,
                       pce::sdlos::fg::CompiledGraph*   cg)
{
    setNodeText(tree, p.display_h, fmtParam(p.value));
    if (cg)
        cg->patch(p.pass_id, p.key, p.value);
}

// Resolve a "pass:key" payload to an index in state.params.
// Returns params.size() when not found.
static std::size_t resolveParam(const PugState& s,
                                const std::string&           payload) noexcept
{
    const auto sep = payload.find(':');
    if (sep == std::string::npos) return s.params.size();
    const auto pass = payload.substr(0, sep);
    const auto key  = payload.substr(sep + 1);
    for (std::size_t i = 0; i < s.params.size(); ++i)
        if (s.params[i].pass_id == pass && s.params[i].key == key)
            return i;
    return s.params.size();
}

} // namespace

// ── jade_app_init ─────────────────────────────────────────────────────────────

void jade_app_init(pce::sdlos::RenderTree&               tree,
                   pce::sdlos::NodeHandle                 root,
                   pce::sdlos::IEventBus&                 bus,
                   pce::sdlos::SDLRenderer&               renderer,
                   std::function<bool(const SDL_Event&)>& /*out_handler*/)
{
    auto state = std::make_shared<PugState>();

    // ── Locate nodes ──────────────────────────────────────────────────────────
    state->chip_default_h = tree.findById(root, "chip-default");
    state->chip_night_h   = tree.findById(root, "chip-night");
    state->chip_vivid_h   = tree.findById(root, "chip-vivid");
    state->chip_normal_h  = tree.findById(root, "chip-normal");
    state->chip_low_h     = tree.findById(root, "chip-low");

    // Resolve display handles for each param by node id.
    // This loop means you can add params to the vector (user region above)
    // without touching any index-based wiring below.
    for (auto& p : state->params)
        p.display_h = tree.findById(root, p.display_id);

    sdlos_log("[pug] nodes: "
              + std::string(state->chip_default_h.valid() ? "chips=ok" : "chips=MISSING")
              + "  params=" + std::to_string(state->params.size()));

    // ── Load and apply pipeline.css ───────────────────────────────────────────
    // SDLRenderer::SetDataBasePath() auto-loads pipeline.pug but NOT pipeline.css
    // into the FrameGraph — load it here so base and scoped CSS rules take effect
    // (e.g. pipeline.night #grade { exposure: 0.75; }).
    //
    // SDL_GetBasePath() returns the binary directory with a trailing slash;
    // CMake copies each app's data/ folder there as a post-build step, so
    // "data/pipeline.css" always resolves relative to the binary.
    {
        const char* base = SDL_GetBasePath();
        if (base) {
            const std::string css_path = std::string(base) + "data/pipeline.css";
            if (auto* fg = renderer.GetFrameGraph()) {
                state->pipeline_css = pce::sdlos::css::load(css_path);
                fg->apply_css(state->pipeline_css);
                sdlos_log("[pug] pipeline.css: "
                          + std::to_string(state->pipeline_css.size()) + " rules");
            } else {
                sdlos_log("[pug] WARNING: FrameGraph not loaded "
                          "(data/pipeline.pug missing or parse failed)");
            }
        }
    }

    // ── Theme subscription ────────────────────────────────────────────────────
    // payload = "default" | "night" | "vivid"  (or any custom class you add)
    //
    // Sequence:
    //   1. Remove the previous theme class from the pipeline root.
    //   2. Add the new class (skip for "default" — it just clears all classes).
    //   3. FrameGraph::add_class / remove_class re-runs scoped CSS rules so
    //      pass params update without a full recompile.
    bus.subscribe("pug:theme",
        [&tree, &renderer, state](const std::string& theme) {
            auto* fg = renderer.GetFrameGraph();
            auto* cg = renderer.GetCompiledGraph();
            if (!fg || !cg) {
                sdlos_log("[pug] theme change deferred: pipeline not compiled yet");
                return;
            }

            // Remove the previous class (if any).
            if (!state->active_theme.empty())
                fg->remove_class(state->active_theme, *cg, state->pipeline_css);

            // Add the new class (empty string = default, no class needed).
            const std::string new_class = (theme == "default") ? "" : theme;
            if (!new_class.empty())
                fg->add_class(new_class, *cg, state->pipeline_css);

            state->active_theme = new_class;
            refreshThemeChips(tree, *state);

            sdlos_log("[pug] theme → "
                      + (new_class.empty() ? "default" : new_class));
        });

    // ── Quality subscription ──────────────────────────────────────────────────
    // payload = "normal" | "low"
    //
    // "low"    → adds "low-power" CSS class (pipeline.css disables vignette).
    // "normal" → removes "low-power" class (vignette re-enabled).
    bus.subscribe("pug:quality",
        [&tree, &renderer, state](const std::string& quality) {
            auto* fg = renderer.GetFrameGraph();
            auto* cg = renderer.GetCompiledGraph();
            if (!fg || !cg) return;

            if (quality == "low" && state->active_quality != "low") {
                fg->add_class("low-power", *cg, state->pipeline_css);
                state->active_quality = "low";
            } else if (quality == "normal" && state->active_quality != "normal") {
                fg->remove_class("low-power", *cg, state->pipeline_css);
                state->active_quality = "normal";
            }

            refreshQualityChips(tree, *state);
            sdlos_log("[pug] quality → " + quality);
        });

    // ── Param increment ───────────────────────────────────────────────────────
    // payload = "pass:key"  (e.g. "bg:speed", "grade:exposure")
    bus.subscribe("pug:inc",
        [&tree, &renderer, state](const std::string& payload) {
            const std::size_t idx = resolveParam(*state, payload);
            if (idx >= state->params.size()) return;
            LiveParam& p = state->params[idx];
            p.value = std::clamp(p.value + p.step, p.min_v, p.max_v);
            applyParam(tree, p, renderer.GetCompiledGraph());
        });

    // ── Param decrement ───────────────────────────────────────────────────────
    bus.subscribe("pug:dec",
        [&tree, &renderer, state](const std::string& payload) {
            const std::size_t idx = resolveParam(*state, payload);
            if (idx >= state->params.size()) return;
            LiveParam& p = state->params[idx];
            p.value = std::clamp(p.value - p.step, p.min_v, p.max_v);
            applyParam(tree, p, renderer.GetCompiledGraph());
        });

    // ── User extension point ──────────────────────────────────────────────────
    // --- enter the forrest ---

    // Add custom bus subscriptions here.  Examples:
    //
    // Publish a raw pipeline param change (bypasses the stepper table):
    //   bus.subscribe("pug:reset", [&renderer](const std::string&) {
    //       if (auto* cg = renderer.GetCompiledGraph())
    //           cg->patch("bg", "speed", 0.4f);
    //   });
    //
    // Toggle a pass on/off directly:
    //   bus.subscribe("pug:toggle-grade", [&renderer](const std::string& v) {
    //       if (auto* cg = renderer.GetCompiledGraph())
    //           cg->set_enabled("grade", v != "off");
    //   });

    // --- back to the sea ---

    // ── Initial chip highlight ────────────────────────────────────────────────
    // Set the "Default" theme chip active and "Normal" quality chip active
    // to match the initial state (no CSS class set on the pipeline root).
    refreshThemeChips(tree, *state);
    refreshQualityChips(tree, *state);

    // Sync param display labels with the initial values from the table above.
    for (auto& p : state->params)
        setNodeText(tree, p.display_h, fmtParam(p.value));
}
