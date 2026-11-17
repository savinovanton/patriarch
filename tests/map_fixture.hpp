#pragma once

// Shared test terrain for the physical-layer and province-layer tests (header-only).

#include "pat/mapdata/physical_layer.hpp"

#include <cstddef>
#include <cstdint>

namespace pat::test {

/// Sea level of make_test_terrain(), in layer height units.
inline constexpr std::uint16_t kTestSeaLevel = 1000;

/// A 64x48 physical layer without georeference and provenance (the caller sets those): a sea band on
/// top (water body 1, rows 0..5), a lake above sea level (water body 2, x 20..29, y 20..27) and
/// sloped land elsewhere. With `varied_lake_bed` the lake bed alternates between 2400 and 2500
/// (flat bed up to the surface is allowed), otherwise it is flat at 2400.
[[nodiscard]] inline mapdata::PhysicalLayer make_test_terrain(bool varied_lake_bed) {
    using mapdata::Material;
    using mapdata::PhysicalLayer;
    using mapdata::WaterKind;
    PhysicalLayer l;
    l.width = 64;
    l.height = 48;
    l.sea_level = kTestSeaLevel;
    l.metres_per_unit_milli = 250;
    l.seed = 77;
    const std::size_t n = l.pixel_count();
    l.heights.assign(n, 0);
    l.water_body.assign(n, 0);
    l.materials.assign(n, static_cast<std::uint8_t>(Material::Plains));
    for (std::uint32_t y = 0; y < l.height; ++y) {
        for (std::uint32_t x = 0; x < l.width; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * l.width + x;
            if (y < 6) {
                l.water_body[i] = 1;
                l.heights[i] = static_cast<std::uint16_t>(400 + x);
                l.materials[i] = static_cast<std::uint8_t>(Material::DeepWater);
            } else if (x >= 20 && x < 30 && y >= 20 && y < 28) {
                l.water_body[i] = 2;
                l.heights[i] = varied_lake_bed && (x + y) % 3 == 0 ? 2500 : 2400;
                l.materials[i] = static_cast<std::uint8_t>(Material::ShallowWater);
            } else {
                l.heights[i] = static_cast<std::uint16_t>(2000 + (x * 13U + y * 7U) % 900U);
            }
        }
    }
    const auto area = mapdata::count_water_area(l.water_body, 2);
    l.water_bodies.push_back({1, WaterKind::sea, 0, kTestSeaLevel, area[0], "sea"});
    l.water_bodies.push_back({2, WaterKind::lake, 0, 2500, area[1], "lake-test"});
    return l;
}

}  // namespace pat::test
