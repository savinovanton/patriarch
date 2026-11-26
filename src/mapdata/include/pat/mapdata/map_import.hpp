#pragma once

#include "pat/core/result.hpp"
#include "pat/mapdata/map_migrate.hpp"
#include "pat/mapdata/physical_layer.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// Stage 2 of the geodata pipeline (Geo-3, ADR-0004 section 3): `mapgen import` turns the stage-1
// files of tools/geo/stage1.py (docs/dev/geodata-pipeline.md section 6, format v1) into a PMD2 map
// set: physical.pmd (heights, water bodies, materials, GEOR, PROV) and provinces.pmd (placeholder
// provinces until real borders are digitised). No GDAL: the inputs are plain rasters and JSON.
//
// Offline tooling (like map_gen): floating point and threads are allowed; the output is the same
// bit for bit for the same input and parameters, whatever the number of threads.

namespace pat::mapdata {

/// Error codes of the import (container 100..118, physical 120..136, provinces 140..152, migration
/// 160..165, nature 170..180).
namespace import_error {
inline constexpr int kIo = 190;                ///< A stage-1 file cannot be read.
inline constexpr int kBadDescriptor = 191;     ///< stage1.json is not valid JSON or misses/garbles fields.
inline constexpr int kRasterSize = 192;        ///< A raster does not have width x height samples.
inline constexpr int kBadWaterCode = 193;      ///< water.u8 holds a code other than 0, 1, 2.
inline constexpr int kTooManyWaters = 194;     ///< More than 65535 water bodies.
inline constexpr int kTooManyProvinces = 195;  ///< More than 65535 provinces (islands included).
inline constexpr int kNoLand = 196;            ///< The water mask has no land at all.
}  // namespace import_error

inline constexpr const char* kStage1DescriptorName = "stage1.json";
inline constexpr int kStage1FormatVersion = 1;
inline constexpr std::int16_t kStage1NoData = -32768;

/// Vertical encoding of imported maps [предл.]: 0.15 m per unit, zero metres at 8192, so heights span
/// -1228.8 .. +8601 m (seas get a synthetic bed below zero, see ImportReport).
inline constexpr std::uint16_t kImportSeaLevel = 8192;
inline constexpr std::uint32_t kImportMetresPerUnitMilli = 150;

/// Water codes of water.u8.
enum class Stage1Water : std::uint8_t { land = 0, sea = 1, lake = 2 };

/// A parsed stage-1 directory: the descriptor fields that stage 2 uses and the two rasters.
struct Stage1Data {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::string projection_id;
    GeoRef georef;
    Provenance provenance;               ///< builder left empty; sha256 null in the JSON becomes zeros.
    std::size_t unverified_files = 0;    ///< Source files listed without a SHA-256.
    std::string height_file;             ///< files.height.path (a plain name in the directory).
    std::string water_file;              ///< files.water.path.
    std::vector<std::int16_t> height_m;  ///< Metres above EGM2008, kStage1NoData where unknown.
    std::vector<std::uint8_t> water;     ///< Stage1Water codes.
};

/// Parses stage1.json (fields only: the rasters are left empty).
[[nodiscard]] core::Result<Stage1Data, core::Error> parse_stage1_descriptor(std::string_view json);

/// Reads `<dir>/stage1.json` and the height and water rasters it names.
[[nodiscard]] core::Result<Stage1Data, core::Error> read_stage1(const std::filesystem::path& dir);

struct ImportParams {
    std::uint32_t province_count = 400;  ///< Placeholder provinces on land (islands get one each).
    std::uint64_t seed = 1;              ///< Seed of the placeholder province split.
    unsigned threads = 0;                ///< 0 = hardware concurrency; the output does not depend on it.
};

/// What the import had to make up or clamp; printed by mapgen and recorded in the docs.
struct ImportReport {
    std::size_t land_px = 0;
    std::size_t nodata_filled_px = 0;       ///< Land without DEM data, filled from the nearest known land.
    std::size_t clamped_px = 0;             ///< Heights outside the representable range.
    std::size_t seas = 0;                   ///< Sea bodies (4-connected components of code 1).
    std::size_t lakes = 0;                  ///< Lake bodies (components of code 2).
    std::size_t placeholder_provinces = 0;  ///< From the seeded split of the land.
    std::size_t island_provinces = 0;       ///< One per landmass the split did not reach.
};

struct ImportedMap {
    MigratedMap layers;
    ImportReport report;
};

/// Builds the layers (validate_physical and validate_provinces run before returning):
///  - HGHT: land heights from the DEM (nodata filled, land at or above 0 m kept above sea_level),
///    sea bed synthetic by the distance to the coast, lakes flat at the level of their lowest shore;
///  - WBOD/WTBL: one body per 4-connected component, seas ("sea", "sea-2", ...) at sea_level and
///    lakes ("lake", "lake-2", ...) in raster order; the Caspian is a sea (code 1 of stage 1);
///  - MATL: by depth, elevation, slope and latitude (inverse projection of GEOR);
///  - GEOR copied from the descriptor, PROV = stage-1 provenance + the step "import:stage2-v1";
///  - provinces: the generate_map split of the land (`province_count`) plus one province per island
///    it did not reach; keys "p0001", ...; PROV step "provinces:placeholder-v1".
[[nodiscard]] core::Result<ImportedMap, core::Error> import_stage1(const Stage1Data& input,
                                                                   const ImportParams& params = {});

}  // namespace pat::mapdata
