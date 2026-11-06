#include "pat/engine/terrain.hpp"

#include <algorithm>
#include <cmath>

namespace pat::engine {

TerrainGeometry TerrainGeometry::from_map(const mapdata::MapData& map, float relief_fraction) {
    TerrainGeometry g;
    g.width = static_cast<float>(map.width);
    g.height = static_cast<float>(map.height);
    g.sea_level = map.sea_level;
    g.relief = relief_fraction * g.width;
    g.root_size = g.height;
    g.max_level = 0;
    while (g.node_size(g.max_level + 1) >= static_cast<float>(g.patch_cells) && g.max_level < 12) {
        ++g.max_level;
    }
    return g;
}

int TerrainGeometry::root_count_x() const noexcept {
    return std::max(1, static_cast<int>(std::ceil(width / root_size)));
}

// ------------------------------------------------------------------ frustum

Frustum Frustum::from_view_proj(const Mat4& m) noexcept {
    const auto row = [&](std::size_t r) { return Vec4{m.m[r][0], m.m[r][1], m.m[r][2], m.m[r][3]}; };
    const Vec4 r0 = row(0);
    const Vec4 r1 = row(1);
    const Vec4 r2 = row(2);
    const Vec4 r3 = row(3);
    const auto add = [](Vec4 a, Vec4 b) { return Vec4{a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w}; };
    const auto sub = [](Vec4 a, Vec4 b) { return Vec4{a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w}; };
    Frustum f;
    f.planes[0] = add(r3, r0);  // left
    f.planes[1] = sub(r3, r0);  // right
    f.planes[2] = add(r3, r1);  // bottom
    f.planes[3] = sub(r3, r1);  // top
    f.planes[4] = r2;           // near (depth range 0..1)
    f.planes[5] = sub(r3, r2);  // far
    return f;
}

bool Frustum::intersects_aabb(Vec3 lo, Vec3 hi) const noexcept {
    for (const Vec4& p : planes) {
        // The corner furthest along the plane normal decides whether the box can be inside.
        const float x = p.x >= 0.0f ? hi.x : lo.x;
        const float y = p.y >= 0.0f ? hi.y : lo.y;
        const float z = p.z >= 0.0f ? hi.z : lo.z;
        if (p.x * x + p.y * y + p.z * z + p.w < 0.0f) {
            return false;
        }
    }
    return true;
}

// ------------------------------------------------------------------ LOD selection

namespace {

float distance_to_box(Vec3 p, Vec3 lo, Vec3 hi) {
    const float dx = std::max({lo.x - p.x, 0.0f, p.x - hi.x});
    const float dy = std::max({lo.y - p.y, 0.0f, p.y - hi.y});
    const float dz = std::max({lo.z - p.z, 0.0f, p.z - hi.z});
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

}  // namespace

void select_terrain_nodes(const TerrainGeometry& g,
                          Vec3 eye,
                          const Frustum* frustum,
                          const LodSettings& lod,
                          std::vector<TerrainNode>& out) {
    out.clear();
    std::vector<TerrainNode> stack;
    for (int i = g.root_count_x() - 1; i >= 0; --i) {
        stack.push_back({static_cast<float>(i) * g.root_size, 0.0f, g.root_size, 0});
    }
    while (!stack.empty()) {
        const TerrainNode n = stack.back();
        stack.pop_back();
        if (n.x >= g.width || n.y >= g.height) {
            continue;  // Entirely outside the map.
        }
        const Vec3 lo{n.x, n.y, 0.0f};
        const Vec3 hi{n.x + n.size, n.y + n.size, g.relief};
        if (frustum != nullptr && !frustum->intersects_aabb(lo, hi)) {
            continue;
        }
        if (n.level < g.max_level && distance_to_box(eye, lo, hi) < lod_range(g, n.level, lod)) {
            const float h = n.size * 0.5f;
            const int next = n.level + 1;
            // Push in reverse so children come out in reading order.
            stack.push_back({n.x + h, n.y + h, h, next});
            stack.push_back({n.x, n.y + h, h, next});
            stack.push_back({n.x + h, n.y, h, next});
            stack.push_back({n.x, n.y, h, next});
        } else {
            out.push_back(n);
        }
    }
}

// ------------------------------------------------------------------ picking

HeightfieldPicker::HeightfieldPicker(const mapdata::MapData& map,
                                     const TerrainGeometry& geometry,
                                     int block_size)
    : map_(&map), geometry_(geometry), block_(std::max(1, block_size)) {
    const int w = static_cast<int>(map.width);
    const int h = static_cast<int>(map.height);
    blocks_x_ = (w + block_ - 1) / block_;
    blocks_y_ = (h + block_ - 1) / block_;
    block_max_.assign(static_cast<std::size_t>(blocks_x_) * static_cast<std::size_t>(blocks_y_), 0.0f);
    for (int by = 0; by < blocks_y_; ++by) {
        for (int bx = 0; bx < blocks_x_; ++bx) {
            int best = 0;
            const int y0 = std::max(0, by * block_ - 1);
            const int y1 = std::min(h - 1, (by + 1) * block_);
            const int x0 = std::max(0, bx * block_ - 1);
            const int x1 = std::min(w - 1, (bx + 1) * block_);
            for (int y = y0; y <= y1; ++y) {
                for (int x = x0; x <= x1; ++x) {
                    best = std::max<int>(
                        best,
                        map.heights[static_cast<std::size_t>(y) * map.width + static_cast<std::size_t>(x)]);
                }
            }
            block_max_[static_cast<std::size_t>(by) * static_cast<std::size_t>(blocks_x_) +
                       static_cast<std::size_t>(bx)] = geometry_.world_z(static_cast<float>(best));
        }
    }
}

float HeightfieldPicker::raw_height(int x, int y) const noexcept {
    x = std::clamp(x, 0, static_cast<int>(map_->width) - 1);
    y = std::clamp(y, 0, static_cast<int>(map_->height) - 1);
    return static_cast<float>(
        map_->heights[static_cast<std::size_t>(y) * map_->width + static_cast<std::size_t>(x)]);
}

float HeightfieldPicker::block_max_z(int bx, int by) const noexcept {
    return block_max_[static_cast<std::size_t>(by) * static_cast<std::size_t>(blocks_x_) +
                      static_cast<std::size_t>(bx)];
}

float HeightfieldPicker::height_at(float x, float y) const noexcept {
    // Texel centres sit at +0.5 (hardware bilinear convention).
    const float fx = x - 0.5f;
    const float fy = y - 0.5f;
    const float x0f = std::floor(fx);
    const float y0f = std::floor(fy);
    const int x0 = static_cast<int>(x0f);
    const int y0 = static_cast<int>(y0f);
    const float tx = fx - x0f;
    const float ty = fy - y0f;
    const float a = raw_height(x0, y0) * (1.0f - tx) + raw_height(x0 + 1, y0) * tx;
    const float b = raw_height(x0, y0 + 1) * (1.0f - tx) + raw_height(x0 + 1, y0 + 1) * tx;
    return geometry_.world_z(a * (1.0f - ty) + b * ty);
}

std::uint16_t HeightfieldPicker::province_at(float x, float y) const noexcept {
    if (x < 0.0f || y < 0.0f || x >= geometry_.width || y >= geometry_.height) {
        return 0;
    }
    const auto ix = static_cast<std::uint32_t>(x);
    const auto iy = static_cast<std::uint32_t>(y);
    return map_->provinces[map_->index(ix, iy)];
}

HeightfieldPicker::Hit HeightfieldPicker::raycast(Vec3 o, Vec3 d, float max_distance) const noexcept {
    const float len = length(d);
    if (len < 1e-12f) {
        return {};
    }
    d = d * (1.0f / len);

    // Clip the ray to the map's bounding box with the slab method.
    // The water plane is z = 0: let the ray reach slightly below it so rounding cannot make it miss.
    const float lo[3] = {0.0f, 0.0f, -0.05f};
    const float hi[3] = {geometry_.width, geometry_.height, geometry_.relief};
    const float org[3] = {o.x, o.y, o.z};
    const float dir[3] = {d.x, d.y, d.z};
    float t_enter = 0.0f;
    float t_exit = max_distance;
    for (int a = 0; a < 3; ++a) {
        if (std::fabs(dir[a]) < 1e-12f) {
            if (org[a] < lo[a] || org[a] > hi[a]) {
                return {};
            }
            continue;
        }
        float t0 = (lo[a] - org[a]) / dir[a];
        float t1 = (hi[a] - org[a]) / dir[a];
        if (t0 > t1) {
            std::swap(t0, t1);
        }
        t_enter = std::max(t_enter, t0);
        t_exit = std::min(t_exit, t1);
        if (t_enter > t_exit) {
            return {};
        }
    }

    const auto point_at = [&](float t) { return Vec3{o.x + d.x * t, o.y + d.y * t, o.z + d.z * t}; };
    const auto below = [&](float t) {
        const Vec3 p = point_at(t);
        return p.z <= height_at(p.x, p.y);
    };
    const auto refine = [&](float t_above, float t_below) {
        for (int i = 0; i < 24; ++i) {
            const float mid = 0.5f * (t_above + t_below);
            (below(mid) ? t_below : t_above) = mid;
        }
        return Hit{true, point_at(t_below)};
    };

    if (below(t_enter)) {
        return {true, point_at(t_enter)};
    }

    // Walk the grid of blocks (2D DDA); inside blocks that the ray may touch, march in half-texel steps.
    const Vec3 start = point_at(t_enter);
    int bx = std::clamp(static_cast<int>(std::floor(start.x / static_cast<float>(block_))), 0, blocks_x_ - 1);
    int by = std::clamp(static_cast<int>(std::floor(start.y / static_cast<float>(block_))), 0, blocks_y_ - 1);
    const int step_x = d.x > 0.0f ? 1 : -1;
    const int step_y = d.y > 0.0f ? 1 : -1;
    const float inf = 1e30f;
    const float cell = static_cast<float>(block_);
    const auto next_boundary = [&](int b, int step) {
        return (static_cast<float>(b) + (step > 0 ? 1.0f : 0.0f)) * cell;
    };
    float t_max_x = std::fabs(d.x) < 1e-12f ? inf : (next_boundary(bx, step_x) - o.x) / d.x;
    float t_max_y = std::fabs(d.y) < 1e-12f ? inf : (next_boundary(by, step_y) - o.y) / d.y;
    const float t_delta_x = std::fabs(d.x) < 1e-12f ? inf : cell / std::fabs(d.x);
    const float t_delta_y = std::fabs(d.y) < 1e-12f ? inf : cell / std::fabs(d.y);

    float t = t_enter;
    float t_prev_above = t_enter;
    while (t < t_exit && bx >= 0 && by >= 0 && bx < blocks_x_ && by < blocks_y_) {
        const float t_cell_end = std::min({t_max_x, t_max_y, t_exit});
        const float z_a = o.z + d.z * t;
        const float z_b = o.z + d.z * t_cell_end;
        if (std::min(z_a, z_b) <= block_max_z(bx, by)) {
            // The ray may dip into this block's terrain: march it.
            float ts = t;
            while (ts < t_cell_end) {
                const float tn = std::min(ts + 0.5f, t_cell_end);
                if (below(tn)) {
                    return refine(t_prev_above, tn);
                }
                t_prev_above = tn;
                ts = tn;
            }
        } else {
            t_prev_above = t_cell_end;
        }
        t = t_cell_end;
        if (t_max_x < t_max_y) {
            bx += step_x;
            t_max_x += t_delta_x;
        } else {
            by += step_y;
            t_max_y += t_delta_y;
        }
    }
    return {};
}

}  // namespace pat::engine
