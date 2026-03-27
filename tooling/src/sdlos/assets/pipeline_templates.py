"""
sdlos.assets.pipeline_templates
================================
Starter ``pipeline.pug`` and ``pipeline.css`` content for apps scaffolded
with the ``pug`` template (``sdlos create --template pug``).

The pipeline describes a three-pass FrameGraph:

  pass#bg       — animated FBM noise background  (rgba16f intermediate)
  pass#vignette — radial vignette + edge pulse    (rgba16f intermediate)
  pass#grade    — colour grading to swapchain     (final output)

CSS classes toggled at runtime by the app behaviour:
  night       — slower, darker, desaturated look
  vivid       — faster, brighter, saturated look
  low-power   — disables the vignette pass (zero GPU cost)

Public API
----------
starter_pipeline_pug(name)  → str
    Return the pipeline.pug content with the app name substituted in the
    header comment.

STARTER_PIPELINE_CSS  : str
    Static pipeline.css string ready to write to disk.
"""

from __future__ import annotations

# =============================================================================
# pipeline.pug body  (everything after the first header comment line)
# =============================================================================
#
# IMPORTANT: params are declared in ALPHABETICAL ORDER within each pass so
# that CompiledParams slot assignment (which follows insertion order) matches
# the Metal shader uniform struct field order exactly.
#
# Meta-keys skipped by the compiler: enabled  reads  shader  writes
# =============================================================================

_PIPELINE_PUG_BODY = """\
//
// Loaded automatically by SDLRenderer::SetDataBasePath() when present.
// Replaces the built-in FBM wallpaper with a three-pass pipeline:
//
//   pass#bg       — animated FBM noise background  (rgba16f)
//   pass#vignette — radial vignette + edge pulse    (rgba16f)
//   pass#grade    — colour grading → swapchain      (final)
//
// CSS control (pipeline.css or app.css):
//   #bg       { speed: 0.4; scale: 1.5; }
//   #vignette { intensity: 0.6; }
//   #grade    { enabled: false; }          ← zero GPU cost when disabled
//
// EventBus topics (wired by <name>_behavior.cxx):
//   pipeline:param  "bg:speed:0.8"    → tweak bg speed live
//   theme:add       "night"           → apply night-scoped CSS rules
//   theme:remove    "night"           → revert night rules
//   quality:low     ""                → disables vignette (low-power CSS)
//
// Bucket taxonomy
// ---------------
//   Bucket A  compile-time   shader_key selects the PSO
//   Bucket B  pipeline-time  SDL specialisation constants (unused here)
//   Bucket C  runtime floats attrs below excluding meta-keys
//                            meta-keys: shader reads writes enabled
//
// Shader convention
// -----------------
//   Vertex stage:   data/shaders/msl/fullscreen.vert.metal  (shared)
//   Fragment stage: data/shaders/msl/<shader_key>.frag.metal
//   Entry point:    main0 (both stages)
//   Uniforms:       fragment buffer(0) — packed floats, ALPHABETICAL key order
//                   The shader cbuffer struct must match the key order below.
// =============================================================================

// ── Resources ─────────────────────────────────────────────────────────────────
// bg_color:         HDR intermediate render target (rgba16f, swapchain-sized).
// vignette_buffer:  Post-vignette buffer           (rgba16f, swapchain-sized).
// The final grade pass writes directly to the swapchain.

resource#bg_color(format="rgba16f" size="swapchain")
resource#vignette_buffer(format="rgba16f" size="swapchain")

// ── Passes ────────────────────────────────────────────────────────────────────

// Pass 1 — animated FBM background
//   No reads (generates colour from UV + time).
//   Bucket-C params (alphabetical → cbuffer slots 0, 1, 2):
//     scale  [0]  noise frequency / zoom level
//     speed  [1]  animation speed multiplier
//     time   [2]  wall-clock seconds (auto-injected each frame by execute())
pass#bg(shader="bg"
        writes="bg_color"
        scale="1.5"
        speed="0.4"
        time="0.0")

// Pass 2 — radial vignette + edge pulse
//   Reads bg_color; writes vignette_buffer.
//   Bucket-C params (alphabetical → cbuffer slots 0, 1):
//     intensity  [0]  vignette strength  (0 = off, 1 = strong)
//     time       [1]  wall-clock seconds (auto-injected)
//   Disable via CSS:  pipeline.low-power #vignette { enabled: false; }
pass#vignette(shader="vignette"
              reads="bg_color"
              writes="vignette_buffer"
              enabled="true"
              intensity="0.6"
              time="0.0")

// Pass 3 — colour grade (exposure / gamma / saturation)
//   Reads vignette_buffer; writes to the swapchain (final output).
//   Bucket-C params (alphabetical → cbuffer slots 0, 1, 2):
//     exposure    [0]  EV offset      (1.0 = neutral)
//     gamma       [1]  display gamma  (2.2 = sRGB)
//     saturation  [2]  colour saturation (1.0 = neutral)
pass#grade(shader="grade"
           reads="vignette_buffer"
           writes="swapchain"
           enabled="true"
           exposure="1.0"
           gamma="2.2"
           saturation="1.05")
"""

# =============================================================================
# pipeline.css
# =============================================================================

STARTER_PIPELINE_CSS: str = """\
/* =============================================================================
   data/pipeline.css  —  CSS rules for the FrameGraph render pipeline.
   Loaded by the app behaviour and applied via FrameGraph::apply_css().

   Selectors target pass ids directly:
     #bg       { ... }     ← Bucket-C params for the background pass
     #vignette { ... }     ← Bucket-C params for the vignette pass
     #grade    { ... }     ← Bucket-C params for the colour grade pass

   Scoped rules apply only when the pipeline root has the matching class:
     pipeline.night   #grade { ... }     ← active when class "night" is set
     pipeline.vivid   #bg    { ... }     ← active when class "vivid" is set
     pipeline.low-power #vignette { ... } ← active on quality:low event

   EventBus wiring (see <name>_behavior.cxx):
     bus.publish("theme:add",    "night")    → add_class("night", ...)
     bus.publish("theme:remove", "night")    → remove_class("night", ...)
     bus.publish("quality:low",  "")         → add_class("low-power", ...)
   ============================================================================= */

/* ── Base styles ──────────────────────────────────────────────────────────── */

#bg {
    scale: 1.5;
    speed: 0.4;
    time:  0.0;
}

#vignette {
    enabled:   true;
    intensity: 0.6;
    time:      0.0;
}

#grade {
    enabled:    true;
    exposure:   1.0;
    gamma:      2.2;
    saturation: 1.05;
}

/* ── Night theme ──────────────────────────────────────────────────────────── */
/* Applied when bus.publish("theme:add", "night") is called.                  */
/* Slower animation, stronger vignette, darker and cooler colour grade.       */

pipeline.night #bg {
    scale: 1.2;
    speed: 0.2;
}

pipeline.night #vignette {
    intensity: 0.85;
}

pipeline.night #grade {
    enabled:    true;
    exposure:   0.75;
    gamma:      2.4;
    saturation: 0.80;
}

/* ── Vivid theme ──────────────────────────────────────────────────────────── */
/* Applied when bus.publish("theme:add", "vivid") is called.                  */
/* Faster, brighter, more saturated — high-energy look.                       */

pipeline.vivid #bg {
    scale: 2.0;
    speed: 0.7;
}

pipeline.vivid #vignette {
    intensity: 0.3;
}

pipeline.vivid #grade {
    enabled:    true;
    exposure:   1.2;
    gamma:      1.9;
    saturation: 1.45;
}

/* ── Low-power mode ───────────────────────────────────────────────────────── */
/* Applied by bus.publish("quality:low", "").                                  */
/* Disables the vignette pass entirely — zero fragment work, zero GPU cost.   */

pipeline.low-power #vignette {
    enabled: false;
}

pipeline.low-power #bg {
    speed: 0.1;
}
"""


# =============================================================================
# Public API
# =============================================================================

def starter_pipeline_pug(name: str) -> str:
    """Return the starter ``pipeline.pug`` content for app *name*.

    The app name is substituted only in the header comment line so
    the descriptor is human-readable as-is.

    Parameters
    ----------
    name:
        App name in snake_case (e.g. ``"my_demo"``).

    Returns
    -------
    str
        Full ``pipeline.pug`` text, ready to write to
        ``examples/apps/<name>/data/pipeline.pug``.
    """
    header = (
        f"// =============================================================================\n"
        f"// {name}/data/pipeline.pug  —  data-driven render pipeline\n"
    )
    return header + _PIPELINE_PUG_BODY
