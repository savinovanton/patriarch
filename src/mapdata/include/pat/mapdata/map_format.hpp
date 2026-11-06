#pragma once

#include "pat/core/result.hpp"
#include "pat/mapdata/map_data.hpp"

#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

// Map file format "PMD1" (little endian), E3-T03. Bump kMapFormatVersion when the layout changes.
//
//   "PMD1" | u16 format_version | u16 flags (0) | u32 width | u32 height | u16 sea_level |
//   u16 province_count | u32 metres_per_unit_milli | u64 seed | u32 section_count
//   then per section: u32 id | u64 uncompressed_size | u64 hash | u64 compressed_size | zstd frame
//
// Sections (each exactly once, in this order):
//   1 heights    u16 per pixel: row delta + byte planes, then zstd
//   2 provinces  u16 per pixel: row delta + byte planes, then zstd
//   3 materials  u8 per pixel, zstd
//   4 table      province records (see map_format.cpp), zstd
// `hash` is the FNV-1a hash (core::HashStream) of the uncompressed section bytes.

namespace pat::mapdata {

inline constexpr std::uint16_t kMapFormatVersion = 1;

/// Serializes the map. The map should satisfy validate_map(); the writer itself only checks sizes.
[[nodiscard]] std::vector<std::uint8_t> serialize_map(const MapData& map, int zstd_level = 3);

/// Parses a map file. Detects truncation, bit flips, unknown versions and inconsistent layers.
[[nodiscard]] core::Result<MapData, core::Error> deserialize_map(std::span<const std::uint8_t> bytes);

[[nodiscard]] core::Result<void, core::Error>
write_map_file(const std::filesystem::path& path, const MapData& map, int zstd_level = 3);
[[nodiscard]] core::Result<MapData, core::Error> read_map_file(const std::filesystem::path& path);

}  // namespace pat::mapdata
