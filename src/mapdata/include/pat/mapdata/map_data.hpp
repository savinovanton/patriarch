#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// Map data shared by the offline generator (tools/mapgen) and the game (ADR-0003, E3-T03).
//
// The map is a set of equally sized raster layers (row-major, top row first, one entry per pixel):
//   heights    u16  elevation; the waterline is exactly `sea_level` (land iff height > sea_level)
//   provinces  u16  0 = water / no province, 1..N = province id
//   materials  u8   surface material (Material enum), used to texture the terrain
// plus a table with one record per province (colour, centroid, area, neighbours with border lengths).
// Normals are derived data: see compute_normals() in normals.hpp.

namespace pat::mapdata {

/// Surface materials (index into the terrain material texture array).
enum class Material : std::uint8_t {
    DeepWater = 0,
    ShallowWater,
    Beach,
    Plains,
    Steppe,
    Forest,
    Hills,
    Rock,
    Snow,
    Tundra,
    Count
};

inline constexpr std::size_t kMaterialCount = static_cast<std::size_t>(Material::Count);

/// Shared border between two provinces.
struct Border {
    std::uint16_t neighbor = 0;   ///< Province id of the other side.
    std::uint32_t length_px = 0;  ///< Number of pixel edges the two provinces share.
    [[nodiscard]] bool operator==(const Border&) const = default;
};

struct Province {
    std::uint16_t id = 0;         ///< 1-based; equals the index in MapData::province_list plus one.
    std::uint32_t color_rgb = 0;  ///< 0xRRGGBB, unique per province, used for the political map.
    std::uint32_t area_px = 0;
    std::uint16_t centroid_x = 0;
    std::uint16_t centroid_y = 0;
    std::uint16_t min_x = 0;
    std::uint16_t min_y = 0;
    std::uint16_t max_x = 0;  ///< Inclusive bounding box.
    std::uint16_t max_y = 0;
    std::uint16_t mean_height = 0;
    bool coastal = false;           ///< Touches water.
    std::vector<Border> neighbors;  ///< Sorted by neighbor id.
    [[nodiscard]] bool operator==(const Province&) const = default;
};

struct MapData {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint16_t sea_level = 0;
    /// Vertical scale: metres of elevation per height unit, times 1000 (integer, so files are exact).
    std::uint32_t metres_per_unit_milli = 0;
    std::uint64_t seed = 0;  ///< Generator seed (informational).

    std::vector<std::uint16_t> heights;
    std::vector<std::uint16_t> provinces;
    std::vector<std::uint8_t> materials;
    std::vector<Province> province_list;

    [[nodiscard]] std::size_t pixel_count() const noexcept {
        return static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    }
    [[nodiscard]] std::size_t index(std::uint32_t x, std::uint32_t y) const noexcept {
        return static_cast<std::size_t>(y) * width + x;
    }
    [[nodiscard]] bool is_land(std::size_t pixel) const noexcept { return heights[pixel] > sea_level; }
    [[nodiscard]] const Province* province(std::uint16_t id) const noexcept {
        return (id == 0 || id > province_list.size()) ? nullptr
                                                      : &province_list[static_cast<std::size_t>(id) - 1];
    }
    /// Elevation above the waterline in metres (negative under water).
    [[nodiscard]] double elevation_m(std::uint16_t height_value) const noexcept {
        return (static_cast<double>(height_value) - static_cast<double>(sea_level)) *
               static_cast<double>(metres_per_unit_milli) / 1000.0;
    }
    [[nodiscard]] bool operator==(const MapData&) const = default;
};

}  // namespace pat::mapdata
