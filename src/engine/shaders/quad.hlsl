// Screen-space coloured quads (E3-T02). Positions are in pixels, origin at the top-left.

cbuffer FrameConstants : register(b0) {
    float2 viewport_px;
    float2 padding;
};

struct VsIn {
    float2 pos : POSITION;
    float4 color : COLOR;
};

struct VsOut {
    float4 pos : SV_Position;
    float4 color : COLOR;
};

VsOut vs_main(VsIn input) {
    VsOut output;
    const float2 ndc = float2(input.pos.x / viewport_px.x * 2.0 - 1.0, 1.0 - input.pos.y / viewport_px.y * 2.0);
    output.pos = float4(ndc, 0.0, 1.0);
    output.color = input.color;
    return output;
}

float4 ps_main(VsOut input) : SV_Target {
    return input.color;
}
