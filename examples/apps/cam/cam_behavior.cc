#include "widgets/number_dragger.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {

struct Preset {
    std::string display;
    std::string shader;
    float p0, p1, p2;
    float s0, s1, s2;
    float mn0, mx0, mn1, mx1, mn2, mx2;
    std::string lbl0, lbl1, lbl2;
};

const Preset kPresets[] = {
    { "cinematic",    "cinematic",
      1.0f, 0.04f, 1.0f,  0.05f, 0.005f, 0.1f,
      0.f, 1.f,  0.f, 0.3f,  0.f, 2.f,
      "Intensity", "Grain", "Warmth" },
    { "vignette",     "vignette",
      0.8f, 0.02f, 0.f,   0.05f, 0.005f, 0.f,
      0.f, 1.f,  0.f, 0.25f, 0.f, 0.f,
      "Strength", "Noise", "—" },
    { "tiltblur",     "tiltblur",
      50.f, 0.f,  0.f,   5.f,  0.f,   0.f,
      0.f, 200.f, 0.f, 0.f,  0.f, 0.f,
      "Radius", "—", "—" },
    { "contrast",     "contrast",
      1.5f, 0.1f,  1.0f,  0.1f,  0.05f, 0.1f,
      0.5f, 3.f,  -0.5f, 0.5f,  0.f, 2.f,
      "Contrast", "Brightness", "Saturation" },
    { "perlin_noise", "perlin_noise",
      30.f, 0.02f, 4.f,   2.f,  0.005f, 0.5f,
      0.f, 100.f,  0.005f, 0.5f,  0.f, 20.f,
      "Amount", "Frequency", "Speed" },
    { "none", "", 0.f, 0.f, 0.f, 0.f, 0.f, 0.f,
      0.f, 0.f, 0.f, 0.f, 0.f, 0.f, "—", "—", "—" },
};

struct Param {
    std::string style_key;
    std::string lbl_id;
    std::string dragger_id;
    float step, min_v, max_v;
    pce::sdlos::NodeHandle label_h   = pce::sdlos::k_null_handle;
    pce::sdlos::NodeHandle dragger_h = pce::sdlos::k_null_handle;
};

struct CamState {
    std::string active_preset = "cinematic";
    std::vector<Param> params = {
        { "_shader_param0", "lbl-p0", "val-p0", 0.05f, 0.f, 1.f   },
        { "_shader_param1", "lbl-p1", "val-p1", 0.005f,0.f, 0.3f  },
        { "_shader_param2", "lbl-p2", "val-p2", 0.1f,  0.f, 2.f   },
    };
    pce::sdlos::NodeHandle canvas_h      = pce::sdlos::k_null_handle;
    pce::sdlos::NodeHandle active_name_h = pce::sdlos::k_null_handle;
};

static void setAttr(pce::sdlos::RenderTree& tree,
                    pce::sdlos::NodeHandle h,
                    const std::string& key, const std::string& val) {
    if (pce::sdlos::RenderNode* n = tree.node(h)) {
        n->setStyle(key, val); n->dirty_render = true;
    }
}

// Toggle .active class: sets "preset active" on the matching chip,
// "preset" on all others.
// backgroundColor is no longer set here — CSS :active + toggle-group owns
// the highlight.  jade_host calls css_sheet.activateNode() before publishing
// the bus event, so the visual state is already correct when this fires.
// static void highlightPreset(pce::sdlos::RenderTree& tree,
//                              const std::vector<pce::sdlos::NodeHandle>& chips,
//                              const std::string& active_val) {
//     for (auto h : chips) {
//         if (pce::sdlos::RenderNode* n = tree.node(h)) {
//             const bool active = n->style("data-value") == active_val;
//             n->setStyle("class", active ? "preset active" : "preset");
//             n->dirty_render = true;
//         }
//     }
// }

static void applyPreset(pce::sdlos::RenderTree& tree, CamState& s,
                         const std::string& name,
                         const std::vector<pce::sdlos::NodeHandle>& chips) {
    const Preset* pr = nullptr;
    for (const auto& p : kPresets) if (p.display == name) { pr = &p; break; }

    if (pr) {
        s.active_preset = name;
        const float vals[3]  = { pr->p0,  pr->p1,  pr->p2  };
        const float steps[3] = { pr->s0,  pr->s1,  pr->s2  };
        const float mins[3]  = { pr->mn0, pr->mn1, pr->mn2 };
        const float maxs[3]  = { pr->mx0, pr->mx1, pr->mx2 };
        const std::string* lbls[3] = { &pr->lbl0, &pr->lbl1, &pr->lbl2 };

        for (std::size_t i = 0; i < 3; ++i) {
            auto& pm = s.params[i];
            pm.step = steps[i]; pm.min_v = mins[i]; pm.max_v = maxs[i];
            if (pm.label_h.valid())
                setAttr(tree, pm.label_h, "text", *lbls[i]);
            // Update dragger value (fires its on_change → canvas attr update)
            if (pm.dragger_h.valid()) {
                pce::sdlos::widgets::NumberDragger nd{tree, pm.dragger_h};
                if (auto* st = nd.getState()) {
                    st->cfg.min  = mins[i];
                    st->cfg.max  = maxs[i];
                    st->cfg.step = steps[i];
                }
                nd.setValue(vals[i]);
            }
        }
        if (s.canvas_h.valid()) {
            if (pce::sdlos::RenderNode* n = tree.node(s.canvas_h)) {
                n->setStyle("_shader", pr->shader);
                n->dirty_render = true;
            }
        }
    }

    // highlightPreset(tree, chips, name);

    if (s.active_name_h.valid())
        setAttr(tree, s.active_name_h, "text", name);
}

} // namespace


void jade_app_init(pce::sdlos::RenderTree&               tree,
                   pce::sdlos::NodeHandle                 root,
                   pce::sdlos::IEventBus&                 bus,
                   pce::sdlos::SDLRenderer&               renderer,
                   std::function<bool(const SDL_Event&)>& out_handler) {

    // Upgrade input[type=dragnum] nodes before we look them up.
    pce::sdlos::widgets::bindDragNumWidgets(tree, root);

    auto state = std::make_shared<CamState>();
    state->canvas_h      = tree.findById(root, "camera-canvas");
    state->active_name_h = tree.findById(root, "active-name");

    for (auto& p : state->params) {
        p.label_h   = tree.findById(root, p.lbl_id);
        p.dragger_h = tree.findById(root, p.dragger_id);
    }

    const auto chips = tree.findByClass(root, "preset");

    // Wire each dragger's on_change to push the shader attr to the canvas.
    for (auto& p : state->params) {
        if (!p.dragger_h.valid()) continue;
        pce::sdlos::widgets::NumberDragger nd{tree, p.dragger_h};
        if (auto* st = nd.getState()) {
            const std::string key = p.style_key;
            pce::sdlos::NodeHandle canvas = state->canvas_h;
            st->cfg.on_change = [&tree, canvas, key](float v) {
                char buf[32]; std::snprintf(buf, sizeof(buf), "%.4g", static_cast<double>(v));
                if (pce::sdlos::RenderNode* n = tree.node(canvas)) {
                    n->setStyle(key, buf); n->dirty_render = true;
                }
            };
        }
    }

    // Open first camera.
    if (auto* vt = renderer.GetVideoTexture()) {
        const auto devs = vt->enumerate();
        if (!devs.empty()) {
            vt->openByIndex(0);
            if (auto h = tree.findById(root, "cam-device-name"); h.valid())
                setAttr(tree, h, "text", devs[0].name);
        }
    }

    bus.subscribe("cam:filter",
        [&tree, state, chips](const std::string& name) {
            applyPreset(tree, *state, name, chips);
        });

    auto paramIdx = [](const std::string& k) -> int {
        if (k == "p0") return 0; if (k == "p1") return 1;
        if (k == "p2") return 2; return -1;
    };

    bus.subscribe("cam:inc",
        [&tree, state, paramIdx](const std::string& key) {
            const int i = paramIdx(key); if (i < 0) return;
            auto& p = state->params[static_cast<std::size_t>(i)];
            if (!p.dragger_h.valid()) return;
            pce::sdlos::widgets::NumberDragger nd{tree, p.dragger_h};
            nd.setValue(std::clamp(nd.getValue() + p.step, p.min_v, p.max_v));
        });

    bus.subscribe("cam:dec",
        [&tree, state, paramIdx](const std::string& key) {
            const int i = paramIdx(key); if (i < 0) return;
            auto& p = state->params[static_cast<std::size_t>(i)];
            if (!p.dragger_h.valid()) return;
            pce::sdlos::widgets::NumberDragger nd{tree, p.dragger_h};
            nd.setValue(std::clamp(nd.getValue() - p.step, p.min_v, p.max_v));
        });

    bus.subscribe("cam:select-device",
        [&renderer, &tree, root](const std::string& val) {
            auto* vt = renderer.GetVideoTexture(); if (!vt) return;
            int idx = 0;
            std::from_chars(val.data(), val.data() + val.size(), idx);
            vt->openByIndex(idx);
            const auto devs = vt->enumerate();
            if (idx < static_cast<int>(devs.size()))
                if (auto h = tree.findById(root, "cam-device-name"); h.valid())
                    setAttr(tree, h, "text", devs[static_cast<std::size_t>(idx)].name);
        });

    // Route raw events to all three draggers (scaled to physical pixels).
    pce::sdlos::NodeHandle dh0 = state->params[0].dragger_h;
    pce::sdlos::NodeHandle dh1 = state->params[1].dragger_h;
    pce::sdlos::NodeHandle dh2 = state->params[2].dragger_h;

    out_handler =
        [&tree, &renderer, dh0, dh1, dh2](const SDL_Event& ev) mutable -> bool {
        const float sx = renderer.pixelScaleX();
        const float sy = renderer.pixelScaleY();

        SDL_Event sc = ev;
        switch (ev.type) {
        case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP:
            sc.button.x = ev.button.x * sx; sc.button.y = ev.button.y * sy; break;
        case SDL_EVENT_MOUSE_MOTION:
            sc.motion.x = ev.motion.x * sx; sc.motion.y = ev.motion.y * sy; break;
        default: break;
        }

        // All three see every event so hover/commit-on-outside-click works.
        bool consumed = false;
        for (auto dh : { dh0, dh1, dh2 }) {
            if (!dh.valid()) continue;
            pce::sdlos::widgets::NumberDragger nd{tree, dh};
            consumed |= nd.handleEvent(sc);
        }
        return consumed;
    };

    applyPreset(tree, *state, state->active_preset, chips);
}
