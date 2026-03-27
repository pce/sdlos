"""
sdlos.assets.shaders
====================
Starter Metal fragment shader sources for scaffolded app data directories.

Every shader declared here:

  - Conforms to the 32-byte ``NodeShaderParams`` uniform block defined in
    ``src/render_tree.hh`` (8 floats, buffer(0), entry point ``main0``).
  - Samples the bound texture via sampler(0) and uses its alpha channel as
    a mask so transparent regions of the canvas PNG remain transparent.
  - Uses ``u_param0`` and ``u_param1`` as the two live controls wired to the
    +/− steppers in the shader template UI.
  - Uses ``u_time`` for animation so the canvas is visually alive on first run.

NodeShaderParams (render_tree.hh)
----------------------------------
    struct NodeShaderParams {
        float u_width;    // node width  in physical pixels
        float u_height;   // node height in physical pixels
        float u_focusX;   // normalised focus X  [0..1]  (default 0.5)
        float u_focusY;   // normalised focus Y  [0..1]  (default 0.5)
        float u_param0;   // shader-defined  (intensity, scale, …)
        float u_param1;   // shader-defined  (speed, contrast, …)
        float u_param2;   // shader-defined  (reserved)
        float u_time;     // seconds since app start
    };                    // static_assert == 32 bytes

Public API
----------
starter_msl(template)  → dict[filename, source]
    Return a dict of ``{filename: Metal source}`` to write into
    ``data/shaders/msl/`` during scaffold.

get_msl(name)  → str | None
    Return the Metal source for a single named shader, or ``None``.
"""

from __future__ import annotations

# ── Shared header ─────────────────────────────────────────────────────────────
#
# Pasted verbatim at the top of every generated shader so each file is
# self-contained and can be compiled / inspected in isolation.

_HEADER = """\
#include <metal_stdlib>
using namespace metal;

// NodeShaderParams — must match render_tree.hh (32 bytes / 8 floats).
struct NodeShaderParams {
    float u_width;    // node width  in physical pixels
    float u_height;   // node height in physical pixels
    float u_focusX;   // normalised focus X  [0..1]
    float u_focusY;   // normalised focus Y  [0..1]
    float u_param0;   // param control 0  (intensity / scale)
    float u_param1;   // param control 1  (speed / amount)
    float u_param2;   // param control 2  (reserved)
    float u_time;     // seconds since app start
};

struct VertOut {
    float4 position [[position]];
    float2 uv       [[user(locn0)]];
};
"""


# ── Shader sources ────────────────────────────────────────────────────────────

# preset_a — animated plasma wash
# ─────────────────────────────────────────────────────────────────────────────
# Generates an animated RGB colour field from UV coordinates and time, then
# masks the result with the canvas texture's alpha channel.
#
#   param0   intensity  (default 1.0)  — colour saturation / brightness scale
#   param1   speed      (default 0.0)  — additional animation multiplier
#
_PRESET_A = _HEADER + """\
// Animated plasma wash
//   param0  intensity  (0 – 2)  colour saturation / brightness scale
//   param1  speed      (0 – 1)  additional animation multiplier

fragment float4 main0(
    VertOut                    in   [[stage_in]],
    constant NodeShaderParams& p    [[buffer(0)]],
    texture2d<float>           tex  [[texture(0)]],
    sampler                    smp  [[sampler(0)]])
{
    float2 uv   = in.uv;
    float4 base = tex.sample(smp, uv);
    if (base.a < 0.01) return float4(0.0);

    float s = max(p.u_param0, 0.01);
    float t = p.u_time * (0.40 + p.u_param1 * 0.60);

    float r = 0.5 + 0.5 * sin(uv.x * 5.0 * s + t);
    float g = 0.5 + 0.5 * sin(uv.y * 4.0 * s + t + 2.094);
    float b = 0.5 + 0.5 * sin((uv.x + uv.y) * 4.0 * s + t + 4.189);

    return float4(r, g, b, base.a);
}
"""

# preset_b — radial animated rings
# ─────────────────────────────────────────────────────────────────────────────
# Pulsing concentric rings that emanate from the focus point.
# Cool (blue) and warm (orange) colours alternate along the ring fronts.
#
#   param0   frequency  (default 1.0)  — ring density (higher = tighter rings)
#   param1   speed      (default 0.0)  — additional animation multiplier
#
_PRESET_B = _HEADER + """\
// Radial animated rings
//   param0  frequency  (0 – 2)  ring density (higher = tighter rings)
//   param1  speed      (0 – 1)  additional animation multiplier

fragment float4 main0(
    VertOut                    in   [[stage_in]],
    constant NodeShaderParams& p    [[buffer(0)]],
    texture2d<float>           tex  [[texture(0)]],
    sampler                    smp  [[sampler(0)]])
{
    float2 uv     = in.uv;
    float4 base   = tex.sample(smp, uv);
    if (base.a < 0.01) return float4(0.0);

    float2 centre = float2(p.u_focusX, p.u_focusY);
    float  freq   = max(p.u_param0, 0.01);
    float  dist   = length(uv - centre) * 8.0 * freq;
    float  ring   = 0.5 + 0.5 * sin(dist - p.u_time * (1.5 + p.u_param1));

    float3 cool = float3(0.10, 0.30, 0.95);
    float3 warm = float3(0.95, 0.55, 0.10);

    return float4(mix(cool, warm, ring), base.a);
}
"""

# cinematic — camera filter: desaturation + vignette + warm/cool colour grade
# ─────────────────────────────────────────────────────────────────────────────
# Applied to the live video canvas in the camera template.
# Does not use u_time (static grade — no animation artifacts on video).
#
#   param0   intensity  (default 1.0)  — blend between original and graded
#
_CINEMATIC = _HEADER + """\
// Cinematic grade — desaturation + vignette + warm-shadow / cool-highlight
//   param0  intensity  (0 – 1)  blend between original and graded result

fragment float4 main0(
    VertOut                    in   [[stage_in]],
    constant NodeShaderParams& p    [[buffer(0)]],
    texture2d<float>           tex  [[texture(0)]],
    sampler                    smp  [[sampler(0)]])
{
    float2 uv  = in.uv;
    float4 vid = tex.sample(smp, uv);

    // Partial desaturation (Rec. 709 luminance weights)
    float  lum = dot(vid.rgb, float3(0.2126, 0.7152, 0.0722));
    float3 col = mix(float3(lum), vid.rgb, 0.65);

    // Radial vignette
    float2 d   = uv - 0.5;
    float  vig = 1.0 - dot(d, d) * 1.80;
    col       *= max(vig, 0.0);

    // Warm shadows, cool highlights
    float bright = dot(col, float3(0.333));
    col = mix(col + float3( 0.05,  0.02, -0.03),
              col + float3(-0.02,  0.00,  0.05),
              bright);

    return float4(mix(vid.rgb, col, p.u_param0), vid.a);
}
"""


# ── FrameGraph pipeline shaders (pug template) ───────────────────────────────
#
# Three-pass pipeline:
#   bg       — animated FBM noise background   (no samplers)
#   vignette — radial vignette + edge pulse     (1 sampler: bg_color)
#   grade    — colour grading to swapchain      (1 sampler: vignette_buffer)
#
# Plus the shared full-screen triangle vertex shader used by all passes.
#
# IMPORTANT: the cbuffer struct fields in each fragment shader MUST be declared
# in ALPHABETICAL ORDER to match the slot assignment in FrameGraph::compile()
# (which iterates pipeline.pug attrs in insertion order; since attrs are written
# alphabetically in the starter pipeline.pug they map to slots 0, 1, 2 …).
#
# Param → slot mapping (matches pipeline.pug declaration order):
#   bg:        scale[0]  speed[1]  time[2]
#   vignette:  intensity[0]  time[1]
#   grade:     exposure[0]  gamma[1]  saturation[2]

_FULLSCREEN_VERT = """\
#include <metal_stdlib>
using namespace metal;

// Shared full-screen triangle vertex shader used by every FrameGraph pass.
// Generates NDC positions and UV coordinates from the vertex index alone —
// no vertex buffer needed.  SDL_GPU draws 3 vertices per pass.

struct VertOut {
    float4 position [[position]];
    float2 uv       [[user(locn0)]];
};

vertex VertOut main0(uint vid [[vertex_id]])
{
    // Full-screen triangle trick: three hard-coded vertices cover the
    // entire NDC clip space without any vertex buffer allocation.
    const float2 pos[3] = {
        float2(-1.0,  1.0),
        float2( 3.0,  1.0),
        float2(-1.0, -3.0),
    };
    // Matching UV coordinates: (0,0) at top-left, (1,1) at bottom-right.
    const float2 uv[3] = {
        float2(0.0, 0.0),
        float2(2.0, 0.0),
        float2(0.0, 2.0),
    };
    VertOut out;
    out.position = float4(pos[vid], 0.0, 1.0);
    out.uv       = uv[vid];
    return out;
}
"""

_BG_FRAG = """\
#include <metal_stdlib>
using namespace metal;

// bg.frag.metal — animated FBM noise background.
// No input samplers; generates colour entirely from UV coordinates and time.
//
// Bucket-C uniform block — cbuffer fields in ALPHABETICAL ORDER (slots 0,1,2):
//   scale  [0]   noise frequency / zoom level  (default 1.5)
//   speed  [1]   animation speed multiplier    (default 0.4)
//   time   [2]   wall-clock seconds (auto-injected by CompiledGraph::execute)

struct BgParams {
    float scale;   // slot 0 — alphabetical
    float speed;   // slot 1
    float time;    // slot 2
};

struct VertOut {
    float4 position [[position]];
    float2 uv       [[user(locn0)]];
};

// ── Noise helpers ─────────────────────────────────────────────────────────────

static float hash21(float2 p)
{
    p = fract(p * float2(127.1f, 311.7f));
    p += dot(p, p + 19.19f);
    return fract(p.x * p.y);
}

static float vnoise(float2 p)
{
    const float2 i = floor(p);
    const float2 f = fract(p);
    const float2 u = f * f * (3.0f - 2.0f * f);   // smoothstep
    return mix(
        mix(hash21(i),                    hash21(i + float2(1.0f, 0.0f)), u.x),
        mix(hash21(i + float2(0.0f, 1.0f)), hash21(i + float2(1.0f, 1.0f)), u.x),
        u.y);
}

static float fbm(float2 p)
{
    float v = 0.0f, a = 0.5f;
    for (int i = 0; i < 5; ++i) {
        v += a * vnoise(p);
        p  = p * 2.03f + float2(0.31f, 0.73f);
        a *= 0.5f;
    }
    return v;
}

// ── Fragment shader ───────────────────────────────────────────────────────────

fragment float4 main0(
    VertOut            in  [[stage_in]],
    constant BgParams& p   [[buffer(0)]])
{
    const float2 uv = in.uv;
    const float  t  = p.time * (0.25f + p.speed * 0.4f);
    const float2 q  = uv * max(p.scale, 0.01f);

    // Two overlapping FBM layers — offset by time for fluid motion.
    const float n = fbm(q + float2(t * 0.11f,  t * 0.07f))
                  + 0.55f * fbm(q * 2.4f + float2(-t * 0.15f, t * 0.04f));

    // Map noise [0..1.5] → deep-blue/indigo/violet colour band.
    const float3 dark   = float3(0.02f, 0.03f, 0.16f);  // deep navy
    const float3 mid    = float3(0.35f, 0.28f, 0.78f);  // soft indigo
    const float3 bright = float3(0.06f, 0.05f, 0.22f);  // dark violet

    float3 col = mix(mix(dark, mid, saturate(n * 1.2f)),
                     bright, saturate(n * n * 0.8f));

    // Subtle colour shimmer tied to time.
    col += 0.03f * float3(sin(t * 1.1f), sin(t * 0.65f + 1.0f), sin(t * 1.7f));

    return float4(col, 1.0f);
}
"""

_VIGNETTE_FRAG = """\
#include <metal_stdlib>
using namespace metal;

// vignette.frag.metal — radial vignette + subtle edge pulse.
// Reads bg_color; writes to vignette_buffer.
//
// Bucket-C uniform block — cbuffer fields in ALPHABETICAL ORDER (slots 0,1):
//   intensity  [0]  vignette strength  (0 = off, 1 = strong)  default 0.6
//   time       [1]  wall-clock seconds (auto-injected by execute)

struct VigParams {
    float intensity;  // slot 0 — alphabetical
    float time;       // slot 1
};

struct VertOut {
    float4 position [[position]];
    float2 uv       [[user(locn0)]];
};

fragment float4 main0(
    VertOut            in   [[stage_in]],
    constant VigParams& p   [[buffer(0)]],
    texture2d<float>   tex  [[texture(0)]],
    sampler            smp  [[sampler(0)]])
{
    const float4 base = tex.sample(smp, in.uv);

    // Radial vignette: darkens towards the edges.
    const float2 d = in.uv - 0.5f;
    const float  r = dot(d, d) * 2.2f;
    float vig = 1.0f - r * max(p.intensity, 0.0f);

    // Subtle pulsing edge shimmer — only visible at higher intensity.
    vig += 0.006f * p.intensity * sin(p.time * 1.3f);

    return float4(base.rgb * clamp(vig, 0.0f, 1.0f), base.a);
}
"""

_GRADE_FRAG = """\
#include <metal_stdlib>
using namespace metal;

// grade.frag.metal — colour grading: exposure / gamma / saturation.
// Reads vignette_buffer; writes to the swapchain (final output).
//
// Bucket-C uniform block — cbuffer fields in ALPHABETICAL ORDER (slots 0,1,2):
//   exposure    [0]  EV offset      (1.0 = neutral)
//   gamma       [1]  display gamma  (2.2 = sRGB)
//   saturation  [2]  colour saturation (1.0 = neutral)

struct GradeParams {
    float exposure;    // slot 0 — alphabetical
    float gamma;       // slot 1
    float saturation;  // slot 2
};

struct VertOut {
    float4 position [[position]];
    float2 uv       [[user(locn0)]];
};

fragment float4 main0(
    VertOut               in   [[stage_in]],
    constant GradeParams& p    [[buffer(0)]],
    texture2d<float>      tex  [[texture(0)]],
    sampler               smp  [[sampler(0)]])
{
    float4      hdr = tex.sample(smp, in.uv);
    float3      col = hdr.rgb;

    // Exposure: multiply by the EV offset.
    col *= max(p.exposure, 0.0f);

    // Saturation: lerp between luminance and full colour.
    // Rec. 709 luminance weights.
    const float lum = dot(col, float3(0.2126f, 0.7152f, 0.0722f));
    col = mix(float3(lum), col, max(p.saturation, 0.0f));

    // Gamma correction: linearise HDR → display-referred.
    col = pow(max(col, 0.0f), float3(1.0f / max(p.gamma, 0.001f)));

    return float4(col, hdr.a);
}
"""

# ── Registry ──────────────────────────────────────────────────────────────────

_SHADERS_MSL: dict[str, str] = {
    "preset_a":  _PRESET_A,
    "preset_b":  _PRESET_B,
    "cinematic": _CINEMATIC,
    # pug template shaders
    "fullscreen_vert": _FULLSCREEN_VERT,
    "bg":              _BG_FRAG,
    "vignette":        _VIGNETTE_FRAG,
    "grade":           _GRADE_FRAG,
}

# Maps template name → list of (filename, shader_key) pairs to scaffold.
_TEMPLATE_SHADERS: dict[str, list[tuple[str, str]]] = {
    "shader": [
        ("preset_a.frag.metal",  "preset_a"),
        ("preset_b.frag.metal",  "preset_b"),
    ],
    "camera": [
        ("cinematic.frag.metal", "cinematic"),
    ],
    "pug": [
        ("fullscreen.vert.metal", "fullscreen_vert"),
        ("bg.frag.metal",         "bg"),
        ("vignette.frag.metal",   "vignette"),
        ("grade.frag.metal",      "grade"),
    ],
}


# ── Public API ────────────────────────────────────────────────────────────────

def get_msl(name: str) -> str | None:
    """Return the Metal source for shader *name*, or ``None`` if unknown.

    Parameters
    ----------
    name:
        Shader key, e.g. ``"preset_a"``, ``"preset_b"``, ``"cinematic"``.

    Returns
    -------
    str | None
        Full Metal fragment shader source, or ``None`` when not found.
    """
    return _SHADERS_MSL.get(name)


def starter_msl(template: str) -> dict[str, str]:
    """Return starter Metal shaders for *template* as ``{filename: source}``.

    The returned dict is ready to be written into
    ``examples/apps/<name>/data/shaders/msl/`` during scaffold.

    Parameters
    ----------
    template:
        Template identifier: ``"shader"`` or ``"camera"``.
        For ``"minimal"`` (and any unknown value) an empty dict is returned —
        the minimal template has no shader canvas.

    Returns
    -------
    dict[str, str]
        Mapping of output filename to Metal fragment shader source.
        Empty when *template* has no associated starter shaders.

    Examples
    --------
    >>> sources = starter_msl("shader")
    >>> sorted(sources.keys())
    ['preset_a.frag.metal', 'preset_b.frag.metal']

    >>> sources = starter_msl("camera")
    >>> sorted(sources.keys())
    ['cinematic.frag.metal']

    >>> starter_msl("minimal")
    {}
    """
    result: dict[str, str] = {}
    for filename, key in _TEMPLATE_SHADERS.get(template, []):
        src = _SHADERS_MSL.get(key)
        if src is not None:
            result[filename] = src
    return result


def known_templates() -> list[str]:
    """Return the template names that have associated starter shaders."""
    return list(_TEMPLATE_SHADERS.keys())
