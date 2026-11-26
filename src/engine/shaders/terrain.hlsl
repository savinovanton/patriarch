// Volumetric terrain with the province layer draped over it (E3-T04, ADR-0003 phase P1).
//
// CDLOD: every terrain node is the same (cells x cells) grid instanced with a per-node placement
// (x, y, size, level). Heights come from a texture in the vertex shader; vertices morph towards the
// coarser level near the end of a node's range so there are no pops or cracks between levels.
// World: x = east, y = south (texel units), z = up. Water is a flat plane at z = 0.
//
// Phase P2 lighting: a directional sun, a sky/ground ambient, soft shadows cast by the relief itself (the
// pixel shader marches a ray towards the sun through the height texture, so no shadow map is needed),
// albedo variation, rock on steep slopes, snow on high ground and distance haze.

cbuffer FrameConstants : register(b0) {
    row_major float4x4 view_proj;
    float4 eye_zoom;      // xyz = camera position, w = zoom fraction (0 near .. 1 far)
    float4 map_info;      // x = width, y = height (texels), z = relief (world height of full scale), w = sea level (0..1)
    float4 select_info;   // x = hovered id, y = selected id, z = tint enabled (0/1), w = time
    float4 lod_ranges[4]; // 16 per-level split ranges (float4 packed)
    float4 patch_info;    // x = cells per patch edge
    float4 sun_dir;       // xyz = unit vector towards the sun
    float4 light_info;    // x = shadow strength, y = shadow steps, z = haze, w = longest shadow (texels)
    float4 sun_color;     // rgb = sunlight colour times intensity (0 at night), w = 1 when the sky is drawn
    float4 ambient_mul;   // rgb = multiplier of the day ambient light
    float4 haze_color;    // rgb = colour of the haze and the horizon
    float4 style_info;    // x = winter palette (0..1), y = colour grading strength, z = vignette, w = painted borders (0/1)
    float4 view_info;     // x, y = target size in pixels, z, w = their reciprocals
    float4 season_info;   // x = autumn tint (0..1)
    float4 paint_info;    // x = painterly strength (0..1), y, z = wind direction (unit, x east, y south), w = focus distance
    float4 farm_info;     // x = farmland strips strength (0..1)
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

// Winter 1703 palette (E5 #269, measured from the winter concept of board 01): fields are warm snow-beige,
// forests dark desaturated conifer, rock and hills muted. Same order as the Material enum; blended with the
// summer colours by style_info.x, so 0 reproduces the summer palette exactly.
static const float3 winter_colors[10] = {
    float3(0.04, 0.12, 0.26),  // DeepWater (water is drawn by water_color)
    float3(0.17, 0.36, 0.50),  // ShallowWater
    float3(0.78, 0.74, 0.66),  // Beach
    float3(0.84, 0.82, 0.77),  // Plains: open snow
    float3(0.80, 0.76, 0.66),  // Steppe: snow with dry grass showing through
    float3(0.17, 0.21, 0.17),  // Forest: dark conifer
    float3(0.68, 0.63, 0.54),  // Hills
    float3(0.44, 0.43, 0.40),  // Rock
    float3(0.90, 0.88, 0.84),  // Snow
    float3(0.72, 0.71, 0.65),  // Tundra
};

// Early October (E5-A4): the summer colours turn golden - meadows ochre, steppe straw, forest amber and
// russet, hills warm. Blended in by season_info.x, so 0 leaves the summer palette untouched.
static const float3 autumn_colors[10] = {
    float3(0.04, 0.12, 0.26),  // DeepWater
    float3(0.17, 0.36, 0.50),  // ShallowWater
    float3(0.80, 0.72, 0.52),  // Beach
    float3(0.66, 0.57, 0.25),  // Plains: stubble and yellowing grass
    float3(0.76, 0.60, 0.28),  // Steppe: straw
    float3(0.34, 0.32, 0.12),  // Forest: amber and russet among the dark conifers
    float3(0.60, 0.44, 0.25),  // Hills
    float3(0.50, 0.47, 0.44),  // Rock
    float3(0.94, 0.95, 0.98),  // Snow
    float3(0.64, 0.58, 0.46),  // Tundra
};

// Colour of a material through the year: the summer palette, tinted towards autumn, blended into the winter one.
float3 material_at(int2 texel, int2 size) {
    texel = clamp(texel, int2(0, 0), size - 1);
    const uint m = materials.Load(int3(texel, 0)) & 15;
    const float3 summer = lerp(material_colors[m].rgb, autumn_colors[min(m, 9u)], season_info.x);
    return lerp(summer, winter_colors[min(m, 9u)], style_info.x);
}

float3 palette_color(uint province) {
    return palette.Load(int3(province & 255, province >> 8, 0)).rgb;
}

uint province_at(int2 texel, int2 size) {
    texel = clamp(texel, int2(0, 0), size - 1);
    return provinces.Load(int3(texel, 0));
}

float hash21(float2 p) {
    p = frac(p * float2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return frac(p.x * p.y);
}

float value_noise(float2 p) {
    const float2 i = floor(p);
    float2 f = frac(p);
    f = f * f * (3.0 - 2.0 * f);
    return lerp(lerp(hash21(i), hash21(i + float2(1, 0)), f.x), lerp(hash21(i + float2(0, 1)), hash21(i + float2(1, 1)), f.x), f.y);
}

// Painterly details (E5-A6, docs/dev/map-art-direction.md section 11). The snow of the concept is drawn in long
// wind-swept strokes with swirls, not in a value-noise grain. Two octaves of noise stretched along the wind
// (about 12:1) give the strokes; a slow warp of the coordinates bends them into sweeping curves, and the height
// of the ground is added to the across-wind coordinate, so on slopes the strokes turn along the contour lines
// (iso-lines of v run where the height changes against the wind). Every octave fades out before it would
// alias. Signed, about -0.5..0.5. All sizes are in texels, so the look does not depend on the map. `warp` is
// the slow field (-0.5..0.5) that bends the strokes, also used for the broad colour variations.
float snow_strokes(float2 p, float height, float footprint, out float2 warp) {
    const float2 wind = paint_info.yz;
    warp = float2(value_noise(p * 0.0045), value_noise(p * 0.0045 + 19.7)) - 0.5;
    p += warp * 80.0;  // gentle: a stronger warp folds the field into concentric "knots" like wood grain
    const float u = dot(p, wind);
    const float v = dot(p, float2(-wind.y, wind.x)) + height * 1.2;
    const float o1 = value_noise(float2(u * 0.014, v * 0.15));
    const float o2 = value_noise(float2(u * 0.034, v * 0.38) + 31.4);
    const float f1 = saturate(2.0 - footprint * 0.28);  // 7-texel strokes are gone by about 7 texels per pixel
    const float f2 = saturate(1.6 - footprint * 0.50);  // the finer octave by about 3.2
    return (o1 - 0.5) * 0.72 * f1 + (o2 - 0.5) * 0.28 * f2;
}

// Broad colour variations of a painted landscape: patches of about a province's size a little lighter or
// darker, warmer or cooler. Low frequency, so it holds at any zoom; it reuses the slow warp field of the strokes
// (two noise lookups instead of more). Returns a colour multiplier.
float3 macro_variation(float2 warp) {
    return (1.0 + warp.x * 0.18) * (1.0 + warp.y * float3(0.12, 0.02, -0.12));
}

// Farmland in strips (E5-A9, docs/dev/map-art-direction.md section 12). Until the farmland mask of the map
// (A7) exists, fields lie on the plains material. The land is cut into village cells of kVillageCell texels
// (gently warped); in some of them a clump of fields lies around a point of the cell - blocks of about
// kFieldCell texels in a grid turned by the village and offset row by row like brickwork, with a ragged
// outline. Every block is one field of the three-field rotation - winter rye, spring crop, fallow - ploughed
// in strips of kStripPeriod texels, each household's strip a little different, a dark furrow between them
// and a grass balk around the block. The strips run by a hash of the block on flat ground and along the
// contour lines on slopes. Summer: ripening rye yellow, spring crop green, fallow grey-brown; autumn: stubble
// and the dark ploughland of the winter sowing; winter: snow with only a faint ochre trace of the furrows.
// Sizes are in texels (the real map has about 1 km per texel, so the fields are stylised, sized for the
// screen); the strips fade out before they would alias (full from 4 pixels per strip), the fields by
// kFieldFar texels per pixel.
static const float kVillageCell = 56.0;
static const float2 kFieldCell = float2(9.0, 5.0);  // long and short side of a field block
static const float kStripPeriod = 1.5;
static const float kFieldFar = 1.0;

// 1 where fields are drawn at full strength (up to 0.5 texels per pixel), fading to 0 by kFieldFar.
float farmland_near(float footprint) {
    return saturate((kFieldFar - footprint) * 2.0);
}

uint material_id(int2 texel, int2 size) {
    texel = clamp(texel, int2(0, 0), size - 1);
    return materials.Load(int3(texel, 0)) & 15;
}

// Summer crops and autumn fields of the three-field rotation (rye, spring crop, fallow).
static const float3 kFieldSummer[3] = {float3(0.74, 0.67, 0.40), float3(0.38, 0.50, 0.25), float3(0.58, 0.52, 0.38)};
static const float3 kFieldAutumn[3] = {float3(0.78, 0.69, 0.47), float3(0.70, 0.61, 0.42), float3(0.47, 0.39, 0.29)};

// `color` with the fields painted over it; `amount` = strength x nearness.
float3 farmland(float3 color, float2 texf, int2 size, float footprint, float amount) {
    // Plains only, blended over the texel edges like the material colour. Tested first: the same four texels
    // the material blend has just loaded (cached), so forest, hills and steppe leave at once.
    const float2 f = texf - 0.5;
    const int2 b = int2(floor(f));
    const float2 t = frac(f);
    const float4 p = float4(material_id(b, size) == 3, material_id(b + int2(1, 0), size) == 3,
                            material_id(b + int2(0, 1), size) == 3, material_id(b + int2(1, 1), size) == 3);
    const float mask = smoothstep(0.35, 0.65, lerp(lerp(p.x, p.y, t.x), lerp(p.z, p.w, t.x), t.y));
    if (mask <= 0.0) {
        return color;
    }

    // A slow warp of about 60 texels (two sines each - cheaper than value noise, smooth enough for outlines).
    const float2 warp = 0.5 * sin(texf.yx * float2(0.071, 0.083) + 2.0 * sin(texf * float2(0.043, 0.037)));
    const float2 qv = texf / kVillageCell + warp * 0.35;
    const float2 village = floor(qv);
    const float hv = hash21(village + 0.37);
    if (hv >= 0.6) {
        return color;  // no village lands here: meadow, waste
    }

    // Field blocks in the frame of the village, around its centre.
    const float2 centre = village + 0.3 + 0.4 * float2(hash21(village + 5.1), hash21(village + 9.7));
    const float turn = hash21(village + 2.9) * 3.14159265;
    const float2 axis = float2(cos(turn), sin(turn));
    const float2 side = float2(-axis.y, axis.x);
    // The grid takes only a trace of the warp: straight-edged blocks, a little hand-drawn.
    const float2 rel = texf + warp * 6.0 - centre * kVillageCell;  // in texels from the centre
    float2 g = float2(dot(rel, axis), dot(rel, side)) / kFieldCell;  // in blocks
    const float row_shift = hash21(float2(floor(g.y), hv * 17.0));
    g.x += row_shift;
    const float2 block = floor(g);
    const float hb = hash21(block + hv * 53.0);
    const float radius = 2.6 + 1.4 * hash21(village + 4.4);  // in short sides: from a hamlet to a large village
    const float2 offset = float2(block.x + 0.5 - row_shift, block.y + 0.5) * kFieldCell / kFieldCell.y;
    if (length(offset) > radius + (hb - 0.5) * 0.5) {
        return color;
    }


    // Direction across the strips (in the block frame): along one side of the block by a hash, turned downhill
    // on slopes, so the strips follow the contours. The slope is taken once at the block centre, so the strips
    // of a block stay straight.
    float2 across = hash21(block + 11.9 + hv) < 0.5 ? float2(1.0, 0.0) : float2(0.0, 1.0);
    const float2 block_rel = offset * kFieldCell.y;  // texels from the village centre, block frame
    const float2 block_tex = centre * kVillageCell + axis * block_rel.x + side * block_rel.y;
    const float2 n_xy = normals.SampleLevel(linear_clamp, block_tex / map_info.xy, 0);
    const float slope = length(n_xy);
    if (slope > 1.0e-4) {
        float2 down = n_xy / slope;
        down = float2(dot(down, axis), dot(down, side));
        across = normalize(lerp(dot(across, down) < 0.0 ? -across : across, down, smoothstep(0.03, 0.12, slope)));
    }

    const float2 local = frac(g);
    const float phase = dot(local * kFieldCell, across) / kStripPeriod;
    const float strip = hash21(float2(floor(phase), hb * 91.7));
    const float detail = saturate(2.0 - 4.0 * footprint / kStripPeriod);  // full from 4 px per strip, none at 2
    const float furrow = smoothstep(0.34, 0.5, abs(frac(phase) - 0.5)) * detail;
    const float2 edges = min(local, 1.0 - local) * kFieldCell;
    const float edge = min(edges.x, edges.y);
    const float balk = 1.0 - smoothstep(0.10, 0.35, edge);  // texels of grass around the block

    const uint kind = min((uint)(hb * 3.0), 2u);
    float3 field = lerp(kFieldSummer[kind], kFieldAutumn[kind], season_info.x);
    field *= (1.0 + (strip - 0.5) * 0.16 * detail) * (1.0 - 0.12 * furrow);
    field = lerp(field, color, 0.2);  // a touch of the ground colour keeps the fields in the painting
    const float3 bare = lerp(field, color, balk);
    // Under snow only the ridges of the furrows show through, in the ochre of the concept (#ae9b83).
    const float3 snowy =
        lerp(color, float3(0.68, 0.61, 0.51), (0.06 + 0.32 * furrow * (0.5 + strip)) * (1.0 - balk));
    return lerp(color, lerp(bare, snowy, style_info.x), mask * amount);
}

// Soft shadow of the relief: 1 = lit, 0 = in shadow. The ray starts slightly above the surface and its step
// grows geometrically up to the longest possible shadow; (ray height - terrain height) / distance gives a
// penumbra that widens with distance from the occluder.
float sun_shadow(float3 p) {
    const int steps = (int)(light_info.y + 0.5);
    if (steps <= 0 || light_info.x <= 0.0) {
        return 1.0;
    }
    const float3 sun = sun_dir.xyz;
    const float max_dist = max(light_info.w, 4.0);
    const float growth = pow(max_dist / 1.5, 1.0 / steps);
    float t = 1.5;
    float lit = 1.0;
    [loop] for (int i = 0; i < steps; ++i) {
        const float3 q = p + sun * t;
        if (q.z > map_info.z) {
            break;  // above the highest possible terrain: nothing can shade the point any more
        }
        const float h = surface_z(q.xy);
        lit = min(lit, saturate(6.0 * (q.z - h) / t));
        if (lit < 0.02) {
            break;
        }
        t *= growth;
    }
    return lerp(1.0, lit, light_info.x * (1.0 - 0.4 * style_info.x));  // winter light is softer: shadows are shallower
}

static const float kSummerExposure = 1.75;    // winter is brightened by 2.7, the high summer sun needs much less
static const float kSummerSaturation = 0.90;  // winter keeps 0.72 of the colour

// Colour of the haze and the horizon (before grading); the sky behind the map starts from the same colour.
float3 scene_haze() {
    // Winter haze is a warm grey instead of blue (the concept's horizon), of the same brightness.
    const float haze_luma = dot(haze_color.rgb, float3(0.3333, 0.3333, 0.3333));
    return lerp(haze_color.rgb, haze_luma * float3(1.04, 0.99, 0.90), style_info.x * 0.8);
}

float3 apply_haze(float3 c, float3 world) {
    const float d = distance(eye_zoom.xyz, world);
    float amount = (1.0 - exp(-pow(d / (0.9 * map_info.x), 1.6) * 0.9)) * 0.55 * light_info.z;
    // With the sky (sun_color.w = 1) the edge of the map dissolves into the haze, so the landscape runs into the
    // sky behind it without a visible seam.
    const float2 uv = world.xy / map_info.xy;
    const float edge = min(min(uv.x, 1.0 - uv.x), min(uv.y, 1.0 - uv.y));
    amount = max(amount, sun_color.w * (1.0 - smoothstep(0.0, 0.05, edge)));
    return lerp(c, scene_haze(), saturate(amount));
}

// Colour grading and vignette (E5 #269): the low winter sun leaves the lit scene dim, so the picture is
// brightened, passed through a soft shoulder (no clipped snow), toned cool in the shadows and warm in the
// lights and desaturated towards the muted painterly look of the concept; the vignette darkens the edges so
// the UI panels read on top. The grading follows the season (E5-A4): the high summer sun needs a softer
// exposure, keeps more of the colour (green stays green) and a lighter split toning, all eased by the winter
// factor so a changing date never makes the picture jump. Identity when both strengths are 0.
float3 apply_style(float3 c, float2 pixel) {
    const float g = style_info.y;
    if (g > 0.0) {
        const float winter = style_info.x;
        float3 x = c * lerp(1.0, lerp(kSummerExposure * (1.0 + 0.12 * season_info.x), 2.7, winter), g);
        const float l = dot(x, float3(0.2126, 0.7152, 0.0722));
        const float3 tone = lerp(float3(0.94, 0.98, 1.06), float3(1.04, 1.0, 0.94), smoothstep(0.10, 0.65, l));
        x *= lerp(float3(1, 1, 1), tone, g * lerp(0.55, 1.0, winter));
        x = lerp(dot(x, float3(0.2126, 0.7152, 0.0722)).xxx, x, lerp(1.0, lerp(kSummerSaturation, 0.72, winter), g));
        x = x * (1.0 + x / 2.56) / (1.0 + x);  // extended Reinhard shoulder, white point 1.6
        x += 0.05 * (1.0 - x);                 // lifted blacks: dark forest stays a colour, not a hole
        c = lerp(c, x, g);
    }
    if (style_info.z > 0.0) {
        const float2 q = (pixel * view_info.zw - 0.5) * float2(1.8, 2.0);
        c *= 1.0 - style_info.z * smoothstep(0.35, 1.6, dot(q, q));
    }
    return c;
}

float4 finish(float3 c, float3 world, float2 pixel) {
    return float4(saturate(apply_style(apply_haze(c, world), pixel)), 1.0);
}

// Same, then the border layer (rgb + coverage) is laid over the graded picture, so its inks and golds keep
// the exact colours of the design instead of going through exposure and tone mapping.
float4 finish(float3 c, float3 world, float2 pixel, float4 overlay) {
    const float3 graded = saturate(apply_style(apply_haze(c, world), pixel));
    return float4(lerp(graded, overlay.rgb, overlay.a), 1.0);
}

// Sea surface (phase P3a): two scrolling noise layers give a wave normal that flattens with distance (it
// would alias), Fresnel blends the depth colour with the sky, the sun leaves a glint and the shallows near
// the coast carry foam.
float wave_height(float2 p, float time) {
    // Two scrolling layers: long swell and shorter chop (heights in texel units).
    return value_noise(p * 0.045 + float2(time * 0.060, time * 0.035)) * 2.2 +
           value_noise(p * 0.13 + float2(-time * 0.11, time * 0.17)) * 0.9;
}

float3 water_color(float3 world, float2 uv, float raw) {
    const float depth = saturate((map_info.w - raw) / 0.18);
    float3 deep = lerp(float3(0.17, 0.36, 0.50), float3(0.04, 0.12, 0.26), sqrt(depth));
    deep = lerp(deep, float3(0.20, 0.25, 0.28) * lerp(1.6, 1.0, sqrt(depth)), style_info.x * 0.75);  // cold steel water

    const float time = select_info.w;
    const float2 p = world.xy;
    const float eps = 1.5;  // texels
    const float ha = wave_height(p, time);
    const float hx = wave_height(p + float2(eps, 0), time);
    const float hy = wave_height(p + float2(0, eps), time);
    // Waves fade out when a pixel covers more than a couple of texels (they would shimmer and alias).
    const float footprint = max(length(ddx(world.xy)), length(ddy(world.xy)));
    const float calm = saturate(1.6 - footprint * 0.5);
    const float2 slope = float2(hx - ha, hy - ha) / eps * calm;
    const float3 n = normalize(float3(-slope, 1.0));

    const float3 v = normalize(eye_zoom.xyz - world);
    const float cos_v = saturate(dot(n, v));
    const float fresnel = clamp(0.02 + 0.98 * pow(1.0 - cos_v, 5.0), 0.0, 0.65);
    const float3 sky = lerp(float3(0.55, 0.66, 0.78), float3(0.30, 0.45, 0.68), saturate(v.z * 1.5)) * ambient_mul.rgb;
    float3 color = lerp(deep, sky, fresnel);

    const float3 h = normalize(sun_dir.xyz + v);
    const float glint = pow(saturate(dot(n, h)), 220.0) * 1.6 * calm;

    const float shore = saturate(1.0 - (map_info.w - raw) / 0.02);
    const float foam = shore * (0.55 + 0.45 * value_noise(p * 0.9 + time * 0.8));
    color = lerp(color, float3(0.90, 0.95, 0.97), foam * 0.7 * calm);
    // Daylight reaches the water mostly through the sky: follow it down at dusk and at night (the glint below
    // carries the sunlight itself).
    color *= lerp(ambient_mul.rgb, float3(1, 1, 1), 0.2);
    color += sun_color.rgb * glint;
    return color;
}

// Province borders without a staircase (E5-A2). The id texture is a hard mask, so the border is rebuilt as a
// smooth field: the indicator "texel belongs to province P" on a 3x3 grid is interpolated with a quadratic
// B-spline (C1, so a one-texel staircase is rounded off) and the 0.5 iso-line of that field is the border.
// Away from the iso-line the raw field is not a distance (its gradient dies out), so the distance is read off
// the known profile of the spline across a straight edge, f ~ 0.5 + 0.5 tanh(1.7 x) with x in grid cells,
// and converted to screen pixels with the steepest per-pixel change of the texel position along the edge
// normal (this also handles the foreshortening of a tilted view). The sign is positive inside P. The grid
// spacing is a whole number of texels and grows with the texel footprint, so the field is sampled at pixel
// rate far away (no shimmering) and the samples of neighbouring pixels line up (no noise on the line).
static const float kFarFromBorder = 1.0e4;  // pixels; "no border within the support of the field"

float3 border_weights(float d, out float3 deriv) {
    // d in [-0.5, 0.5] is the offset from the nearest grid node; weights of the nodes -1, 0, +1 (xyz).
    deriv = float3(d - 0.5, -2.0 * d, d + 0.5);
    return float3(0.5 * (0.5 - d) * (0.5 - d), 0.75 - d * d, 0.5 * (0.5 + d) * (0.5 + d));
}

float distance_to_edge(float field[9], float3 wx, float3 dwx, float3 wy, float3 dwy, float stride,
                       float2 dtex_dx, float2 dtex_dy) {
    float f = 0.0;
    float2 g = float2(0.0, 0.0);
    [unroll] for (int j = 0; j < 3; ++j) {
        [unroll] for (int i = 0; i < 3; ++i) {
            const float v = field[j * 3 + i];
            f += v * wx[i] * wy[j];
            g += v * float2(dwx[i] * wy[j], wx[i] * dwy[j]);
        }
    }
    const float2 normal = g / max(length(g), 1e-6);
    const float texels_per_pixel = max(length(float2(dot(normal, dtex_dx), dot(normal, dtex_dy))), 1e-3);
    const float x = clamp(2.0 * f - 1.0, -0.97, 0.97);
    return 0.5 * log((1.0 + x) / (1.0 - x)) * (1.0 / 1.7) * stride / texels_per_pixel;
}

// Signed distances in pixels to the edge of the pixel's own province (sea counts as inside) and to the edge
// of the selected province (large negative when nothing is selected), from one set of 3x3 id taps.
void border_distances(float2 texf, int2 size, uint own_id, uint selected, float stride, float2 dtex_dx,
                      float2 dtex_dy, out float own_distance, out float selected_distance) {
    const float2 grid = texf / stride - 0.5;
    const float2 node = floor(grid + 0.5);
    const float2 d = grid - node;
    float3 dwx;
    float3 dwy;
    const float3 wx = border_weights(d.x, dwx);
    const float3 wy = border_weights(d.y, dwy);

    // Most pixels are far from any border. A line through the middle of the support separates at least one of
    // its four corners from the centre, so five taps are enough to see that all agree and the field is
    // constant, which is much cheaper than loading all nine and evaluating the spline.
    float own[9];
    float sel[9];
    [unroll] for (int k = 0; k < 9; ++k) {
        own[k] = 0.0;
        sel[k] = 0.0;
    }
    float own_sum = 0.0;
    float sel_sum = 0.0;
    [unroll] for (int t = 0; t < 5; ++t) {
        const int i = t < 4 ? (t & 1) * 2 : 1;
        const int j = t < 4 ? (t >> 1) * 2 : 1;
        const uint id = province_at(int2(floor((node + float2(i - 1, j - 1) + 0.5) * stride)), size);
        own[j * 3 + i] = (id == own_id || id == 0) ? 1.0 : 0.0;
        sel[j * 3 + i] = (selected != 0 && id == selected) ? 1.0 : 0.0;
        own_sum += own[j * 3 + i];
        sel_sum += sel[j * 3 + i];
    }
    if (own_sum > 4.5 && (sel_sum < 0.5 || sel_sum > 4.5)) {
        own_distance = kFarFromBorder;
        selected_distance = sel_sum > 4.5 ? kFarFromBorder : -kFarFromBorder;
        return;
    }
    [unroll] for (int e = 0; e < 4; ++e) {
        const int i = e < 2 ? e * 2 : 1;
        const int j = e < 2 ? 1 : (e - 2) * 2;
        const uint id = province_at(int2(floor((node + float2(i - 1, j - 1) + 0.5) * stride)), size);
        own[j * 3 + i] = (id == own_id || id == 0) ? 1.0 : 0.0;
        sel[j * 3 + i] = (selected != 0 && id == selected) ? 1.0 : 0.0;
    }
    own_distance = distance_to_edge(own, wx, dwx, wy, dwy, stride, dtex_dx, dtex_dy);
    selected_distance = distance_to_edge(sel, wx, dwx, wy, dwy, stride, dtex_dx, dtex_dy);
}

// "Over" compositing of a colour with coverage onto a layer (rgb + coverage, not premultiplied).
float4 layer_over(float4 dst, float3 rgb, float coverage) {
    const float a = coverage + dst.a * (1.0 - coverage);
    return float4((rgb * coverage + dst.rgb * dst.a * (1.0 - coverage)) / max(a, 1e-4), a);
}

// The layer over the graded picture. Everything is sized in screen pixels of the 1080p design layout (scaled
// with the target height, so a 4K frame keeps the proportions): a thin ink line (#242e29) in the tone of the
// land, weaker when far away; the selected province gets a light fill (#365847 at 12 %), an inner light line
// (#eee5d2 at 38 %, 1.3 px, about 3 px inside) and a gold contour (#d2b77d, 3 px, soft edge) centred on its
// border, which is why the field of the selected province is used rather than the pixel's own (half of the
// contour lies on the neighbour's side). `inner` is the same distance from a coarser grid, which can see
// far enough inside for the inner line.
float4 border_overlay(float own_distance, float selected_distance, float inner_distance, bool has_selection,
                      float scale) {
    float4 layer = float4(0.141, 0.180, 0.161,
                          lerp(0.56, 0.36, eye_zoom.w) * saturate(0.5 * scale + 0.5 - abs(own_distance)));
    if (has_selection) {
        layer = layer_over(layer, float3(0.212, 0.345, 0.278), 0.12 * saturate(selected_distance + 0.5));
        layer = layer_over(layer, float3(0.933, 0.898, 0.824),
                           0.38 * saturate(0.65 * scale + 0.5 - abs(inner_distance - 3.2 * scale)));
        layer = layer_over(layer, float3(0.824, 0.718, 0.490),
                           1.0 - smoothstep(0.9 * scale, 2.1 * scale, abs(selected_distance)));
    }
    return layer;
}

float4 ps_main(VsOut input) : SV_Target {
    const int2 size = int2(map_info.xy);
    const float2 texf = input.uv * map_info.xy;
    const int2 texel = clamp(int2(floor(texf)), int2(0, 0), size - 1);
    const float raw = heights.SampleLevel(linear_clamp, input.uv, 0);
    const bool water = raw <= map_info.w;

    float3 color;
    if (water) {
        color = water_color(input.world, input.uv, raw);
        return finish(color, input.world, input.pos.xy);
    }

    const float2 dtex_dx = ddx(texf);
    const float2 dtex_dy = ddy(texf);
    const float footprint = max(length(dtex_dx), length(dtex_dy));  // texels per pixel

    // Painterly softening (E5-A6): far behind the point the camera looks at (paint_info.w) the picture softens
    // like a shallow depth of field - nothing in front of 1.15x the focus distance, full at 2.75x - and where a
    // texel is smaller than a pixel the material is averaged over the pixel's footprint instead of being point
    // sampled, so small details do not shimmer. Both only widen one rotated-grid set of four taps (the same
    // number of material loads as the bilinear blend), so no extra pass is needed; behind the focus the relief
    // shading and the colours are also softened below. (Averaging the normals as well cost +0.3 ms for little.)
    const float paint = paint_info.x;
    const float coc =
        paint > 0.0 ? paint * saturate((distance(eye_zoom.xyz, input.world) / paint_info.w - 1.15) / 1.6) : 0.0;
    const float spread = paint * 0.5 * saturate(footprint - 1.0) + 1.25 * coc;  // radius of the taps in pixels
    const float blur = saturate((footprint - 0.75) * 4.0) * saturate(spread * 8.0);

    // Surface material with bilinear blending so class changes do not show as hard texel edges.
    color = float3(0.0, 0.0, 0.0);
    if (blur < 1.0) {
        const float2 f = texf - 0.5;
        const int2 base = int2(floor(f));
        const float2 t = frac(f);
        const float3 m0 = lerp(material_at(base, size), material_at(base + int2(1, 0), size), t.x);
        const float3 m1 = lerp(material_at(base + int2(0, 1), size), material_at(base + int2(1, 1), size), t.x);
        color = lerp(m0, m1, t.y);
    }
    if (blur > 0.0) {
        static const float2 kTaps[4] = {float2(0.125, 0.375), float2(0.375, -0.125), float2(-0.125, -0.375),
                                        float2(-0.375, 0.125)};
        float3 sum = float3(0.0, 0.0, 0.0);
        [unroll] for (int i = 0; i < 4; ++i) {
            const float2 q = texf + (kTaps[i].x * dtex_dx + kTaps[i].y * dtex_dy) * (2.0 * spread);
            sum += material_at(int2(floor(q)), size);
        }
        color = lerp(color, sum * 0.25, blur);
    }

    // Normal of the relief (also used for the slope).
    const float2 n2 = normals.SampleLevel(linear_clamp, input.uv, 0);
    const float3 n = float3(n2, sqrt(saturate(1.0 - dot(n2, n2))));

    // Farmland strips near the camera (E5-A9); behind the focus they soften away with the rest.
    if (farm_info.x > 0.0 && footprint < kFieldFar) {
        const float amount = farm_info.x * farmland_near(footprint) * (1.0 - coc);
        if (amount > 0.0) {
            color = farmland(color, texf, size, footprint, amount);
        }
    }

    // Albedo variation fades out with distance (it would alias), rock on steep slopes, snow on high ground.
    // With the painterly details the strokes below take over most of the value-noise grain.
    const float detail = saturate(1.6 - footprint * 0.45);
    const float noise = value_noise(texf * 0.35) * 0.6 + value_noise(texf * 1.7) * 0.4;
    color *= 1.0 + (noise - 0.5) * (paint > 0.0 ? lerp(0.30, 0.12, paint) : 0.30) * detail;
    const float slope = 1.0 - n.z;
    const float rock = smoothstep(0.16, 0.40, slope + (noise - 0.5) * 0.08);
    color = lerp(color, float3(0.44, 0.40, 0.35) * (0.85 + 0.3 * noise), rock * 0.85);
    const float high = (raw - map_info.w) / max(1.0 - map_info.w, 0.001);
    const float snow = smoothstep(0.58, 0.78, high + (noise - 0.5) * 0.10) * (1.0 - rock * 0.75);
    color = lerp(color, lerp(float3(0.92, 0.94, 0.97), float3(0.93, 0.91, 0.86), style_info.x), snow);

    if (paint > 0.0) {
        // Snow cover - the open snow of the winter palette (a bright material) and snow on high ground - carries
        // the full strokes: crests a little lighter and warmer, troughs darker and cooler. Bare ground of the other
        // seasons keeps a lighter brush texture, rock its own. Then the broad colour variations.
        const float bright = smoothstep(0.45, 0.70, dot(color, float3(0.2126, 0.7152, 0.0722)));
        const float cover = max(style_info.x * bright, snow);
        const float height = max(raw - map_info.w, 0.0) * map_info.z;
        float2 warp;
        const float s = snow_strokes(texf, height, footprint, warp);
        const float amp = paint * lerp(0.25, 1.0, cover) * (1.0 - 0.7 * rock);
        color *= (1.0 + s * 0.32 * amp) * (1.0 + s * cover * amp * float3(0.05, 0.0, -0.07));
        color *= lerp(float3(1.0, 1.0, 1.0), macro_variation(warp), paint);
    }

    // Political layer: palette colour per province; weaker when close so the landscape shows through.
    const uint prov = provinces.Load(int3(texel, 0));
    const uint hovered = (uint)(select_info.x + 0.5);
    const uint selected = (uint)(select_info.y + 0.5);
    if (select_info.z > 0.5 && prov != 0) {
        const float strength = lerp(0.35, 0.78, eye_zoom.w);
        color = lerp(color, palette_color(prov), strength);
    }

    // Lighting: warm sun with relief shadows plus a sky/ground ambient.
    const float diffuse = saturate(dot(n, sun_dir.xyz));
    float shadow = diffuse > 0.0 ? sun_shadow(float3(input.world.xy, input.world.z + 0.2)) : 0.0;
    if (coc > 0.0) {
        shadow = lerp(shadow, 1.0, 0.35 * coc);  // behind the focus the relief shading flattens
    }
    const float3 ground_light = lerp(float3(0.30, 0.28, 0.25), float3(0.38, 0.36, 0.33), style_info.x);  // snow bounce
    const float3 sky_light = lerp(float3(0.40, 0.47, 0.58), float3(0.46, 0.47, 0.50), style_info.x);      // overcast, not blue
    const float3 ambient = lerp(ground_light, sky_light, n.z * 0.5 + 0.5) * 0.9;
    color *= ambient * ambient_mul.rgb + sun_color.rgb * (diffuse * shadow * 0.95);
    if (coc > 0.0) {
        color = lerp(dot(color, float3(0.2126, 0.7152, 0.0722)).xxx, color, 1.0 - 0.2 * coc);  // and colours fade
    }

    // Highlights.
    // (With painted borders the selection is drawn by the border layer below, the hover is a plain lift.)
    const bool painted = style_info.w > 0.5;
    if (prov != 0 && prov == selected && !painted) {
        color = lerp(color, float3(1.0, 0.93, 0.62), 0.32);
    } else if (prov != 0 && prov == hovered && prov != selected) {
        color = lerp(color, float3(1.0, 1.0, 1.0), 0.16);
    }

    if (painted) {
        if (prov == 0) {
            return finish(color, input.world, input.pos.xy);
        }
        const float scale = clamp(view_info.y * (1.0 / 1080.0), 0.75, 4.0);
        const float stride = ceil(max(1.0, footprint));
        const bool has_selection = selected != 0;
        float own_distance;
        float selected_distance;
        border_distances(texf, size, prov, selected, stride, dtex_dx, dtex_dy, own_distance, selected_distance);
        float inner_distance = selected_distance;
        if (has_selection && prov == selected) {
            // Coarser grid (about 3 px per cell) for the inner line, which lies deeper than the fine one sees.
            float ignored;
            border_distances(texf, size, prov, selected, ceil(max(stride, 3.0 * scale * footprint)), dtex_dx, dtex_dy,
                             ignored, inner_distance);
        }
        return finish(color, input.world, input.pos.xy,
                      border_overlay(own_distance, selected_distance, inner_distance, has_selection, scale));
    }

    // Province borders with constant width in screen pixels: the sample offset follows the texel footprint.
    if (prov != 0) {
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
    return finish(color, input.world, input.pos.xy);
}

// Sky behind the map (E5-A5, docs/dev/map-art-direction.md section 9): a fullscreen triangle drawn after the
// terrain at the far plane, so only the pixels the landscape did not cover are shaded. The colour depends on
// the elevation of the view ray: the horizon (and everything below it, i.e. the ground beyond the map edge) is
// the haze colour of the terrain, the sky above it turns slightly lighter, and on the sunset side a narrow band
// of gold clouds lies along the horizon. The result goes through the same grading and vignette as the terrain.
cbuffer SkyConstants : register(b2) {
    row_major float4x4 inv_view_proj;
    float4 sky_high;  // rgb = sky above the horizon (before grading)
    float4 sky_gold;  // rgb = gold clouds (before grading), a = strength 0..1
};

struct SkyOut {
    float4 pos : SV_Position;
    float2 ndc : TEXCOORD0;
};

SkyOut vs_sky(uint id : SV_VertexID) {
    const float2 corner = float2((id << 1) & 2, id & 2);  // (0,0) (2,0) (0,2): one triangle covers the screen
    SkyOut o;
    o.ndc = corner * 2.0 - 1.0;
    o.pos = float4(o.ndc, 1.0, 1.0);
    return o;
}

float4 ps_sky(SkyOut input) : SV_Target {
    const float4 far_point = mul(inv_view_proj, float4(input.ndc, 1.0, 1.0));
    const float3 dir = normalize(far_point.xyz / far_point.w - eye_zoom.xyz);
    const float elevation = degrees(asin(clamp(dir.z, -1.0, 1.0)));  // above the horizon, negative below

    // Angle between the view ray and the sun around the vertical axis; +-pi (the noise seam) is opposite the sun.
    const float2 flat_dir = dir.xy / max(length(dir.xy), 1e-4);
    const float2 flat_sun = sun_dir.xy / max(length(sun_dir.xy), 1e-4);
    const float rel = atan2(flat_dir.x * flat_sun.y - flat_dir.y * flat_sun.x, dot(flat_dir, flat_sun));
    const float side = smoothstep(-0.35, 1.0, cos(rel));  // 1 towards the sun, 0 away from it

    float3 c = lerp(scene_haze(), sky_high.rgb, smoothstep(0.0, 22.0, elevation));

    // Gold clouds: a band along the horizon broken into long streaks (noise stretched along the horizon).
    const float band = exp(-pow((elevation - 1.5) / (elevation < 1.5 ? 8.0 : 4.0), 2.0));  // narrow above, soft below
    const float streaks = 0.55 + 0.45 * (value_noise(float2(rel * 9.0, elevation * 0.45)) * 0.65 +
                                          value_noise(float2(rel * 23.0, elevation * 1.1)) * 0.35);
    c = lerp(c, sky_gold.rgb, saturate(sky_gold.a * side * band * streaks));

    // A little noise hides the 8-bit banding of the very soft gradient.
    c += (hash21(input.pos.xy) - 0.5) * (1.0 / 255.0);
    return float4(saturate(apply_style(c, input.pos.xy)), 1.0);
}
