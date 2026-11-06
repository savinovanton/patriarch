#pragma once

#include "pat/engine/math.hpp"
#include "pat/mapdata/map_data.hpp"

#include <array>
#include <cstdint>
#include <vector>

// CPU side of the volumetric map (ADR-0003, E3-T04): terrain geometry parameters, CDLOD node
// selection with frustum culling, and ray picking against the height field. No graphics API here -
// everything is testable without a GPU.
//
// World units: one unit is one height-map texel horizontally. World z (up) of a height value h is
// max(h - sea_level, 0) / 65535 * relief; water is a flat plane at z = 0.

namespace pat::engine {

struct TerrainGeometry {
    float width = 0.0f;   ///< Map width in world units (= texels).
    float height = 0.0f;  ///< Map height in world units (= texels).
    float relief = 0.0f;  ///< World height of a full-scale (65535) height value above the waterline.
    std::uint16_t sea_level = 0;
    int patch_cells = 64;  ///< Quads per patch edge (every node is rendered with the same grid).
    float root_size =
        0.0f;           ///< Edge of a level-0 node; the map is covered by ceil(width / root_size) of them.
    int max_level = 0;  ///< Finest level; its nodes have about one vertex per texel.

    /// `relief_fraction` is the world height of a full-scale value as a fraction of the map width
    /// (a deliberate vertical exaggeration so mountains read at strategy-map distances).
    [[nodiscard]] static TerrainGeometry from_map(const mapdata::MapData& map, float relief_fraction = 0.03f);

    [[nodiscard]] float world_z(float height_value) const noexcept {
        const float above = height_value - static_cast<float>(sea_level);
        return above > 0.0f ? above / 65535.0f * relief : 0.0f;
    }
    [[nodiscard]] float node_size(int level) const noexcept {
        return root_size / static_cast<float>(1U << static_cast<unsigned>(level));
    }
    [[nodiscard]] int root_count_x() const noexcept;
};

struct TerrainNode {
    float x = 0.0f;  ///< World position of the node's north-west corner.
    float y = 0.0f;
    float size = 0.0f;
    int level = 0;  ///< 0 = coarsest.
};

struct LodSettings {
    /// A node is split while the camera is closer than `size * range_scale` to its bounding box.
    float range_scale = 2.5f;
};

/// Distance below which a node of `level` is split into its four children.
[[nodiscard]] inline float lod_range(const TerrainGeometry& g, int level, const LodSettings& s) noexcept {
    return g.node_size(level) * s.range_scale;
}

struct Frustum {
    std::array<Vec4, 6> planes{};  ///< (nx, ny, nz, d); a point is inside when n.p + d >= 0 for all planes.

    [[nodiscard]] static Frustum from_view_proj(const Mat4& view_proj) noexcept;
    [[nodiscard]] bool intersects_aabb(Vec3 lo, Vec3 hi) const noexcept;
};

/// Selects the nodes to draw: the quadtree is split around the camera and nodes outside the frustum
/// (when given) or outside the map are dropped. The selected nodes tile the visible part of the map
/// without gaps or overlaps. `out` is cleared first.
void select_terrain_nodes(const TerrainGeometry& geometry,
                          Vec3 eye,
                          const Frustum* frustum,
                          const LodSettings& lod,
                          std::vector<TerrainNode>& out);

/// Ray casts against the (sea clamped) height field; also answers province lookups.
class HeightfieldPicker {
public:
    struct Hit {
        bool hit = false;
        Vec3 point;
    };

    /// `map` must outlive the picker.
    HeightfieldPicker(const mapdata::MapData& map, const TerrainGeometry& geometry, int block_size = 16);

    /// Surface height in world units at (x, y), bilinearly interpolated like the GPU sampler does.
    [[nodiscard]] float height_at(float x, float y) const noexcept;
    /// Province id under world position (x, y); 0 for water or positions outside the map.
    [[nodiscard]] std::uint16_t province_at(float x, float y) const noexcept;
    /// First intersection of the ray with the terrain surface within [0, max_distance].
    [[nodiscard]] Hit raycast(Vec3 origin, Vec3 direction, float max_distance) const noexcept;

private:
    [[nodiscard]] float raw_height(int x, int y) const noexcept;
    [[nodiscard]] float block_max_z(int bx, int by) const noexcept;

    const mapdata::MapData* map_;
    TerrainGeometry geometry_;
    int block_;
    int blocks_x_;
    int blocks_y_;
    std::vector<float> block_max_;  ///< Max world z in each block, including the 1 texel bilinear apron.
};

}  // namespace pat::engine
