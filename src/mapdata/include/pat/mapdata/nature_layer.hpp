#pragma once

#include "pat/core/result.hpp"
#include "pat/mapdata/physical_layer.hpp"
#include "pat/mapdata/pmd2_container.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

// Optional chunks of the PMD2 physical container (ADR-0007 sections 3.4 and 3.5, Geo-14): rivers
// (RIVR raster of ranks + RVEC vector channels for the ribbon meshes) and natural regions (NREG raster,
// possibly coarser than the grid, + NRTB table). All integers; no floating point.
//
// These chunks are optional: read_physical skips them, and a file without them stays valid. They live
// beside the physical chunks, so they are attached to / read from an existing kind = 1 container
// (attach_nature / read_nature) without touching the physical layer code. RIVR and RVEC go together,
// as do NREG and NRTB: a half pair is an error.

namespace pat::mapdata {

inline constexpr std::size_t kMaxRiverKeyLength = 28;
inline constexpr std::size_t kMaxRegionKeyLength = 40;
inline constexpr std::size_t kMaxRegionNameLength = 64;
inline constexpr std::size_t kMaxRiverPoints = 1U << 22;    ///< Per river.
inline constexpr std::size_t kMaxRiverVertices = 1U << 24;  ///< All rivers together.
inline constexpr std::int32_t kRiverSubpixel = 16;          ///< RVEC coordinates are in 1/16 pixel.

/// Error codes of the nature layer (container 100..118, physical 120..136, provinces 140..152,
/// migration 160..165).
namespace nature_error {
inline constexpr int kBadRiverRaster = 170;    ///< RIVR is not a full-resolution raster of the grid size.
inline constexpr int kBadRiverTable = 171;     ///< RVEC: ids, keys, ranks or point counts invalid.
inline constexpr int kRiverInSea = 172;        ///< A river pixel or vertex lies in a sea.
inline constexpr int kRankNotMonotonic = 173;  ///< Width class decreases downstream or differs from the rank.
inline constexpr int kBadRiverGeometry = 174;  ///< Vertex outside the grid or a repeated vertex.
inline constexpr int kBadRegionTable = 175;    ///< NRTB: ids, kinds, keys or names invalid.
inline constexpr int kBadRegionRaster = 176;   ///< NREG: wrong size or shift.
inline constexpr int kBadRegionId = 177;       ///< NREG id without an entry, or an entry without pixels.
inline constexpr int kBadChunk = 178;          ///< A blob is truncated, malformed or has trailing bytes.
inline constexpr int kMissingChunk = 179;      ///< Half of a RIVR/RVEC or NREG/NRTB pair is absent.
inline constexpr int kWrongKind = 180;         ///< The container is not a physical one.
}  // namespace nature_error

struct RiverPoint {
    std::int32_t x = 0;  ///< 1/16 pixel, 0 .. width * 16 - 1.
    std::int32_t y = 0;
    std::uint8_t width_class = 1;  ///< 1..255, never decreases downstream.
    [[nodiscard]] bool operator==(const RiverPoint&) const = default;
};

/// One river, points run from the source to the mouth.
struct River {
    std::uint16_t id = 0;            ///< Dense 1..N.
    std::uint8_t rank = 1;           ///< Equals the width class of the mouth.
    std::string key;                 ///< [a-z0-9-], unique, 1..kMaxRiverKeyLength.
    std::vector<RiverPoint> points;  ///< At least two, no two consecutive equal.
    [[nodiscard]] bool operator==(const River&) const = default;
};

struct RiverLayer {
    /// RIVR: full resolution (width x height), 0 = no river, 1..255 rank. Rivers are marks on land and
    /// lakes; the bed is carved into HGHT by the builder.
    std::vector<std::uint8_t> rank_raster;
    std::vector<River> rivers;  ///< RVEC.
    [[nodiscard]] bool operator==(const RiverLayer&) const = default;
};

enum class RegionKind : std::uint8_t { climate_zone = 1, orographic = 2 };

struct NaturalRegion {
    std::uint8_t id = 0;  ///< Dense 1..N (0 in the raster means "no region").
    RegionKind kind = RegionKind::climate_zone;
    std::string key;   ///< [a-z0-9-], unique, e.g. "west-siberian-plain".
    std::string name;  ///< English reference name, printable ASCII; display names are E4 content.
    [[nodiscard]] bool operator==(const NaturalRegion&) const = default;
};

struct RegionLayer {
    std::uint8_t shift = 0;              ///< Raster is ceil(width / 2^shift) x ceil(height / 2^shift).
    std::vector<std::uint8_t> raster;    ///< NREG: 0 = none, else a region id.
    std::vector<NaturalRegion> regions;  ///< NRTB.
    [[nodiscard]] bool operator==(const RegionLayer&) const = default;
};

/// What attach_nature adds to a physical container; either part may be absent.
struct NatureLayer {
    std::optional<RiverLayer> rivers;
    std::optional<RegionLayer> regions;
    [[nodiscard]] bool operator==(const NatureLayer&) const = default;
};

/// Size of the NREG raster along one axis for a grid size and shift.
[[nodiscard]] std::uint32_t region_raster_extent(std::uint32_t size, std::uint8_t shift) noexcept;

/// Rivers against the physical layer: RIVR size, ids/keys/ranks, vertices inside the grid, no river
/// pixel and no vertex in a sea or inland sea (lakes are fine), width classes non-decreasing
/// downstream and the rank equal to the mouth class. Returns the first violation (nature_error code).
[[nodiscard]] core::Result<void, core::Error> validate_rivers(const RiverLayer& rivers,
                                                              const PhysicalLayer& physical);
/// Regions against the physical layer: NREG size for the shift, table sanity, every raster id has an
/// entry and every entry is used.
[[nodiscard]] core::Result<void, core::Error> validate_regions(const RegionLayer& regions,
                                                               const PhysicalLayer& physical);
[[nodiscard]] core::Result<void, core::Error> validate_nature(const NatureLayer& nature,
                                                              const PhysicalLayer& physical);

// Blob codecs (ADR-0007 sections 3.4 and 3.5). Decoders reject truncation; trailing bytes are an
// error unless `allow_trailing` (a newer minor may append fields).
[[nodiscard]] std::vector<std::uint8_t> encode_river_vectors(std::span<const River> rivers);
[[nodiscard]] core::Result<std::vector<River>, core::Error>
decode_river_vectors(std::span<const std::uint8_t> bytes, bool allow_trailing = false);
[[nodiscard]] std::vector<std::uint8_t> encode_region_table(std::span<const NaturalRegion> regions);
[[nodiscard]] core::Result<std::vector<NaturalRegion>, core::Error>
decode_region_table(std::span<const std::uint8_t> bytes, bool allow_trailing = false);

/// Returns `physical_bytes` (a kind = 1 container) with the RIVR/RVEC/NREG/NRTB chunks of `nature`
/// added; chunks of those ids already in the file are replaced, everything else is copied byte for
/// byte, the bind_key is kept. The nature layer is validated against the physical layer read from the
/// same bytes. The output does not depend on `options.threads`.
[[nodiscard]] core::Result<std::vector<std::uint8_t>, core::Error>
attach_nature(std::span<const std::uint8_t> physical_bytes,
              const NatureLayer& nature,
              const Pmd2WriterOptions& options = {});
/// serialize_physical + attach_nature in one step.
[[nodiscard]] core::Result<std::vector<std::uint8_t>, core::Error> serialize_physical_with_nature(
    const PhysicalLayer& layer, const NatureLayer& nature, const Pmd2WriterOptions& options = {});
[[nodiscard]] core::Result<void, core::Error>
write_physical_with_nature_file(const std::filesystem::path& path,
                                const PhysicalLayer& layer,
                                const NatureLayer& nature,
                                const Pmd2WriterOptions& options = {});

/// Reads the nature chunks of a physical container and validates them against `physical` (the layer
/// read from the same file). Absent pairs stay empty.
[[nodiscard]] core::Result<NatureLayer, core::Error> read_nature(std::span<const std::uint8_t> physical_bytes,
                                                                 const PhysicalLayer& physical,
                                                                 const PhysicalReadOptions& options = {});
[[nodiscard]] core::Result<NatureLayer, core::Error>
read_nature_file(const std::filesystem::path& path,
                 const PhysicalLayer& physical,
                 const PhysicalReadOptions& options = {});

}  // namespace pat::mapdata
