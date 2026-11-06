#include "pat/mapdata/map_format.hpp"

#include "pat/core/bytes.hpp"
#include "pat/core/compress.hpp"
#include "pat/core/hash_stream.hpp"

#include <array>
#include <cstring>
#include <fstream>
#include <string>
#include <system_error>

namespace pat::mapdata {

namespace {

using core::Error;
using core::Result;

constexpr std::array<std::uint8_t, 4> kMagic = {'P', 'M', 'D', '1'};
constexpr std::uint32_t kSectionCount = 4;
constexpr std::uint32_t kSectionHeights = 1;
constexpr std::uint32_t kSectionProvinces = 2;
constexpr std::uint32_t kSectionMaterials = 3;
constexpr std::uint32_t kSectionTable = 4;

constexpr std::uint32_t kMaxDimension = 65535;    // Coordinates are stored as u16.
constexpr std::uint64_t kMaxPixels = 1ULL << 30;  // Sanity bound against absurd headers.
constexpr std::uint64_t kMaxTableBytes = 64ULL << 20;

Error map_error(int code, std::string message) {
    return Error{code, std::move(message)};
}

std::uint64_t section_hash(std::span<const std::uint8_t> bytes) {
    core::HashStream h;
    h.bytes(bytes);
    return h.value();
}

// Row-wise delta (wrapping) followed by splitting into low-byte and high-byte planes. Smooth terrain
// and large constant regions become long runs of small values that zstd compresses very well.
std::vector<std::uint8_t> encode_u16_layer(const std::vector<std::uint16_t>& v, std::uint32_t width) {
    const std::size_t n = v.size();
    std::vector<std::uint8_t> out(n * 2);
    for (std::size_t i = 0; i < n; ++i) {
        const bool row_start = (i % width) == 0;
        const auto d =
            static_cast<std::uint16_t>(row_start ? v[i] : static_cast<std::uint16_t>(v[i] - v[i - 1]));
        out[i] = static_cast<std::uint8_t>(d & 0xFFU);
        out[n + i] = static_cast<std::uint8_t>(d >> 8U);
    }
    return out;
}

std::vector<std::uint16_t> decode_u16_layer(std::span<const std::uint8_t> bytes, std::uint32_t width) {
    const std::size_t n = bytes.size() / 2;
    std::vector<std::uint16_t> v(n);
    for (std::size_t i = 0; i < n; ++i) {
        const auto d =
            static_cast<std::uint16_t>(bytes[i] | (static_cast<std::uint16_t>(bytes[n + i]) << 8U));
        const bool row_start = (i % width) == 0;
        v[i] = row_start ? d : static_cast<std::uint16_t>(v[i - 1] + d);
    }
    return v;
}

std::vector<std::uint8_t> encode_table(const std::vector<Province>& list) {
    core::ByteWriter w;
    for (const Province& p : list) {
        w.u16(p.id);
        w.u32(p.color_rgb);
        w.u32(p.area_px);
        w.u16(p.centroid_x);
        w.u16(p.centroid_y);
        w.u16(p.min_x);
        w.u16(p.min_y);
        w.u16(p.max_x);
        w.u16(p.max_y);
        w.u16(p.mean_height);
        w.boolean(p.coastal);
        w.u16(static_cast<std::uint16_t>(p.neighbors.size()));
        for (const Border& b : p.neighbors) {
            w.u16(b.neighbor);
            w.u32(b.length_px);
        }
    }
    return w.take();
}

bool decode_table(std::span<const std::uint8_t> bytes, std::uint16_t count, std::vector<Province>& out) {
    core::ByteReader r(bytes);
    out.clear();
    out.reserve(count);
    for (std::uint16_t i = 0; i < count; ++i) {
        Province p;
        p.id = r.u16();
        p.color_rgb = r.u32();
        p.area_px = r.u32();
        p.centroid_x = r.u16();
        p.centroid_y = r.u16();
        p.min_x = r.u16();
        p.min_y = r.u16();
        p.max_x = r.u16();
        p.max_y = r.u16();
        p.mean_height = r.u16();
        p.coastal = r.boolean();
        const std::uint16_t nb = r.u16();
        if (!r.ok() || p.id != i + 1U || nb > count) {
            return false;
        }
        p.neighbors.reserve(nb);
        for (std::uint16_t k = 0; k < nb; ++k) {
            Border b;
            b.neighbor = r.u16();
            b.length_px = r.u32();
            p.neighbors.push_back(b);
        }
        if (!r.ok()) {
            return false;
        }
        out.push_back(std::move(p));
    }
    return r.ok() && r.at_end();
}

void write_section(core::ByteWriter& out, std::uint32_t id, std::span<const std::uint8_t> raw, int level) {
    const std::vector<std::uint8_t> packed = core::compress(raw, level);
    out.u32(id);
    out.u64(raw.size());
    out.u64(section_hash(raw));
    out.u64(packed.size());
    out.bytes(packed);
}

/// Reads and verifies one section; the result is the uncompressed payload.
Result<std::vector<std::uint8_t>, Error> read_section(core::ByteReader& in,
                                                      std::uint32_t expected_id,
                                                      std::uint64_t expected_size,
                                                      std::uint64_t max_size) {
    using R = Result<std::vector<std::uint8_t>, Error>;
    const std::uint32_t id = in.u32();
    const std::uint64_t raw_size = in.u64();
    const std::uint64_t hash = in.u64();
    const std::uint64_t packed_size = in.u64();
    if (!in.ok()) {
        return R::err(map_error(30, "map file is truncated (section header)"));
    }
    if (id != expected_id) {
        return R::err(map_error(31, "unexpected section id " + std::to_string(id)));
    }
    if (raw_size > max_size || (expected_size != 0 && raw_size != expected_size)) {
        return R::err(map_error(32, "section " + std::to_string(id) + " has an invalid size"));
    }
    if (packed_size > in.remaining()) {
        return R::err(map_error(30, "map file is truncated (section " + std::to_string(id) + ")"));
    }
    const auto packed = in.bytes(static_cast<std::size_t>(packed_size));
    auto raw = core::decompress(packed, static_cast<std::size_t>(raw_size));
    if (!raw) {
        return R::err(
            map_error(33, "section " + std::to_string(id) + " is corrupted: " + raw.error().message));
    }
    if (section_hash(raw.value()) != hash) {
        return R::err(map_error(34, "section " + std::to_string(id) + " is corrupted (hash mismatch)"));
    }
    return R::ok(std::move(raw).value());
}

}  // namespace

std::vector<std::uint8_t> serialize_map(const MapData& map, int zstd_level) {
    core::ByteWriter out;
    out.bytes(kMagic);
    out.u16(kMapFormatVersion);
    out.u16(0);  // flags
    out.u32(map.width);
    out.u32(map.height);
    out.u16(map.sea_level);
    out.u16(static_cast<std::uint16_t>(map.province_list.size()));
    out.u32(map.metres_per_unit_milli);
    out.u64(map.seed);
    out.u32(kSectionCount);

    write_section(out, kSectionHeights, encode_u16_layer(map.heights, map.width), zstd_level);
    write_section(out, kSectionProvinces, encode_u16_layer(map.provinces, map.width), zstd_level);
    write_section(out, kSectionMaterials, map.materials, zstd_level);
    write_section(out, kSectionTable, encode_table(map.province_list), zstd_level);
    return out.take();
}

Result<MapData, Error> deserialize_map(std::span<const std::uint8_t> bytes) {
    using R = Result<MapData, Error>;
    core::ByteReader in(bytes);

    const auto magic = in.bytes(kMagic.size());
    if (!in.ok() || std::memcmp(magic.data(), kMagic.data(), kMagic.size()) != 0) {
        return R::err(map_error(20, "not a Patriarch map file (bad magic)"));
    }
    const std::uint16_t version = in.u16();
    const std::uint16_t flags = in.u16();
    MapData map;
    map.width = in.u32();
    map.height = in.u32();
    map.sea_level = in.u16();
    const std::uint16_t province_count = in.u16();
    map.metres_per_unit_milli = in.u32();
    map.seed = in.u64();
    const std::uint32_t sections = in.u32();
    if (!in.ok()) {
        return R::err(map_error(21, "map file is truncated (header)"));
    }
    if (version != kMapFormatVersion) {
        return R::err(map_error(22, "unsupported map format version " + std::to_string(version)));
    }
    if (flags != 0 || sections != kSectionCount) {
        return R::err(map_error(23, "unsupported map flags or section count"));
    }
    if (map.width == 0 || map.height == 0 || map.width > kMaxDimension || map.height > kMaxDimension ||
        static_cast<std::uint64_t>(map.width) * map.height > kMaxPixels) {
        return R::err(map_error(24, "invalid map dimensions"));
    }
    const std::uint64_t pixels = static_cast<std::uint64_t>(map.width) * map.height;

    auto heights = read_section(in, kSectionHeights, pixels * 2, pixels * 2);
    if (!heights) {
        return R::err(heights.error());
    }
    auto provinces = read_section(in, kSectionProvinces, pixels * 2, pixels * 2);
    if (!provinces) {
        return R::err(provinces.error());
    }
    auto materials = read_section(in, kSectionMaterials, pixels, pixels);
    if (!materials) {
        return R::err(materials.error());
    }
    auto table = read_section(in, kSectionTable, 0, kMaxTableBytes);
    if (!table) {
        return R::err(table.error());
    }
    if (!in.at_end()) {
        return R::err(map_error(35, "trailing bytes after the last section"));
    }

    map.heights = decode_u16_layer(heights.value(), map.width);
    map.provinces = decode_u16_layer(provinces.value(), map.width);
    map.materials = std::move(materials).value();
    if (!decode_table(table.value(), province_count, map.province_list)) {
        return R::err(map_error(36, "province table is malformed"));
    }
    for (const std::uint16_t p : map.provinces) {
        if (p > province_count) {
            return R::err(map_error(37, "province layer references an unknown province"));
        }
    }
    for (const std::uint8_t m : map.materials) {
        if (m >= kMaterialCount) {
            return R::err(map_error(38, "material layer contains an unknown material"));
        }
    }
    return R::ok(std::move(map));
}

core::Result<void, Error>
write_map_file(const std::filesystem::path& path, const MapData& map, int zstd_level) {
    using R = Result<void, Error>;
    const std::vector<std::uint8_t> bytes = serialize_map(map, zstd_level);
    std::filesystem::path temp = path;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            return R::err(map_error(40, "cannot create " + temp.string()));
        }
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        out.flush();
        if (!out) {
            return R::err(map_error(41, "write failed: " + temp.string()));
        }
    }
    std::error_code ec;
    std::filesystem::rename(temp, path, ec);  // Atomic replace on the same volume.
    if (ec) {
        std::filesystem::remove(temp, ec);
        return R::err(map_error(42, "cannot move the map into place: " + path.string()));
    }
    return R::ok();
}

core::Result<MapData, Error> read_map_file(const std::filesystem::path& path) {
    using R = Result<MapData, Error>;
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        return R::err(map_error(43, "cannot open " + path.string()));
    }
    const std::streamsize size = in.tellg();
    if (size < 0) {
        return R::err(map_error(44, "cannot determine the size of " + path.string()));
    }
    in.seekg(0);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    in.read(reinterpret_cast<char*>(bytes.data()), size);
    if (!in) {
        return R::err(map_error(45, "read failed: " + path.string()));
    }
    return deserialize_map(bytes);
}

}  // namespace pat::mapdata
