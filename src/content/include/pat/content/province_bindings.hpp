#pragma once

#include "pat/core/result.hpp"
#include "pat/data/content_loader.hpp"
#include "pat/mapdata/map_data.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace pat::content {

/// A stable key and its raster/palette id. Zero is water and has no definition.
struct ProvinceMapKey {
    std::uint16_t map_id = 0;
    std::string key;
};

/// A definition in ContentRegistry::ids(), not a sim::ProvinceId. The scenario creates World rows
/// and keeps the map -> World correspondence separately (E4-T10, owner decision #201).
struct ProvinceBinding {
    std::uint16_t map_id = 0;
    core::StringId definition_id;
    [[nodiscard]] bool operator==(const ProvinceBinding&) const = default;
};

using ProvinceBindingsResult = core::Result<std::vector<ProvinceBinding>, std::vector<data::LoadError>>;

/// Checks a bijection: every map key has exactly one province definition and vice versa. Input map
/// ids and keys must be nonzero/nonempty and unique. Returns bindings sorted by numeric map id,
/// independent of input order. The registry must come from a successful load.
[[nodiscard]] ProvinceBindingsResult bind_provinces(const data::ContentRegistry& registry,
                                                    std::span<const ProvinceMapKey> keys);

/// Adapter for an already decoded PMD1 map: its dense ids 1..N produce keys p0001..pNNNN.
/// Validates the table id ordering; geometry validation remains mapdata::validate_map's job.
/// No map, save format or World state is changed.
[[nodiscard]] ProvinceBindingsResult bind_pmd1_provinces(const data::ContentRegistry& registry,
                                                         const mapdata::MapData& map);

}  // namespace pat::content
