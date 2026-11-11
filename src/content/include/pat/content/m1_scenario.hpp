#pragma once

#include "pat/content/m1_content.hpp"
#include "pat/content/province_bindings.hpp"
#include "pat/core/result.hpp"
#include "pat/data/content_registry.hpp"
#include "pat/data/content_source.hpp"
#include "pat/loc/localizer.hpp"
#include "pat/mapdata/map_gen.hpp"
#include "pat/sim/demography.hpp"
#include "pat/sim/world.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace pat::content {

struct M1Definitions {
    data::ContentRegistry registry;
    std::vector<ProvinceBinding> bindings;
    // Indexed by raster id. Entry 0 is water.
    std::vector<std::string> province_names;
    loc::Localizer localizer;
    sim::DemographyRules demography;
    std::vector<HonorRank> honor_ranks;
};

struct M1Scenario {
    sim::World world;
    std::uint64_t replay_id;
    std::uint64_t content_hash;  // Simulation compatibility hash, excluding presentation-only ranks.
    // Both arrays use the raster id. Entry 0 is water.
    std::vector<sim::ProvinceId> map_to_world;
    std::vector<std::string> province_names;
    // World name ids refer to this registry's interner.
    data::ContentRegistry registry;
    sim::DemographyRules demography;
    std::vector<HonorRank> honor_ranks;
};

[[nodiscard]] mapdata::GenParams m1_map_params();

/// Loads and validates M1 definitions and Russian labels against the PMD1 province table.
/// The caller validates the geometry when preparing the map.
[[nodiscard]] core::Result<M1Definitions> load_m1_definitions(const data::ContentSource& source,
                                                              const mapdata::MapData& map);

/// Loads the procedural M1 definitions and Russian labels, validates the map, then builds the
/// initial world. Gameplay systems and historical scenario setup are installed separately.
[[nodiscard]] core::Result<M1Scenario>
load_m1_scenario(const std::filesystem::path& root, const mapdata::MapData& map, std::uint64_t seed);

}  // namespace pat::content
