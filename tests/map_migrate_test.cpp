// PMD1 -> PMD2 migration, load_map and the committed fixtures (ADR-0007 section 5, Geo-13).
//
// The fixtures in tests/data/maps are small files, not generated at test time: generate_map uses
// floating point, so a map made on the fly could differ between platforms, while a file read back is
// the same everywhere. They are produced by the regeneration case at the end of this file
// (PAT_REGEN_MAP_FIXTURES=1), which also prints the golden values below. Written PMD2 files are NOT
// compared byte for byte (zstd output depends on the library version): the golden values are the
// content_id and the raw_hash of every chunk, which do not.

#include "pat/core/hash_stream.hpp"
#include "pat/mapdata/map_format.hpp"
#include "pat/mapdata/map_gen.hpp"
#include "pat/mapdata/map_migrate.hpp"
#include "pat/mapdata/map_stats.hpp"
#include "pat/mapdata/map_validate.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <utility>
#include <vector>

using namespace pat::mapdata;

namespace {

namespace fs = std::filesystem;
using Bytes = std::vector<std::uint8_t>;

constexpr Pmd2ChunkId kHght = pmd2_chunk_id('H', 'G', 'H', 'T');
constexpr Pmd2ChunkId kXopt = pmd2_chunk_id('X', 'O', 'P', 'T');
constexpr Pmd2ChunkId kXcrt = pmd2_chunk_id('X', 'C', 'R', 'T');

// ---- golden values (change only on purpose; see the regeneration case) ---------------------------

struct GoldenChunk {
    const char* id;
    std::uint64_t raw_hash;
};

/// FNV-1a of the uncompressed sections of v1_tiny.pmd, in file order (heights, provinces, materials, table).
constexpr std::array<std::uint64_t, 4> kV1SectionHashes = {
    0x0fd5a358b6e2f00aULL, 0xc4ba4bf8757465fbULL, 0x2fb505159499dc89ULL, 0xea92d23ec74b113cULL};
/// Migration of v1_tiny.pmd.
constexpr std::uint64_t kMigratedPhysicalContentId = 0x17d458767484ae5eULL;
constexpr std::uint64_t kMigratedProvincesContentId = 0x53d9b635ea240b56ULL;
constexpr std::uint64_t kMigratedCoastKey = 0x29f489dfbd547ac8ULL;
constexpr std::uint64_t kMigrationParamsHash = 0x1cda72f28af746d5ULL;
constexpr std::array<GoldenChunk, 7> kMigratedPhysicalChunks = {{
    {"GEOR", 0x0e04cd4efc0dc244ULL},
    {"GRID", 0x39d1e076f2131ddeULL},
    {"HGHT", 0x6e412aed0dc0c85bULL},
    {"MATL", 0x2fb505159499dc89ULL},
    {"PROV", 0x9616a771ccf998fcULL},
    {"WBOD", 0x0e27cce889350b87ULL},
    {"WTBL", 0x45c5cd432ad0c536ULL},
}};
constexpr std::array<GoldenChunk, 2> kMigratedProvincesChunks = {
    {{"PIDX", 0xf9647b468ea461abULL}, {"PTBL", 0xdfc421b3be7cad74ULL}}};
/// The pair with a lake whose surface is not sea_level.
constexpr std::uint64_t kLakePhysicalContentId = 0x6b6685275d5500ceULL;
constexpr std::uint64_t kLakeProvincesContentId = 0x27facb91e530a106ULL;
constexpr std::uint64_t kLakeCoastKey = 0x0625d2a23d97d427ULL;

// ---- helpers --------------------------------------------------------------------------------------

fs::path maps_dir() {
    return fs::path(PATRIARCH_TEST_DATA_DIR) / "maps";
}

Bytes read_bytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    REQUIRE_MESSAGE(static_cast<bool>(in), path.string());
    const auto size = static_cast<std::size_t>(in.tellg());
    in.seekg(0);
    Bytes bytes(size);
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    return bytes;
}

void write_bytes(const fs::path& path, const Bytes& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(static_cast<bool>(out));
}

template <class T>
std::string error_text(const pat::core::Result<T, pat::core::Error>& r) {
    return r.has_value() ? "" : r.error().message;
}

template <class T>
int code_of(const pat::core::Result<T, pat::core::Error>& r) {
    return r.has_value() ? 0 : r.error().code;
}

/// The fixture map. Only the regeneration case calls this; tests read v1_tiny.pmd.
MapData generate_v1() {
    GenParams gp;
    gp.width = 96;
    gp.height = 48;
    gp.seed = 3;
    gp.province_count = 8;
    gp.threads = 1;
    return generate_map(gp);
}

MigratedMap migrated(const MapData& map) {
    auto m = migrate_pmd1(map);
    REQUIRE_MESSAGE(m.has_value(), error_text(m));
    return std::move(m).value();
}

/// Adds a 3x3 lake in the interior of the largest province: the surface (sea_level - 5) differs from
/// sea_level, the bed lies below it, the province keeps one 4-connected region around the hole.
MigratedMap with_lake(const MigratedMap& base) {
    MigratedMap m = base;
    PhysicalLayer& l = m.physical;
    ProvinceLayer& p = m.provinces;
    std::uint16_t big = 1;
    for (const ProvinceEntry& e : p.entries) {
        if (e.area_px > p.entry(big)->area_px) {
            big = e.id;
        }
    }
    std::size_t centre = 0;
    bool found = false;
    for (std::uint32_t y = 2; y + 2 < l.height && !found; ++y) {
        for (std::uint32_t x = 2; x + 2 < l.width && !found; ++x) {
            bool inside = true;
            for (std::uint32_t dy = 0; dy < 5; ++dy) {
                for (std::uint32_t dx = 0; dx < 5; ++dx) {
                    inside = inside && p.province_id[(y - 2 + dy) * l.width + (x - 2 + dx)] == big;
                }
            }
            if (inside) {
                centre = static_cast<std::size_t>(y) * l.width + x;
                found = true;
            }
        }
    }
    REQUIRE(found);
    const auto lake_id = static_cast<std::uint16_t>(l.water_bodies.size() + 1);
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            const std::size_t i = centre + static_cast<std::size_t>(dy * static_cast<int>(l.width) + dx);
            p.province_id[i] = 0;
            l.water_body[i] = lake_id;
            l.heights[i] = static_cast<std::uint16_t>(l.sea_level - 20);
        }
    }
    l.water_bodies.push_back(
        {lake_id, WaterKind::lake, 0, static_cast<std::uint16_t>(l.sea_level - 5), 9, "lake-1"});
    auto geometry =
        compute_province_geometry(l.width, l.height, p.province_id, p.entries.size(), sea_mask(l));
    for (std::size_t i = 0; i < geometry.size(); ++i) {
        geometry[i].key = p.entries[i].key;
        geometry[i].color_rgb = p.entries[i].color_rgb;
    }
    p.entries = std::move(geometry);
    p.bind_key = coast_key(l);
    return m;
}

struct Variant {
    std::uint16_t minor = 0;
    bool extra_optional = false;
    bool extra_critical = false;
    bool future_height_version = false;
};

/// A copy of a physical file with an extra chunk, a newer minor or a newer chunk version.
Bytes make_variant(const Bytes& physical, const Variant& v) {
    Pmd2OpenOptions oo;
    oo.inspect = true;
    auto reader = Pmd2Reader::open(physical, oo);
    REQUIRE_MESSAGE(reader.has_value(), error_text(reader));
    const Pmd2Header& h = reader.value().header();
    Pmd2Writer w(Pmd2Kind::physical, h.width, h.height);
    w.set_bind_key(h.bind_key);
    for (Pmd2ChunkInfo info : reader.value().chunks()) {
        if (v.future_height_version && info.id == kHght) {
            info.version = 2;
        }
        REQUIRE(w.add_stored_chunk(info, reader.value().stored_bytes(info)).has_value());
    }
    const Bytes extra = {1, 2, 3, 4};
    if (v.extra_optional) {
        REQUIRE(w.add_blob({kXopt, 1, false, 0}, extra).has_value());
    }
    if (v.extra_critical) {
        REQUIRE(w.add_blob({kXcrt, 1, true, 0}, extra).has_value());
    }
    auto out = w.finish();
    REQUIRE_MESSAGE(out.has_value(), error_text(out));
    Bytes b = std::move(out).value();
    if (v.minor != 0) {
        // The minor is not part of content_id, so only header_hash (bytes 0..55 + the table) changes.
        b[4] = static_cast<std::uint8_t>(v.minor & 0xFFU);
        b[5] = static_cast<std::uint8_t>(v.minor >> 8U);
        const std::size_t count =
            b[20] | (b[21] << 8U) | (b[22] << 16U) | (static_cast<std::size_t>(b[23]) << 24U);
        pat::core::HashStream hash;
        hash.bytes(std::span<const std::uint8_t>(b).first(56));
        hash.bytes(std::span<const std::uint8_t>(b).subspan(kPmd2HeaderSize, kPmd2ChunkEntrySize * count));
        for (std::size_t i = 0; i < 8; ++i) {
            b[56 + i] = static_cast<std::uint8_t>((hash.value() >> (8U * i)) & 0xFFU);
        }
    }
    return b;
}

std::uint64_t content_id_of(const Bytes& bytes) {
    Pmd2OpenOptions oo;
    oo.inspect = true;
    auto reader = Pmd2Reader::open(bytes, oo);
    REQUIRE_MESSAGE(reader.has_value(), error_text(reader));
    return reader.value().header().content_id;
}

void check_chunks(const Bytes& bytes, std::span<const GoldenChunk> golden) {
    Pmd2OpenOptions oo;
    oo.inspect = true;
    auto reader = Pmd2Reader::open(bytes, oo);
    REQUIRE_MESSAGE(reader.has_value(), error_text(reader));
    REQUIRE(reader.value().chunks().size() == golden.size());
    for (const GoldenChunk& g : golden) {
        const Pmd2ChunkInfo* info = reader.value().find(pmd2_chunk_id(g.id[0], g.id[1], g.id[2], g.id[3]));
        REQUIRE_MESSAGE(info != nullptr, g.id);
        CHECK_MESSAGE(info->raw_hash == g.raw_hash, g.id);
    }
}

/// A 6x3 map with two separate ponds (4-connected water components), for the "several seas" rule.
MapData two_ponds() {
    MapData m;
    m.width = 6;
    m.height = 3;
    m.sea_level = 50;
    m.metres_per_unit_milli = 1000;
    m.seed = 9;
    m.heights = {10, 10, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 10, 10};
    for (const std::uint16_t h : m.heights) {
        m.provinces.push_back(h > m.sea_level ? 1 : 0);
        m.materials.push_back(h > m.sea_level ? static_cast<std::uint8_t>(Material::Plains)
                                              : static_cast<std::uint8_t>(Material::DeepWater));
    }
    Province p;
    p.id = 1;
    p.color_rgb = 0x336699;
    m.province_list.push_back(p);
    rebuild_province_stats(m);
    return m;
}

struct TempDir {
    fs::path path;
    explicit TempDir(const char* name) : path(fs::temp_directory_path() / name) {
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

}  // namespace

TEST_CASE("fixtures: the committed maps stay within 100 KB") {
    std::uintmax_t total = 0;
    std::size_t files = 0;
    for (const auto& entry : fs::recursive_directory_iterator(maps_dir())) {
        if (entry.is_regular_file()) {
            total += entry.file_size();
            ++files;
        }
    }
    CHECK(files == 9);
    CHECK(total <= 100U * 1024U);
}

TEST_CASE("PMD1: v1_tiny.pmd is read unchanged, load_map takes the old path") {
    const fs::path file = maps_dir() / "v1_tiny.pmd";
    auto old_path = read_map_file(file);
    REQUIRE_MESSAGE(old_path.has_value(), error_text(old_path));
    const MapData& map = old_path.value();
    CHECK(map.width == 96);
    CHECK(map.height == 48);
    CHECK(map.seed == 3);
    CHECK(map.province_list.size() == 8);
    CHECK(validate_map(map).has_value());

    auto detected = load_map(file);
    REQUIRE_MESSAGE(detected.has_value(), error_text(detected));
    CHECK(detected.value() == map);

    // The section hashes are what the file carries: read them from the header, not from our own code.
    const Bytes bytes = read_bytes(file);
    std::array<std::uint64_t, 4> in_file{};
    std::size_t pos = 36;  // after the 36-byte PMD1 header
    for (std::uint64_t& hash : in_file) {
        std::uint64_t packed = 0;
        hash = 0;
        for (std::size_t i = 0; i < 8; ++i) {
            hash |= static_cast<std::uint64_t>(bytes[pos + 12 + i]) << (8U * i);
            packed |= static_cast<std::uint64_t>(bytes[pos + 20 + i]) << (8U * i);
        }
        pos += 28 + static_cast<std::size_t>(packed);
    }
    CHECK(pos == bytes.size());
    CHECK(in_file == pmd1_section_hashes(map));
    CHECK(in_file == kV1SectionHashes);
}

TEST_CASE("migration: golden content ids and chunk hashes, fixtures equal the migration") {
    auto v1 = read_map_file(maps_dir() / "v1_tiny.pmd");
    REQUIRE_MESSAGE(v1.has_value(), error_text(v1));
    const MigratedMap m = migrated(v1.value());

    // Shape required by the ADR (the tiny map has two separate seas: "sea" and "sea-2"; a map with one
    // connected sea gets exactly the single "sea" entry), provenance step with the PMD1 section hashes.
    REQUIRE(m.physical.water_bodies.size() == 2);
    CHECK(m.physical.water_bodies[1].key == "sea-2");
    const WaterBody& sea = m.physical.water_bodies[0];
    CHECK(sea.id == 1);
    CHECK(sea.kind == WaterKind::sea);
    CHECK(sea.key == "sea");
    CHECK(sea.surface_level == v1.value().sea_level);
    CHECK(m.physical.georef == GeoRef{});
    REQUIRE(m.physical.provenance.has_value());
    CHECK(m.physical.provenance->sources.empty());
    REQUIRE(m.physical.provenance->steps.size() == 1);
    CHECK(m.physical.provenance->steps[0].step_id == "migrated-from-pmd1");
    pat::core::HashStream params;
    for (const std::uint64_t h : kV1SectionHashes) {
        params.u64(h);
    }
    CHECK(m.physical.provenance->steps[0].params_hash == params.value());
    CHECK(m.physical.provenance->steps[0].params_hash == kMigrationParamsHash);
    CHECK(m.provinces.entries[0].key == "p0001");
    CHECK(m.provinces.entries[7].key == "p0008");
    CHECK(m.provinces.bind_key == coast_key(m.physical));
    CHECK(coast_key(m.physical) == kMigratedCoastKey);

    // Written in memory: the golden values do not depend on the zstd version.
    auto physical = serialize_physical(m.physical);
    auto provinces = serialize_provinces(m.provinces);
    REQUIRE_MESSAGE(physical.has_value(), error_text(physical));
    REQUIRE_MESSAGE(provinces.has_value(), error_text(provinces));
    CHECK(content_id_of(physical.value()) == kMigratedPhysicalContentId);
    CHECK(content_id_of(provinces.value()) == kMigratedProvincesContentId);
    check_chunks(physical.value(), kMigratedPhysicalChunks);
    check_chunks(provinces.value(), kMigratedProvincesChunks);

    // The committed PMD2 files are the same content.
    const Bytes file_physical = read_bytes(maps_dir() / "v2_migrated" / kPhysicalFileName);
    const Bytes file_provinces = read_bytes(maps_dir() / "v2_migrated" / kProvincesFileName);
    CHECK(content_id_of(file_physical) == kMigratedPhysicalContentId);
    CHECK(content_id_of(file_provinces) == kMigratedProvincesContentId);
    auto read_physical_layer = read_physical(file_physical, {true, 1});
    auto read_province_layer = read_provinces(file_provinces, {true, 1});
    REQUIRE_MESSAGE(read_physical_layer.has_value(), error_text(read_physical_layer));
    REQUIRE_MESSAGE(read_province_layer.has_value(), error_text(read_province_layer));
    CHECK(read_physical_layer.value() == m.physical);
    CHECK(read_province_layer.value() == m.provinces);

    // The same, through load_map from either file of the set.
    for (const char* name : {kPhysicalFileName, kProvincesFileName}) {
        auto loaded = load_map(maps_dir() / "v2_migrated" / name, {true, 2});
        REQUIRE_MESSAGE(loaded.has_value(), error_text(loaded));
        CHECK(loaded.value() == v1.value());
    }
}

TEST_CASE("migration: PMD1 -> PMD2 -> MapData is bit for bit the original") {
    struct Case {
        std::uint32_t w, h;
        std::uint64_t seed;
        std::uint32_t provinces;
    };
    std::vector<MapData> maps;
    for (const Case c : {Case{96, 48, 3, 8}, Case{256, 128, 5, 20}, Case{200, 120, 11, 14}}) {
        GenParams gp;
        gp.width = c.w;
        gp.height = c.h;
        gp.seed = c.seed;
        gp.province_count = c.provinces;
        gp.threads = 1;
        maps.push_back(generate_map(gp));
    }
    maps.push_back(two_ponds());
    auto fixture = read_map_file(maps_dir() / "v1_tiny.pmd");
    REQUIRE(fixture.has_value());
    maps.push_back(fixture.value());

    TempDir dir("pat_map_migrate_roundtrip");
    std::size_t index = 0;
    for (const MapData& original : maps) {
        CAPTURE(index);
        REQUIRE_MESSAGE(validate_map(original).has_value(), error_text(validate_map(original)));
        const MigratedMap m = migrated(original);

        // Migration is a pure function.
        const MigratedMap again = migrated(original);
        CHECK(m.physical == again.physical);
        CHECK(m.provinces == again.provinces);

        // In memory.
        auto direct = build_map_data(m.physical, m.provinces);
        REQUIRE_MESSAGE(direct.has_value(), error_text(direct));
        CHECK(direct.value() == original);

        // Through the container bytes, strict mode, several threads.
        auto physical = serialize_physical(m.physical);
        auto provinces = serialize_provinces(m.provinces);
        REQUIRE(physical.has_value());
        REQUIRE(provinces.has_value());
        auto from_bytes = load_map_set(physical.value(), provinces.value(), {true, 3});
        REQUIRE_MESSAGE(from_bytes.has_value(), error_text(from_bytes));
        CHECK(from_bytes.value() == original);

        // Through files: write_map_set, then load_map from either file.
        const fs::path set_dir = dir.path / std::to_string(index);
        REQUIRE(write_map_set(set_dir, m).has_value());
        for (const char* name : {kPhysicalFileName, kProvincesFileName}) {
            auto from_file = load_map(set_dir / name, {true, 1});
            REQUIRE_MESSAGE(from_file.has_value(), error_text(from_file));
            CHECK(from_file.value() == original);
        }
        ++index;
    }
}

TEST_CASE("migration: separate water components become separate seas") {
    const MapData map = two_ponds();
    const MigratedMap m = migrated(map);
    REQUIRE(m.physical.water_bodies.size() == 2);
    CHECK(m.physical.water_bodies[0].key == "sea");
    CHECK(m.physical.water_bodies[1].key == "sea-2");
    CHECK(m.physical.water_bodies[0].area_px == 2);
    CHECK(m.physical.water_bodies[1].area_px == 2);
    CHECK(m.physical.water_body[0] == 1);
    CHECK(m.physical.water_body[17] == 2);
    CHECK(m.provinces.entries[0].coastal);  // as in PMD1: it touches water
}

TEST_CASE("migration: an invalid PMD1 map is an error, never an altered map") {
    auto v1 = read_map_file(maps_dir() / "v1_tiny.pmd");
    REQUIRE(v1.has_value());

    MapData water_over_land = v1.value();  // a land pixel without a province: "water <=> height <= s" broken
    for (std::size_t i = 0; i < water_over_land.provinces.size(); ++i) {
        if (water_over_land.provinces[i] != 0) {
            water_over_land.provinces[i] = 0;
            break;
        }
    }
    MapData land_in_water = v1.value();  // a province pixel on water
    for (std::size_t i = 0; i < land_in_water.provinces.size(); ++i) {
        if (land_in_water.provinces[i] == 0) {
            land_in_water.provinces[i] = 1;
            break;
        }
    }
    MapData wrong_mean = v1.value();  // table record that does not match the pixels
    wrong_mean.province_list[2].mean_height =
        static_cast<std::uint16_t>(wrong_mean.province_list[2].mean_height + 1);
    MapData empty;

    for (const MapData* bad : {&water_over_land, &land_in_water, &wrong_mean, &empty}) {
        const auto r = migrate_pmd1(*bad);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().code == migrate_error::kInvalidSource);
    }
}

TEST_CASE("fixtures: the lake pair has a water body whose level is not sea_level") {
    const Bytes physical = read_bytes(maps_dir() / "v2_lake" / kPhysicalFileName);
    const Bytes provinces = read_bytes(maps_dir() / "v2_lake" / kProvincesFileName);
    CHECK(content_id_of(physical) == kLakePhysicalContentId);
    CHECK(content_id_of(provinces) == kLakeProvincesContentId);

    auto layer = read_physical(physical, {true, 1});
    REQUIRE_MESSAGE(layer.has_value(), error_text(layer));
    REQUIRE(layer.value().water_bodies.size() == 3);  // two seas and the lake
    const WaterBody& lake = layer.value().water_bodies[2];
    CHECK(lake.kind == WaterKind::lake);
    CHECK(lake.surface_level != layer.value().sea_level);
    CHECK(lake.area_px == 9);
    CHECK(coast_key(layer.value()) == kLakeCoastKey);
    CHECK(coast_key(layer.value()) != kMigratedCoastKey);  // another coast than the plain migration

    // It is the in-memory construction, and loads as a map in which the lake is not a province.
    auto v1 = read_map_file(maps_dir() / "v1_tiny.pmd");
    REQUIRE(v1.has_value());
    const MigratedMap expected = with_lake(migrated(v1.value()));
    CHECK(layer.value() == expected.physical);
    auto loaded = load_map(maps_dir() / "v2_lake" / kPhysicalFileName, {true, 2});
    REQUIRE_MESSAGE(loaded.has_value(), error_text(loaded));
    auto built = build_map_data(expected.physical, expected.provinces);
    REQUIRE(built.has_value());
    CHECK(loaded.value() == built.value());
    CHECK(loaded.value().province_list.size() == 8);
}

TEST_CASE("compatibility: unknown optional chunk and a newer minor are read, the chunk is kept on re-save") {
    const Bytes plain = read_bytes(maps_dir() / "v2_migrated" / kPhysicalFileName);
    const Bytes file = read_bytes(maps_dir() / "v2_unknown_optional.pmd");

    Pmd2OpenOptions inspect;
    inspect.inspect = true;
    auto reader = Pmd2Reader::open(file, inspect);
    REQUIRE_MESSAGE(reader.has_value(), error_text(reader));
    CHECK(reader.value().header().minor == 1);
    const Pmd2ChunkInfo* extra = reader.value().find(kXopt);
    REQUIRE(extra != nullptr);
    CHECK_FALSE(extra->critical());

    auto layer = read_physical(file, {true, 1});
    REQUIRE_MESSAGE(layer.has_value(), error_text(layer));
    auto expected = read_physical(plain, {true, 1});
    REQUIRE(expected.has_value());
    CHECK(layer.value() == expected.value());  // the unknown chunk is ignored by the layer code

    // Re-saving at the container level keeps the chunk, byte for byte, and the content.
    Pmd2Writer w(Pmd2Kind::physical, reader.value().header().width, reader.value().header().height);
    w.set_bind_key(reader.value().header().bind_key);
    for (const Pmd2ChunkInfo& info : reader.value().chunks()) {
        REQUIRE(w.add_stored_chunk(info, reader.value().stored_bytes(info)).has_value());
    }
    auto resaved = w.finish();
    REQUIRE(resaved.has_value());
    CHECK(content_id_of(resaved.value()) == content_id_of(file));
    CHECK(content_id_of(file) != kMigratedPhysicalContentId);  // the extra chunk is part of the identity
    auto again = Pmd2Reader::open(resaved.value(), inspect);
    REQUIRE(again.has_value());
    CHECK(again.value().find(kXopt) != nullptr);
}

TEST_CASE("compatibility: unknown critical chunk, newer chunk version, stale binding") {
    const fs::path lake_physical = maps_dir() / "v2_lake" / kPhysicalFileName;

    auto critical = read_physical_file(maps_dir() / "v2_unknown_critical.pmd");
    REQUIRE_FALSE(critical.has_value());
    CHECK(critical.error().code == pmd2_error::kUnknownCritical);
    CHECK(critical.error().message.find("XCRT") != std::string::npos);

    auto future = read_physical_file(maps_dir() / "v2_future_chunk_version.pmd");
    REQUIRE_FALSE(future.has_value());
    CHECK(future.error().code == pmd2_error::kUnsupportedChunk);
    CHECK(future.error().message.find("HGHT") != std::string::npos);

    // Borders drawn for the plain coast next to a physical layer that has a lake: another coast.
    const fs::path stale = maps_dir() / "v2_stale_bind.pmd";
    CHECK(content_id_of(read_bytes(stale)) == kMigratedProvincesContentId);
    auto pair = load_map_set(lake_physical, stale);
    REQUIRE_FALSE(pair.has_value());
    CHECK(pair.error().code == province_error::kBindKey);
    // load_map cannot find a matching physical.pmd next to a lone provinces file.
    auto lone = load_map(stale);
    REQUIRE_FALSE(lone.has_value());
    CHECK(lone.error().code == migrate_error::kMissingSibling);
}

TEST_CASE("load_map: unknown formats, missing files and missing siblings are errors") {
    TempDir dir("pat_map_migrate_errors");
    CHECK(code_of(load_map(dir.path / "absent.pmd")) == migrate_error::kIo);

    write_bytes(dir.path / "text.pmd", Bytes{'h', 'e', 'l', 'l', 'o', ' ', 'w', 'o', 'r', 'l', 'd'});
    CHECK(code_of(load_map(dir.path / "text.pmd")) == migrate_error::kUnknownFormat);
    write_bytes(dir.path / "short.pmd", Bytes{'P', 'M', 'D', '2', 0});
    CHECK(code_of(load_map(dir.path / "short.pmd")) == migrate_error::kUnknownFormat);
    write_bytes(dir.path / "empty.pmd", Bytes{});
    CHECK(code_of(load_map(dir.path / "empty.pmd")) == migrate_error::kUnknownFormat);

    // A corrupt PMD1 keeps its PMD1 error code (the old path is used as is).
    Bytes broken = read_bytes(maps_dir() / "v1_tiny.pmd");
    broken[broken.size() / 2] ^= 0x5AU;
    write_bytes(dir.path / "broken.pmd", broken);
    const int old_code = code_of(deserialize_map(broken));
    CHECK(old_code >= 20);
    CHECK(old_code <= 45);
    CHECK(code_of(load_map(dir.path / "broken.pmd")) == old_code);

    // A physical file alone, without provinces.pmd next to it.
    fs::copy_file(maps_dir() / "v2_migrated" / kPhysicalFileName, dir.path / kPhysicalFileName);
    CHECK(code_of(load_map(dir.path / kPhysicalFileName)) == migrate_error::kMissingSibling);

    // A PMD2 file of another kind (patches) is not a map layer.
    Pmd2Writer patches(Pmd2Kind::patches, 8, 8);
    auto patch_bytes = patches.finish();
    REQUIRE_MESSAGE(patch_bytes.has_value(), error_text(patch_bytes));
    write_bytes(dir.path / "patches.pmd", patch_bytes.value());
    CHECK(code_of(load_map(dir.path / "patches.pmd")) == migrate_error::kUnsupportedKind);
}

TEST_CASE("mapgen --migrate: write_map_set creates the directory and overwrites a set") {
    TempDir dir("pat_map_migrate_set");
    auto v1 = read_map_file(maps_dir() / "v1_tiny.pmd");
    REQUIRE(v1.has_value());
    const MigratedMap m = migrated(v1.value());
    const fs::path target = dir.path / "nested" / "set";
    REQUIRE(write_map_set(target, m).has_value());
    REQUIRE(write_map_set(target, m).has_value());  // a second run replaces the files
    CHECK(fs::exists(target / kPhysicalFileName));
    CHECK(fs::exists(target / kProvincesFileName));
    CHECK(content_id_of(read_bytes(target / kPhysicalFileName)) == kMigratedPhysicalContentId);
    CHECK(content_id_of(read_bytes(target / kProvincesFileName)) == kMigratedProvincesContentId);
}

// Deliberate regeneration of the fixtures and the golden values above:
//   PAT_REGEN_MAP_FIXTURES=1 patriarch_tests -tc="fixtures: regenerate*"
// Writes tests/data/maps/ and prints the constants to paste into this file. Run it only when the
// fixture maps or the migration are changed on purpose, and explain the change in the commit.
TEST_CASE("fixtures: regenerate (only with PAT_REGEN_MAP_FIXTURES=1)") {
    if (std::getenv("PAT_REGEN_MAP_FIXTURES") == nullptr) {
        return;
    }
    const fs::path dir = maps_dir();
    const MapData v1 = generate_v1();
    REQUIRE(write_map_file(dir / "v1_tiny.pmd", v1).has_value());
    auto reread = read_map_file(dir / "v1_tiny.pmd");
    REQUIRE(reread.has_value());
    REQUIRE(reread.value() == v1);

    const MigratedMap plain = migrated(v1);
    const MigratedMap lake = with_lake(plain);
    REQUIRE(write_map_set(dir / "v2_migrated", plain).has_value());
    REQUIRE(write_map_set(dir / "v2_lake", lake).has_value());

    const Bytes plain_physical = read_bytes(dir / "v2_migrated" / kPhysicalFileName);
    write_bytes(dir / "v2_unknown_optional.pmd", make_variant(plain_physical, {1, true, false, false}));
    write_bytes(dir / "v2_unknown_critical.pmd", make_variant(plain_physical, {0, false, true, false}));
    write_bytes(dir / "v2_future_chunk_version.pmd", make_variant(plain_physical, {0, false, false, true}));
    write_bytes(dir / "v2_stale_bind.pmd", read_bytes(dir / "v2_migrated" / kProvincesFileName));

    const auto print_chunks = [](const char* name, const Bytes& bytes) {
        Pmd2OpenOptions oo;
        oo.inspect = true;
        auto reader = Pmd2Reader::open(bytes, oo);
        REQUIRE(reader.has_value());
        std::printf("%s content_id 0x%016llxULL\n",
                    name,
                    static_cast<unsigned long long>(reader.value().header().content_id));
        for (const Pmd2ChunkInfo& info : reader.value().chunks()) {
            std::printf("    {\"%s\", 0x%016llxULL},\n",
                        pmd2_chunk_id_string(info.id).c_str(),
                        static_cast<unsigned long long>(info.raw_hash));
        }
    };
    std::printf("kV1SectionHashes:");
    for (const std::uint64_t h : pmd1_section_hashes(v1)) {
        std::printf(" 0x%016llxULL,", static_cast<unsigned long long>(h));
    }
    std::printf("\nparams_hash 0x%016llxULL\ncoast_key 0x%016llxULL\nlake coast_key 0x%016llxULL\n",
                static_cast<unsigned long long>(plain.physical.provenance->steps[0].params_hash),
                static_cast<unsigned long long>(coast_key(plain.physical)),
                static_cast<unsigned long long>(coast_key(lake.physical)));
    print_chunks("migrated physical", plain_physical);
    print_chunks("migrated provinces", read_bytes(dir / "v2_migrated" / kProvincesFileName));
    print_chunks("lake physical", read_bytes(dir / "v2_lake" / kPhysicalFileName));
    print_chunks("lake provinces", read_bytes(dir / "v2_lake" / kProvincesFileName));
}
