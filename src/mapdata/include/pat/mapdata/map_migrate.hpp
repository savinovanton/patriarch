#pragma once

#include "pat/core/result.hpp"
#include "pat/mapdata/map_data.hpp"
#include "pat/mapdata/physical_layer.hpp"
#include "pat/mapdata/province_layer.hpp"

#include <filesystem>

// PMD1 -> PMD2 migration and the format-detecting map loader (ADR-0007 section 5, Geo-13).
//
// migrate_pmd1 is a pure function: the same MapData always gives the same layers, and
// build_map_data(physical, provinces) gives the original MapData back bit for bit. PMD1 itself is
// still read by read_map_file / deserialize_map, unchanged.

namespace pat::mapdata {

/// Error codes of migration and loading (container 100..118, physical 120..136, provinces 140..152,
/// PMD1 20..45).
namespace migrate_error {
inline constexpr int kInvalidSource = 160;    ///< The PMD1 map fails validate_map (nothing is migrated).
inline constexpr int kTooManyWaters = 161;    ///< More than 65535 separate water bodies.
inline constexpr int kUnknownFormat = 162;    ///< The file starts with neither "PMD1" nor "PMD2".
inline constexpr int kMissingSibling = 163;   ///< physical.pmd / provinces.pmd of the set is not next to it.
inline constexpr int kUnsupportedKind = 164;  ///< A PMD2 file that is neither physical nor provinces.
inline constexpr int kIo = 165;               ///< The file or directory cannot be opened, read or created.
}  // namespace migrate_error

inline constexpr const char* kPhysicalFileName = "physical.pmd";
inline constexpr const char* kProvincesFileName = "provinces.pmd";
inline constexpr const char* kMigrationStepId = "migrated-from-pmd1";

struct MigratedMap {
    PhysicalLayer physical;
    ProvinceLayer provinces;
};

/// Splits a valid PMD1 map into a physical and a provinces layer:
///  - water is every pixel with height <= sea_level, one `sea` body per 4-connected component
///    (key "sea", then "sea-2", "sea-3", ... in raster order of the first pixel; a connected body of
///    water gives exactly the single entry of the ADR), surface_level = sea_level;
///  - GEOR is the "none" georeference, PROV records the step "migrated-from-pmd1" whose params_hash
///    is FNV-1a64 of the four PMD1 section hashes;
///  - provinces copy PIDX and the table; keys are "p" + id padded with zeros to four digits;
///    bind_key is the coast key of the physical layer.
/// A map that violates validate_map (in particular "water <=> height <= sea_level") is an error:
/// migrate_error::kInvalidSource, never a silently altered map.
[[nodiscard]] core::Result<MigratedMap, core::Error> migrate_pmd1(const MapData& map);

/// Writes `physical.pmd` and `provinces.pmd` of a migrated map into `directory` (created if needed).
[[nodiscard]] core::Result<void, core::Error> write_map_set(const std::filesystem::path& directory,
                                                            const MigratedMap& layers,
                                                            const Pmd2WriterOptions& options = {});

/// Loads a map of either format, detected by the first bytes of the file:
///  - "PMD1": read_map_file, exactly as before;
///  - "PMD2": the file is one of a set (physical.pmd + provinces.pmd) and the other file is taken from
///    the same directory under its fixed name; the pair is checked like load_map_set.
[[nodiscard]] core::Result<MapData, core::Error> load_map(const std::filesystem::path& path,
                                                          const ProvinceReadOptions& options = {});

}  // namespace pat::mapdata
