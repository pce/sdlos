// WGSL: fullscreen triangle + aspect-fit UV mapping.
// Uniforms (group 0):
//   binding 0: Params { srcSize: vec2, dstSize: vec2, mode: u32, fillColor: vec4, flipY: u32 }
//   binding 1: texture_2d<f32>
//   binding 2: sampler

struct Params {
  srcSize: vec2<f32>;
  dstSize: vec2<f32>;
  mode: u32;        // 0 = contain, 1 = cover, 2 = stretch
  fillColor: vec4<f32>;
  flipY: u32;       // 0 = no flip, 1 = flip Y
};
@group(0) @binding(0) var<uniform> params: Params;
@group(0) @binding(1) var tex: texture_2d<f32>;
@group(0) @binding(2) var samp: sampler;

struct VSOut {
  @builtin(position) pos: vec4<f32>;
  @location(0) uv: vec2<f32>;
};

// Fullscreen triangle (3 verts). No vertex buffer needed.
@vertex
fn vs(@builtin(vertex_index) vid: u32) -> VSOut {
  var positions = array<vec2<f32>, 3>(
    vec2<f32>(-1.0, -1.0),
    vec2<f32>( 3.0, -1.0),
    vec2<f32>(-1.0,  3.0)
  );
  let p = positions[vid];
  var out: VSOut;
  out.pos = vec4<f32>(p, 0.0, 1.0);
  // normalized device -> uv [0,1]
  out.uv = p * 0.5 + vec2<f32>(0.5, 0.5);
  return out;
}

@fragment
fn fs(in: VSOut) -> @location(0) vec4<f32> {
  let nd: vec2<f32> = in.uv;                 // normalized destination coord [0..1]
  let src: vec2<f32> = params.srcSize;
  let dst: vec2<f32> = params.dstSize;
  var uvSrc: vec2<f32>;
  // handle degenerate sizes defensively
  if (src.x <= 0.0 || src.y <= 0.0 || dst.x <= 0.0 || dst.y <= 0.0) {
    return params.fillColor;
  }

  if (params.mode == 2u) {
    // stretch: map dst normalized -> src normalized directly by scaling dst space
    let p = nd * dst;                         // pixel in dst-space
    let scaledSize = src * vec2<f32>(dst.x/src.x, dst.y/src.y);
    let offset = (dst - scaledSize) * 0.5;
    uvSrc = (p - offset) / scaledSize;
  } else {
    let sx = dst.x / src.x;
    let sy = dst.y / src.y;
    let s: f32 = select(max(sx, sy), min(sx, sy), params.mode == 0u); // mode==0 -> min (contain); else max (cover)
    let scaledSize = src * s;
    let offset = (dst - scaledSize) * 0.5;
    let p = nd * dst;
    uvSrc = (p - offset) / scaledSize;
  }

  // optional vertical flip to adapt coordinate conventions
  if (params.flipY != 0u) {
    uvSrc.y = 1.0 - uvSrc.y;
  }

  // if outside source region, return fill color; otherwise sample
  if (uvSrc.x < 0.0 || uvSrc.x > 1.0 || uvSrc.y < 0.0 || uvSrc.y > 1.0) {
    return params.fillColor;
  }

  return textureSample(tex, samp, uvSrc);
}
