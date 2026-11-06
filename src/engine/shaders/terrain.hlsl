// Volumetric terrain with the province layer draped over it (E3-T04, ADR-0003 phase P1).
//
// CDLOD: every terrain node is the same (cells x cells) grid instanced with a per-node placement
// (x, y, size, level). Heights come from a texture in the vertex shader; vertices morph towards the
// coarser level near the end of a node's range so there are no pops or cracks between levels.
// World: x = east, y = south (texel units), z = up. Water is a flat plane at z = 0.

cbuffer FrameConstants : register(b0) {
    row_major float4x4 view_proj;
    float4 eye_zoom;      // xyz = camera position, w = zoom fraction (0 near .. 1 far)
    float4 map_info;      // x = width, y = height (texels), z = relief (world height of full scale), w = sea level (0..1)
    float4 select_info;   // x = hovered id, y = selected id, z = tint enabled (0/1), w = time
    float4 lod_ranges[4]; // 16 per-level split ranges (float4 packed)
    float4 patch_info;    // x = cells per patch edge
};

cbuffer MaterialConstants : register(b1) {
    float4 material_colors[16];
};

Texture2D<float> heights : register(t0);
Texture2D<float2> normals : register(t1);
Texture2D<uint> provinces : register(t2);
Texture2D<uint> materials : register(t3);
Texture2D<float4> palette : register(t4);
SamplerState linear_clamp : register(s0);

struct VsIn {
    float2 grid : POSITION;       // per vertex: (0..cells, 0..cells)
    float4 node : TEXCOORD0;      // per instance: x, y, size, level
};

struct VsOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
    float3 world : TEXCOORD1;
};

float level_range(uint level) {
    return lod_ranges[level >> 2][level & 3];
}

float surface_z(float2 world_xy) {
    const float2 uv = world_xy / map_info.xy;
    const float raw = heights.SampleLevel(linear_clamp, uv, 0);
    return max(raw - map_info.w, 0.0) * map_info.z;  // raw is height / 65535, relief the world height of full scale
}

VsOut vs_main(VsIn input) {
    const uint level = (uint)(input.node.w + 0.5);
    const float cell = input.node.z / patch_info.x;

    // Distance used for morphing is measured to the unmorphed vertex.
    const float2 xy0 = input.node.xy + input.grid * cell;
    const float d = distance(eye_zoom.xyz, float3(xy0, surface_z(xy0)));

    // A node of level l is split while the camera is closer than r_l; its parent (range 2 r_l) takes over
    // beyond that, so the node must look like the parent by then. Level 0 has no parent.
    const float r = level_range(level);
    const float k = level == 0 ? 0.0 : saturate((d - 1.5 * r) / (0.5 * r));
    const float2 odd = frac(input.grid * 0.5) * 2.0;  // 1 for odd grid coordinates
    const float2 xy = input.node.xy + (input.grid - odd * k) * cell;

    const float z = surface_z(xy);
    VsOut o;
    o.pos = mul(view_proj, float4(xy, z, 1.0));
    o.uv = xy / map_info.xy;
    o.world = float3(xy, z);
    return o;
}

float3 material_at(int2 texel, int2 size) {
    texel = clamp(texel, int2(0, 0), size - 1);
    return material_colors[materials.Load(int3(texel, 0)) & 15].rgb;
}

float3 palette_color(uint province) {
    return palette.Load(int3(province & 255, province >> 8, 0)).rgb;
}

uint province_at(int2 texel, int2 size) {
    texel = clamp(texel, int2(0, 0), size - 1);
    return provinces.Load(int3(texel, 0));
}

float4 ps_main(VsOut input) : SV_Target {
    const int2 size = int2(map_info.xy);
    const float2 texf = input.uv * map_info.xy;
    const int2 texel = clamp(int2(floor(texf)), int2(0, 0), size - 1);
    const float raw = heights.SampleLevel(linear_clamp, input.uv, 0);
    const bool water = raw <= map_info.w;

    float3 color;
    if (water) {
        const float depth = saturate((map_info.w - raw) / 0.18);
        color = lerp(float3(0.17, 0.36, 0.50), float3(0.04, 0.12, 0.26), sqrt(depth));
        return float4(color, 1.0);
    }

    // Surface material with bilinear blending so class changes do not show as hard texel edges.
    const float2 f = texf - 0.5;
    const int2 base = int2(floor(f));
    const float2 t = frac(f);
    const float3 m0 = lerp(material_at(base, size), material_at(base + int2(1, 0), size), t.x);
    const float3 m1 = lerp(material_at(base + int2(0, 1), size), material_at(base + int2(1, 1), size), t.x);
    color = lerp(m0, m1, t.y);

    // Political layer: palette colour per province; weaker when close so the landscape shows through.
    const uint prov = provinces.Load(int3(texel, 0));
    const uint hovered = (uint)(select_info.x + 0.5);
    const uint selected = (uint)(select_info.y + 0.5);
    if (select_info.z > 0.5 && prov != 0) {
        const float strength = lerp(0.35, 0.78, eye_zoom.w);
        color = lerp(color, palette_color(prov), strength);
    }

    // Lighting from the normal map; sun from the north-west.
    const float2 n2 = normals.SampleLevel(linear_clamp, input.uv, 0);
    const float3 n = float3(n2, sqrt(saturate(1.0 - dot(n2, n2))));
    const float3 sun = normalize(float3(-0.55, -0.65, 0.62));
    const float diffuse = saturate(dot(n, sun));
    const float shade = 0.50 + 0.70 * diffuse;
    color *= shade;

    // Highlights.
    if (prov != 0 && prov == selected) {
        color = lerp(color, float3(1.0, 0.93, 0.62), 0.32);
    } else if (prov != 0 && prov == hovered) {
        color = lerp(color, float3(1.0, 1.0, 1.0), 0.16);
    }

    // Province borders with constant width in screen pixels: the sample offset follows the texel footprint.
    if (prov != 0) {
        const float footprint = max(length(ddx(texf)), length(ddy(texf)));  // texels per pixel
        const bool near_selected = prov == selected;
        const float width_px = near_selected ? 1.9 : 1.1;
        const int o = max(1, (int)round(width_px * footprint * 0.5));
        uint ids[4];
        ids[0] = province_at(texel + int2(o, 0), size);
        ids[1] = province_at(texel - int2(o, 0), size);
        ids[2] = province_at(texel + int2(0, o), size);
        ids[3] = province_at(texel - int2(0, o), size);
        bool border = false;
        bool selected_border = false;
        [unroll] for (int i = 0; i < 4; ++i) {
            const bool other = ids[i] != 0 && ids[i] != prov;
            border = border || other;
            selected_border = selected_border || (other && (ids[i] == selected || prov == selected));
        }
        if (selected_border) {
            color = float3(1.0, 0.86, 0.30);
        } else if (border) {
            color *= 0.38;
        }
    }
    return float4(saturate(color), 1.0);
}
