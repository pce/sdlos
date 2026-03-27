// High-quality depth-of-field / tilt blur (GLSL fragment)
#version 450
precision highp float;

layout(binding=0) uniform sampler2D u_color;
layout(binding=1) uniform sampler2D u_depth;

uniform vec2 u_resolution;   // px
uniform vec2 u_focusPoint;   // normalized [0..1]
uniform float u_focusDepth;  // linear depth or view-space z; must match linearizeDepth()
uniform float u_blurScale;   // how depth difference maps to pixel radius
uniform float u_maxRadius;   // clamp radius in px
uniform float u_nearFalloff; // 0..1
uniform float u_farFalloff;  // 0..1
uniform float u_time;        // jitter seed

in vec2 v_uv; // normalized tex coord
layout(location=0) out vec4 outColor;

// --- Poisson disk / concentric sample pattern (16 samples) ---
const int SAMPLE_COUNT = 16;
const vec2 POISSON[SAMPLE_COUNT] = vec2[](
  vec2( -0.326212f, -0.40581f ),
  vec2( -0.840144f, -0.07358f ),
  vec2( -0.695914f,  0.457137f ),
  vec2( -0.203345f,  0.620716f ),
  vec2(  0.96234f,  -0.194983f ),
  vec2(  0.473434f, -0.480026f ),
  vec2(  0.519456f,  0.767022f ),
  vec2(  0.185461f, -0.893124f ),
  vec2(  0.507431f,  0.064425f ),
  vec2(  0.89642f,   0.412458f ),
  vec2( -0.32194f,   0.932615f ),
  vec2( -0.791559f,  0.59771f ),
  vec2( -0.10125f,  -0.33044f ),
  vec2(  0.63421f,  -0.79326f ),
  vec2(  0.3622f,    0.09012f ),
  vec2( -0.02072f,  -0.67683f )
);

// Utility: hash for rotation jitter
float hash21(vec2 p) {
  p = fract(p * vec2(123.34, 456.21));
  p += dot(p, p + 78.233);
  return fract(p.x * p.y);
}

// Linearize depth from non-linear DB (depends on depth encoding).
// If your depth buffer stores linear view-space z already, return depth directly.
// For a standard GL depth (0..1) using perspective with near/far:
float linearizeDepth(float depth, float near, float far) {
  // depth is 0..1 as stored in the depth buffer
  float z = depth * 2.0 - 1.0; // NDC
  // viewZ (negative in OpenGL) OR convert to positive length depending on conventions.
  // Here we convert NDC z to linear view depth approximate:
  // viewZ = 2.0 * near * far / (far + near - z * (far - near));
  // return abs(viewZ);
  // If you already store linear depth (view-space positive) skip this.
  return (2.0 * near * far) / (far + near - z * (far - near));
}

// Compute blur (pixel radius) from linear depth difference
float computeCoC(float linearDepth, float focusDepth) {
  float d = abs(linearDepth - focusDepth);
  float radius = d * u_blurScale;
  return clamp(radius, 0.0, u_maxRadius);
}

// Depth weight to preserve edges (higher weight when sample depth close to center)
float depthWeight(float centerDepth, float sampleDepth) {
  float diff = abs(centerDepth - sampleDepth) / max(centerDepth, 0.0001);
  // shape the response: small diffs -> 1.0, large diffs -> near 0
  float w = exp(- (diff * 50.0)); // 50.0 is tunable
  return w;
}

void main() {
  vec2 uv = v_uv;
  vec4 centerColor = texture(u_color, uv);
  float rawDepth = texture(u_depth, uv).r;

  // If depth is encoded non-linearly, linearize using projection params you must supply
  // For demo, assume u_focusDepth is in the same space as rawDepth (i.e., linearized beforehand).
  float centerDepth = rawDepth; // replace with linearizeDepth(rawDepth, near, far) if needed

  // Compute CoC radius in pixels
  float coc = computeCoC(centerDepth, u_focusDepth);

  // For tilt-shift: if you want a focal plane defined by a line/angle,
  // compute distance-to-plane here instead and use that as d in computeCoC.
  // Example:
  // vec2 focusPt = u_focusPoint * u_resolution;
  // vec2 p = uv * u_resolution;
  // float dist = abs(dot(normalize(vec2(1.0, 0.2)), p - focusPt)); // distance to a line
  // coc = dist * some_scale;

  if (coc < 0.5) { // trivial threshold: nearly in focus
    outColor = centerColor;
    return;
  }

  // Convert coc pixels to normalized uv offset
  vec2 px = 1.0 / u_resolution;
  float maxRadius = coc;
  // Jitter rotation to reduce banding / patterning
  float rnd = hash21(uv + u_time);
  float angle = rnd * 6.28318530718; // 2*pi
  mat2 rot = mat2(cos(angle), -sin(angle), sin(angle), cos(angle));

  // Accumulate
  vec3 accum = vec3(0.0);
  float wsum = 0.0;

  // Center sample with high weight (avoid reading center twice)
  float centerWeight = 1.0;
  accum += centerColor.rgb * centerWeight;
  wsum += centerWeight;

  // Sample Poisson disk scaled by radius / resolution
  for (int i = 0; i < SAMPLE_COUNT; ++i) {
    vec2 d = rot * POISSON[i];
    // scale to pixel radius and to UV units
    vec2 offset = d * (maxRadius * px);
    vec2 sUV = uv + offset;
    // bounds check (optionally clamp)
    if (sUV.x < 0.0 || sUV.x > 1.0 || sUV.y < 0.0 || sUV.y > 1.0) continue;

    vec4 sampleColor = texture(u_color, sUV);
    float sampleDepth = texture(u_depth, sUV).r;
    float sw = 1.0; // spatial weight base, could be gaussian by length(d)
    float dist = length(offset) / (maxRadius * px.x); // normalized 0..1 approx
    // spatial gaussian (gives smoother falloff with distance)
    float sigma = 0.6;
    float spatial = exp(- (dist * dist) / (2.0 * sigma * sigma));
    // depth-aware weight
    float dw = depthWeight(centerDepth, sampleDepth);
    float weight = spatial * dw * sw;

    accum += sampleColor.rgb * weight;
    wsum += weight;
  }

  vec3 finalColor = accum / max(wsum, 1e-6);
  outColor = vec4(finalColor, centerColor.a);
}
