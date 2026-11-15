#pragma once

#include "pat/core/result.hpp"
#include "pat/mapdata/pmd2_container.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

// PMD2 physical layer (ADR-0007 section 3, Geo-11): the content of a `kind = 1` container on top of
// Pmd2Writer / Pmd2Reader. Chunks: GRID, GEOR, HGHT, WBOD, WTBL, MATL (critical) and PROV (optional).
// Everything is integer: no floating point in the file or in the checks. Water is defined by the WBOD
// raster (0 = land), not by comparing a height with sea_level.
//
// The PMD1 code (MapData, map_format) is not touched; migration is Geo-13.

namespace pat::mapdata {

/// Version of the Material enumeration stored in GRID and as the version of the MATL chunk.
inline constexpr std::uint16_t kMaterialSetVersion = 1;
inline constexpr std::size_t kMaxWaterKeyLength = 28;  ///< WTBL entry stays within 40 bytes.
inline constexpr std::size_t kMaxProjTextLength = 4096;
inline constexpr std::size_t kMaxProvStringLength = 1024;

/// Error codes of the physical layer (container errors are 100..118, PMD1 uses 20..45).
namespace physical_error {
inline constexpr int kBadDimensions = 120;    ///< Zero or too large width/height/pixel count.
inline constexpr int kLayerSize = 121;        ///< A raster does not match width x height.
inline constexpr int kBadGrid = 122;          ///< GRID: version, scale or material set invalid.
inline constexpr int kBadMaterial = 123;      ///< A material value is outside the enumeration.
inline constexpr int kBadWaterTable = 124;    ///< WTBL: id, kind, flags, key or level invalid.
inline constexpr int kBadWaterId = 125;       ///< WBOD holds an id that has no table entry.
inline constexpr int kWaterArea = 126;        ///< area_px differs from the WBOD raster.
inline constexpr int kBedAboveSurface = 127;  ///< HGHT above the water surface inside a water body.
inline constexpr int kWaterSplit = 128;       ///< A water body is not 4-connected.
inline constexpr int kSeaLevel = 129;         ///< A sea whose surface level differs from sea_level.
inline constexpr int kBadGeoref = 130;        ///< GEOR invalid or inconsistent with the dimensions.
inline constexpr int kBadProvenance = 131;    ///< PROV invalid.
inline constexpr int kBadChunk = 132;         ///< A chunk blob is truncated, malformed or has trailing bytes.
inline constexpr int kBindKey = 133;          ///< Header bind_key differs from the coast key of the content.
inline constexpr int kWrongKind = 134;        ///< The container is not a physical one.
inline constexpr int kGridHeader = 135;       ///< GRID width/height differ from the header.
inline constexpr int kMissingChunk = 136;     ///< A required chunk is absent.
}  // namespace physical_error

enum class WaterKind : std::uint8_t { sea = 1, lake = 2, inland_sea = 3 };

/// One water body (WTBL record). `id` is the dense number 1..N used in the WBOD raster.
struct WaterBody {
    std::uint16_t id = 0;
    WaterKind kind = WaterKind::sea;
    std::uint8_t flags = 0;  ///< Reserved, must be 0.
    /// Water surface in height units. For seas it equals sea_level; lakes and inland seas may differ.
    std::uint16_t surface_level = 0;
    std::uint32_t area_px = 0;
    std::string key;  ///< [a-z0-9-], unique, 1..kMaxWaterKeyLength characters.
    [[nodiscard]] bool operator==(const WaterBody&) const = default;
};

enum class Projection : std::uint8_t {
    none = 0,  ///< Procedural map: no geographic reference.
    albers = 1,
    lambert_conformal = 2,
    lambert_azimuthal = 3,
    equirectangular = 4,
};
enum class Datum : std::uint8_t { none = 0, wgs84 = 1 };

/// Georeference (GEOR chunk, ADR-0007 section 3.3). All integers: micro-degrees and millimetres.
/// A default-constructed value is the "none" georeference of procedural maps.
struct GeoRef {
    std::uint16_t version = 1;
    Projection projection = Projection::none;
    Datum datum = Datum::none;
    std::int32_t lat_origin_udeg = 0;
    std::int32_t lon_central_udeg = 0;
    std::int32_t lat_std1_udeg = 0;
    std::int32_t lat_std2_udeg = 0;
    std::int64_t false_easting_mm = 0;
    std::int64_t false_northing_mm = 0;
    std::int64_t origin_x_mm = 0;  ///< Top left corner of pixel (0, 0) in the projection plane.
    std::int64_t origin_y_mm = 0;
    std::int64_t pixel_size_x_mm = 0;  ///< > 0 for real projections, row 0 is north.
    std::int64_t pixel_size_y_mm = 0;
    std::int32_t bbox_min_lon_udeg = 0;
    std::int32_t bbox_max_lon_udeg = 0;
    std::int32_t bbox_min_lat_udeg = 0;
    std::int32_t bbox_max_lat_udeg = 0;
    std::uint32_t epsg = 0;  ///< 0 = custom projection.
    std::string proj_text;   ///< Informational PROJ/WKT text for external tools.
    [[nodiscard]] bool operator==(const GeoRef&) const = default;
};

struct ProvFile {
    std::string path;                       ///< Manifest-relative path, never an absolute or machine path.
    std::array<std::uint8_t, 32> sha256{};  ///< Computed by tools/geo; stored as data.
    [[nodiscard]] bool operator==(const ProvFile&) const = default;
};
struct ProvSource {
    std::string source_id;       ///< Id from the data-source manifest.
    std::string source_version;  ///< Version/date of the data set.
    std::vector<ProvFile> files;
    [[nodiscard]] bool operator==(const ProvSource&) const = default;
};
struct ProvStep {
    std::string step_id;  ///< "reproject:albers-v1", "resample:8192x4096", ...
    std::uint64_t params_hash = 0;
    [[nodiscard]] bool operator==(const ProvStep&) const = default;
};
struct ProvCorrection {
    std::string correction_id;
    std::uint64_t content_id = 0;
    [[nodiscard]] bool operator==(const ProvCorrection&) const = default;
};
/// Provenance (PROV chunk, ADR-0007 section 3.6): where the data came from and how it was processed.
struct Provenance {
    std::uint16_t provenance_version = 1;
    std::string builder;  ///< For example "mapgen <git sha12>"; no paths or machine names.
    std::uint32_t manifest_version = 0;
    std::vector<ProvSource> sources;
    std::vector<ProvStep> steps;
    std::vector<ProvCorrection> corrections;
    [[nodiscard]] bool operator==(const Provenance&) const = default;
};

/// In-memory physical layer of a map: grid, heights, water, materials, georeference, provenance.
struct PhysicalLayer {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint16_t sea_level = 0;  ///< Height of zero metres.
    std::uint32_t metres_per_unit_milli = 0;
    std::uint64_t seed = 0;  ///< Generator seed for procedural maps, else 0.
    std::uint16_t material_version = kMaterialSetVersion;

    std::vector<std::uint16_t> heights;
    std::vector<std::uint16_t> water_body;  ///< WBOD: 0 = land, k = water body k.
    std::vector<std::uint8_t> materials;
    std::vector<WaterBody> water_bodies;
    GeoRef georef;
    std::optional<Provenance> provenance;

    [[nodiscard]] std::size_t pixel_count() const noexcept {
        return static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    }
    [[nodiscard]] bool is_land(std::size_t pixel) const noexcept { return water_body[pixel] == 0; }
    [[nodiscard]] const WaterBody* water(std::uint16_t id) const noexcept {
        return (id == 0 || id > water_bodies.size()) ? nullptr
                                                     : &water_bodies[static_cast<std::size_t>(id) - 1];
    }
    [[nodiscard]] bool operator==(const PhysicalLayer&) const = default;
};

/// Checks every invariant of the physical layer (like validate_map for PMD1): sizes, materials, GRID
/// scale, the water table (dense ids, kinds, keys), WBOD ids and areas, every body 4-connected, bed
/// not above the surface, seas at sea_level, GEOR consistent with the dimensions, PROV sane.
/// Returns the first violation found (physical_error code).
[[nodiscard]] core::Result<void, core::Error> validate_physical(const PhysicalLayer& layer);

/// GEOR alone: projection "none" must be all zero; real projections need a datum, positive pixel
/// sizes, in-range angles and a non-empty bounding box whose extent fits the dimensions.
[[nodiscard]] core::Result<void, core::Error>
validate_georef(const GeoRef& georef, std::uint32_t width, std::uint32_t height);

/// PROV alone (strings, counts, version); shared with the province container.
[[nodiscard]] core::Result<void, core::Error> validate_provenance(const Provenance& provenance);

/// Coast key (ADR-0007 section 1.1): FNV-1a64(width, height, raw_hash(GEOR), raw_hash(WBOD)).
/// Province layers bind to it; it ignores heights, materials, rivers and provenance.
[[nodiscard]] std::uint64_t coast_key(std::uint32_t width,
                                      std::uint32_t height,
                                      std::uint64_t georef_raw_hash,
                                      std::uint64_t water_raw_hash) noexcept;
[[nodiscard]] std::uint64_t coast_key(const PhysicalLayer& layer);
/// The same from the chunk table of an opened file (no decoding; uses the stored raw hashes).
[[nodiscard]] core::Result<std::uint64_t, core::Error> coast_key(const Pmd2Reader& reader);

// Chunk blob codecs (the layouts of ADR-0007 section 3). Decoders reject truncation; trailing bytes
// are an error unless `allow_trailing` (a newer minor may append fields).
[[nodiscard]] std::vector<std::uint8_t> encode_georef(const GeoRef& georef);
[[nodiscard]] core::Result<GeoRef, core::Error> decode_georef(std::span<const std::uint8_t> bytes,
                                                              bool allow_trailing = false);
[[nodiscard]] std::vector<std::uint8_t> encode_water_table(std::span<const WaterBody> bodies);
[[nodiscard]] core::Result<std::vector<WaterBody>, core::Error>
decode_water_table(std::span<const std::uint8_t> bytes, bool allow_trailing = false);
[[nodiscard]] std::vector<std::uint8_t> encode_provenance(const Provenance& provenance);
[[nodiscard]] core::Result<Provenance, core::Error> decode_provenance(std::span<const std::uint8_t> bytes,
                                                                      bool allow_trailing = false);

/// Water body table entries recomputed from the raster: area_px per id (ids above `count` ignored).
[[nodiscard]] std::vector<std::uint32_t> count_water_area(std::span<const std::uint16_t> water_body,
                                                          std::size_t count);

struct PhysicalReadOptions {
    bool verify_raw = false;  ///< Strict mode: also compare every chunk's raw_hash.
    unsigned threads = 1;     ///< Tile decoding threads (0 = hardware concurrency).
};

/// Serialises a valid layer (validate_physical runs first) into a kind = 1 container whose header
/// bind_key is the coast key. The output does not depend on `options.threads`.
[[nodiscard]] core::Result<std::vector<std::uint8_t>, core::Error>
serialize_physical(const PhysicalLayer& layer, const Pmd2WriterOptions& options = {});
[[nodiscard]] core::Result<void, core::Error> write_physical_file(const std::filesystem::path& path,
                                                                  const PhysicalLayer& layer,
                                                                  const Pmd2WriterOptions& options = {});

/// Reads and validates a physical container: unknown optional chunks are skipped, a missing or
/// unknown critical chunk, a bad blob, a failed validation or a wrong bind_key is an error.
[[nodiscard]] core::Result<PhysicalLayer, core::Error> read_physical(std::span<const std::uint8_t> bytes,
                                                                     const PhysicalReadOptions& options = {});
[[nodiscard]] core::Result<PhysicalLayer, core::Error>
read_physical_file(const std::filesystem::path& path, const PhysicalReadOptions& options = {});

}  // namespace pat::mapdata
