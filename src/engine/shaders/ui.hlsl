// RmlUi geometry (E3-T06). Vertices carry premultiplied-alpha colours; textures are premultiplied too.
// Positions are in pixels (origin top-left); RmlUi supplies an optional 4x4 transform (column-major, the
// default RmlUi layout) that is applied before the pixel -> clip space mapping, so 3D transforms work.

cbuffer UiConstants : register(b0) {
    column_major float4x4 transform;
    float2 translation;
    float2 viewport;
};

Texture2D tex : register(t0);
SamplerState samp : register(s0);

struct VsIn {
    float2 pos : POSITION;
    float4 color : COLOR;
    float2 uv : TEXCOORD0;
};

struct VsOut {
    float4 pos : SV_Position;
    float4 color : COLOR;
    float2 uv : TEXCOORD0;
};

VsOut vs_main(VsIn input) {
    const float4 p = mul(transform, float4(input.pos + translation, 0.0, 1.0));
    VsOut o;
    // Homogeneous version of ndc = (2x / w - 1, 1 - 2y / h) so perspective transforms stay correct.
    o.pos = float4(2.0 * p.x / viewport.x - p.w, p.w - 2.0 * p.y / viewport.y, 0.0, p.w);
    o.color = input.color;
    o.uv = input.uv;
    return o;
}

float4 ps_main(VsOut input) : SV_Target {
    return tex.Sample(samp, input.uv) * input.color;
}
