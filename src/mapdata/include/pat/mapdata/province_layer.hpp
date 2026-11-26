#pragma once

#include "pat/core/result.hpp"
#include "pat/mapdata/map_data.hpp"
#include "pat/mapdata/physical_layer.hpp"
#include "pat/mapdata/pmd2_container.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

// PMD2 provinces layer (ADR-0007 section 4, Geo-12): the content of a `kind = 2` container on top of
// Pmd2Writer / Pmd2Reader. Chunks: PIDX (u16 province id per pixel, 0 = water) and PTBL (province
// table), both critical, and PROV (optional, geometry provenance). The header bind_key is the coast
// key of the physical layer the borders were drawn for (ADR-0007 section 1.1).
//
// Nothing derived from the relief is stored: mean_height is computed when a MapData is assembled
// (build_map_data / load_map_set), so a physical file rebuilt without changing the coast needs no new
// province file. The PMD1 code (MapData, map_format, validate_map) is not touched; migration is Geo-13.

namespace pat::mapdata {

inline constexpr std::size_t kMaxProvinceKeyLength = 32;
inline constexpr std::size_t kMaxProvinceCount = 65535;

/// Error codes of the provinces layer (container 100..118, physical 120..136, PMD1 20..45).
namespace province_error {
inline constexpr int kWrongKind = 140;          ///< The container is not a provinces one.
inline constexpr int kLayerSize = 141;          ///< PIDX does not match width x height or the physical layer.
inline constexpr int kBadTable = 142;           ///< PTBL: structure, counts or value ranges invalid.
inline constexpr int kBadKey = 143;             ///< A province key is empty, too long, invalid or not unique.
inline constexpr int kBadId = 144;              ///< Ids are not dense 1..N or PIDX holds an id above N.
inline constexpr int kLandMask = 145;           ///< Province != 0 is not exactly where WBOD == 0.
inline constexpr int kTableMismatch = 146;      ///< A table record differs from the PIDX pixels.
inline constexpr int kProvinceShape = 147;      ///< A province is empty or not one 4-connected region.
inline constexpr int kDuplicateColor = 148;     ///< Two provinces share a colour.
inline constexpr int kGraphDisconnected = 149;  ///< The province graph is not connected.
inline constexpr int kBindKey = 150;            ///< Borders were built for another coast.
inline constexpr int kBadChunk = 151;      ///< A chunk blob is truncated, malformed or has trailing bytes.
inline constexpr int kMissingChunk = 152;  ///< A required chunk is absent.
}  // namespace province_error

/// One PTBL record. `id` is the dense number 1..N used in PIDX; `key` is the permanent identity that
/// content, scenarios and saves refer to (owner decision #201): [a-z0-9-], unique, 1..32 characters.
/// There is no mean_height (derived from the relief).
struct ProvinceEntry {
    std::uint16_t id = 0;
    std::string key;
    std::uint32_t color_rgb = 0;  ///< 0xRRGGBB, unique.
    std::uint32_t area_px = 0;
    std::uint16_t min_x = 0;
    std::uint16_t min_y = 0;
    std::uint16_t max_x = 0;  ///< Inclusive bounding box.
    std::uint16_t max_y = 0;
    std::uint16_t centroid_x = 0;
    std::uint16_t centroid_y = 0;
    bool coastal = false;           ///< Touches a water body of kind sea.
    std::vector<Border> neighbors;  ///< Sorted by neighbor id, symmetric.
    [[nodiscard]] bool operator==(const ProvinceEntry&) const = default;
};

/// In-memory provinces layer: the PIDX raster, the table and the coast key it is bound to.
struct ProvinceLayer {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t bind_key = 0;              ///< coast_key of the physical layer these borders belong to.
    std::vector<std::uint16_t> province_id;  ///< PIDX: 0 = water, k = province k.
    std::vector<ProvinceEntry> entries;
    std::optional<Provenance> provenance;

    [[nodiscard]] std::size_t pixel_count() const noexcept {
        return static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    }
    [[nodiscard]] const ProvinceEntry* entry(std::uint16_t id) const noexcept {
        return (id == 0 || id > entries.size()) ? nullptr : &entries[static_cast<std::size_t>(id) - 1];
    }
    [[nodiscard]] bool operator==(const ProvinceLayer&) const = default;
};

/// The province table without rasters (headless tools, AI, scenario loading).
struct ProvinceTable {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t bind_key = 0;
    std::uint64_t content_id = 0;
    std::vector<ProvinceEntry> entries;
    [[nodiscard]] bool operator==(const ProvinceTable&) const = default;
};

/// True for a valid province key: 1..kMaxProvinceKeyLength characters of [a-z0-9-].
[[nodiscard]] bool valid_province_key(const std::string& key);

/// Geometry recomputed from a raster: area, bounding box, centroid (integer mean), coastal flag and
/// neighbours with border lengths for provinces 1..count. Keys and colours are left empty/zero.
/// `sea_mask` (empty = none) has one byte per pixel, non-zero where the pixel is a sea; a province is
/// coastal if one of its pixels has a 4-neighbour in the mask (the map edge is not water). Ids above
/// `count` in the raster are ignored.
[[nodiscard]] std::vector<ProvinceEntry>
compute_province_geometry(std::uint32_t width,
                          std::uint32_t height,
                          std::span<const std::uint16_t> province_id,
                          std::size_t count,
                          std::span<const std::uint8_t> sea_mask = {});

/// Sea mask of a physical layer (WBOD of bodies with kind sea).
[[nodiscard]] std::vector<std::uint8_t> sea_mask(const PhysicalLayer& physical);

/// PTBL blob codec (ADR-0007 section 4): u16 count, then per record
///   u16 id | str key | u32 color_rgb | u32 area_px | u16 min_x, min_y, max_x, max_y |
///   u16 centroid_x, centroid_y | u8 flags (bit 0 coastal, others 0) | u16 neighbor_count |
///   { u16 neighbor | u32 length_px }
/// with str = u16 length + bytes. Decoding rejects truncation; trailing bytes are an error unless
/// `allow_trailing` (a newer minor may append fields). It checks the structure only, not semantics.
[[nodiscard]] std::vector<std::uint8_t> encode_province_table(std::span<const ProvinceEntry> entries);
[[nodiscard]] core::Result<std::vector<ProvinceEntry>, core::Error>
decode_province_table(std::span<const std::uint8_t> bytes, bool allow_trailing = false);

/// The table alone: dense ids, valid unique keys and colours, ranges (bounding box and centroid in
/// the raster, area), neighbour lists sorted, in range, symmetric with equal lengths.
[[nodiscard]] core::Result<void, core::Error>
validate_province_table(std::span<const ProvinceEntry> entries, std::uint32_t width, std::uint32_t height);

/// Every province reachable from province 1 through the neighbour lists
/// (province_error::kGraphDisconnected otherwise). Required for maps without a georeference only.
[[nodiscard]] core::Result<void, core::Error> validate_province_graph(std::span<const ProvinceEntry> entries);

/// Everything checkable without the physical layer: sizes, ids within 1..N, the table, every record
/// against the pixels (all but the coastal flag, which needs WBOD), every province one 4-connected
/// region, unique colours. Returns the first violation (province_error code).
[[nodiscard]] core::Result<void, core::Error> validate_province_layer(const ProvinceLayer& layer);

/// validate_province_layer plus the pairing with a physical layer (ADR-0007 section 4): same size,
/// bind_key == coast_key(physical), province != 0 exactly where WBOD == 0, coastal flags against the
/// sea bodies. A different coast is province_error::kBindKey. A map without a georeference (GEOR
/// projection none, one landmass) must also have a connected graph (validate_province_graph); a
/// georeferenced map may have islands, and the graph of each landmass is connected by construction.
[[nodiscard]] core::Result<void, core::Error> validate_provinces(const PhysicalLayer& physical,
                                                                 const ProvinceLayer& provinces);

struct ProvinceReadOptions {
    bool verify_raw = false;  ///< Strict mode: also compare every chunk's raw_hash.
    unsigned threads = 1;     ///< Tile decoding threads (0 = hardware concurrency).
};

/// Serialises a valid layer (validate_province_layer runs first) into a kind = 2 container whose
/// header bind_key is `layer.bind_key`. The output does not depend on `options.threads`.
[[nodiscard]] core::Result<std::vector<std::uint8_t>, core::Error>
serialize_provinces(const ProvinceLayer& layer, const Pmd2WriterOptions& options = {});
[[nodiscard]] core::Result<void, core::Error> write_provinces_file(const std::filesystem::path& path,
                                                                   const ProvinceLayer& layer,
                                                                   const Pmd2WriterOptions& options = {});

/// Reads and validates a provinces container on its own (validate_province_layer); the pairing with a
/// physical file is checked by validate_provinces / load_map_set.
[[nodiscard]] core::Result<ProvinceLayer, core::Error>
read_provinces(std::span<const std::uint8_t> bytes, const ProvinceReadOptions& options = {});
[[nodiscard]] core::Result<ProvinceLayer, core::Error>
read_provinces_file(const std::filesystem::path& path, const ProvinceReadOptions& options = {});

/// Reads only the header and PTBL (PIDX is not decompressed) and validates the table. The container
/// is still opened, so its structure and every chunk's stored hash are checked.
[[nodiscard]] core::Result<ProvinceTable, core::Error>
load_province_table(std::span<const std::uint8_t> bytes);
[[nodiscard]] core::Result<ProvinceTable, core::Error> load_province_table(const std::filesystem::path& path);

/// Assembles the in-memory map from a valid pair (validate_provinces runs first): heights, materials,
/// sea_level, scale and seed from the physical layer, provinces and the table from the provinces
/// layer; mean_height is recomputed as sum(heights) / area_px (integer division, like PMD1).
[[nodiscard]] core::Result<MapData, core::Error> build_map_data(const PhysicalLayer& physical,
                                                                const ProvinceLayer& provinces);

/// Reads physical.pmd and provinces.pmd, checks that the borders belong to this coast (bind_key,
/// before the province raster is decoded) and returns the assembled MapData.
[[nodiscard]] core::Result<MapData, core::Error> load_map_set(std::span<const std::uint8_t> physical_bytes,
                                                              std::span<const std::uint8_t> province_bytes,
                                                              const ProvinceReadOptions& options = {});
[[nodiscard]] core::Result<MapData, core::Error> load_map_set(const std::filesystem::path& physical_path,
                                                              const std::filesystem::path& provinces_path,
                                                              const ProvinceReadOptions& options = {});

}  // namespace pat::mapdata
