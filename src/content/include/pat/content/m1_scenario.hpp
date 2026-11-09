#pragma once

#include "pat/core/result.hpp"
#include "pat/data/content_registry.hpp"
#include "pat/mapdata/map_gen.hpp"
#include "pat/sim/world.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace pat::content {

struct M1Scenario {
    sim::World world;
    std::uint64_t replay_id;
    std::uint64_t content_hash;
    // Both arrays use the raster id. Entry 0 is water.
    std::vector<sim::ProvinceId> map_to_world;
    std::vector<std::string> province_names;
    // World name ids refer to this registry's interner.
    data::ContentRegistry registry;
};

[[nodiscard]] mapdata::GenParams m1_map_params();

/// Loads the procedural M1 definitions and Russian labels, validates the map, then builds the
/// initial world. Gameplay systems and historical scenario setup are installed separately.
[[nodiscard]] core::Result<M1Scenario>
load_m1_scenario(const std::filesystem::path& root, const mapdata::MapData& map, std::uint64_t seed);

}  // namespace pat::content
