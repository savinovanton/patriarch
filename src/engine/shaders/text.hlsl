// Signed-distance-field text (E3-T05). The atlas stores distance to the glyph outline: 0.5 on the outline,
// larger inside. Edges are anti-aliased with the screen-space derivative of the field, so text stays sharp
// at any size; a second, lower threshold gives an outline (halo) that keeps labels readable on busy terrain.

cbuffer TextConstants : register(b0) {
    float2 viewport;
    float outline_width;  // in field units: atlas pixels of outline / (2 * spread)
    float halo_soft;      // in field units: extra width over which the halo fades out (0 = crisp)
};

Texture2D atlas : register(t0);
SamplerState samp : register(s0);

struct VsIn {
    float2 pos : POSITION;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
    float4 outline : COLOR1;
};

struct VsOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
    float4 outline : COLOR1;
};

VsOut vs_main(VsIn input) {
    VsOut o;
    o.pos = float4(input.pos.x / viewport.x * 2.0 - 1.0, 1.0 - input.pos.y / viewport.y * 2.0, 0.0, 1.0);
    o.uv = input.uv;
    o.color = input.color;
    o.outline = input.outline;
    return o;
}

float4 ps_main(VsOut input) : SV_Target {
    const float d = atlas.Sample(samp, input.uv).r;
    const float smoothing = max(fwidth(d) * 0.75, 0.002);
    const float fill = smoothstep(0.5 - smoothing, 0.5 + smoothing, d);
    const float edge = 0.5 - outline_width;
    const float halo = smoothstep(edge - smoothing - halo_soft, edge + smoothing, d);
    const float3 rgb = lerp(input.outline.rgb, input.color.rgb, fill);
    const float alpha = max(fill * input.color.a, halo * input.outline.a);
    return float4(rgb, alpha);
}
