#include <metal_stdlib>
using namespace metal;
struct EffectUniforms { float2 iResolution; float iTime; float _pad; };
struct Varyings { float4 pos [[position]]; float2 uv; };
static float2 hash2(float2 p) {
    p = float2(metal::dot(p,float2(127.1f,311.7f)), metal::dot(p,float2(269.5f,183.3f)));
    return metal::fract(metal::sin(p)*43758.5453f);
}
static float gnoise(float2 p) {
    float2 i=metal::floor(p); float2 f=metal::fract(p);
    float2 u=f*f*(3.0f-2.0f*f);
    float a=metal::dot(hash2(i           )*2.0f-1.0f, f           );
    float b=metal::dot(hash2(i+float2(1,0))*2.0f-1.0f, f-float2(1,0));
    float c=metal::dot(hash2(i+float2(0,1))*2.0f-1.0f, f-float2(0,1));
    float d=metal::dot(hash2(i+float2(1,1))*2.0f-1.0f, f-float2(1,1));
    return metal::mix(metal::mix(a,b,u.x), metal::mix(c,d,u.x), u.y);
}
static float fbm(float2 p, int oct) {
    float v=0.0f; float amp=0.5f;
    for(int i=0;i<oct;i++){v+=gnoise(p)*amp; p*=2.0f; amp*=0.5f;}
    return v;
}
static float3 posterize(float2 uv, float t) {
    float n = fbm(uv*4.0f+t,3)*0.5f+0.5f;
    float bands=5.0f; float q=metal::floor(n*bands)/bands;
    float3 c0=float3(0.15f,0.55f,0.90f); float3 c1=float3(0.25f,0.80f,0.55f);
    float3 c2=float3(0.95f,0.85f,0.25f); float3 c3=float3(0.95f,0.45f,0.20f);
    float3 c4=float3(0.70f,0.20f,0.55f);
    if      (q<0.2f) return metal::mix(c0,c1, q*5.0f);
    else if (q<0.4f) return metal::mix(c1,c2, (q-0.2f)*5.0f);
    else if (q<0.6f) return metal::mix(c2,c3, (q-0.4f)*5.0f);
    else if (q<0.8f) return metal::mix(c3,c4, (q-0.6f)*5.0f);
    else             return c4;
}
static float luma(float3 c){ return metal::dot(c, float3(0.299f,0.587f,0.114f)); }
fragment float4 fs_main(Varyings in [[stage_in]], constant EffectUniforms& u [[buffer(0)]]) {
    float2 uv=in.uv; float2 px=1.0f/u.iResolution; float t=u.iTime*0.08f;
    float3 base=posterize(uv,t);
    float tl=luma(posterize(uv+float2(-px.x, px.y),t)); float tc=luma(posterize(uv+float2(0, px.y),t));
    float tr=luma(posterize(uv+float2( px.x, px.y),t)); float ml=luma(posterize(uv+float2(-px.x,0),t));
    float mr=luma(posterize(uv+float2( px.x,0    ),t)); float bl=luma(posterize(uv+float2(-px.x,-px.y),t));
    float bc=luma(posterize(uv+float2(0,-px.y),t));     float br=luma(posterize(uv+float2(px.x,-px.y),t));
    float gx=-tl-2.0f*ml-bl+tr+2.0f*mr+br;
    float gy=-tl-2.0f*tc-tr+bl+2.0f*bc+br;
    float edge=metal::smoothstep(0.05f,0.3f, metal::sqrt(gx*gx+gy*gy));
    float3 col=metal::mix(base, float3(0.05f), edge);
    return float4(col,1.0f);
}
