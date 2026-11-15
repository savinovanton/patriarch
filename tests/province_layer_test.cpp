#include "pat/core/bytes.hpp"
#include "pat/mapdata/map_gen.hpp"
#include "pat/mapdata/map_validate.hpp"
#include "pat/mapdata/physical_layer.hpp"
#include "pat/mapdata/province_layer.hpp"

#include <doctest/doctest.h>

#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

using namespace pat::mapdata;

namespace {

constexpr std::uint16_t kSea = 1000;

constexpr Pmd2ChunkId kPidx = pmd2_chunk_id('P', 'I', 'D', 'X');
constexpr Pmd2ChunkId kPtbl = pmd2_chunk_id('P', 'T', 'B', 'L');
constexpr Pmd2ChunkId kProv = pmd2_chunk_id('P', 'R', 'O', 'V');
constexpr Pmd2ChunkId kXtra = pmd2_chunk_id('X', 'T', 'R', 'A');

std::string failure(const pat::core::Result<void, pat::core::Error>& r) {
    return r.has_value() ? "" : r.error().message;
}

template <class T>
std::string error_text(const pat::core::Result<T, pat::core::Error>& r) {
    return r.has_value() ? "" : r.error().message;
}

template <class T>
int code_of(const pat::core::Result<T, pat::core::Error>& r) {
    return r.has_value() ? 0 : r.error().code;
}

GeoRef albers() {
    GeoRef g;
    g.projection = Projection::albers;
    g.datum = Datum::wgs84;
    g.lat_origin_udeg = 40'000'000;
    g.lon_central_udeg = 100'000'000;
    g.lat_std1_udeg = 50'000'000;
    g.lat_std2_udeg = 65'000'000;
    g.origin_x_mm = -4'000'000'000'000;
    g.origin_y_mm = 3'500'000'000'000;
    g.pixel_size_x_mm = 2'000'000;
    g.pixel_size_y_mm = 2'000'000;
    g.bbox_min_lon_udeg = 19'000'000;
    g.bbox_max_lon_udeg = 179'000'000;
    g.bbox_min_lat_udeg = 41'000'000;
    g.bbox_max_lat_udeg = 82'000'000;
    g.proj_text = "+proj=aea";
    return g;
}

Provenance provenance() {
    Provenance p;
    p.builder = "mapgen 0123456789ab";
    p.steps.push_back({"digitise:borders-v1", 7});
    return p;
}

/// 64x48: sea band on top (id 1), a lake above sea level (id 2), land elsewhere.
PhysicalLayer make_physical() {
    PhysicalLayer l;
    l.width = 64;
    l.height = 48;
    l.sea_level = kSea;
    l.metres_per_unit_milli = 250;
    l.seed = 77;
    const std::size_t n = l.pixel_count();
    l.heights.assign(n, 0);
    l.water_body.assign(n, 0);
    l.materials.assign(n, static_cast<std::uint8_t>(Material::Plains));
    for (std::uint32_t y = 0; y < l.height; ++y) {
        for (std::uint32_t x = 0; x < l.width; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * l.width + x;
            if (y < 6) {
                l.water_body[i] = 1;
                l.heights[i] = static_cast<std::uint16_t>(400 + x);
                l.materials[i] = static_cast<std::uint8_t>(Material::DeepWater);
            } else if (x >= 20 && x < 30 && y >= 20 && y < 28) {
                l.water_body[i] = 2;
                l.heights[i] = 2400;
                l.materials[i] = static_cast<std::uint8_t>(Material::ShallowWater);
            } else {
                l.heights[i] = static_cast<std::uint16_t>(2000 + (x * 13U + y * 7U) % 900U);
            }
        }
    }
    const auto area = count_water_area(l.water_body, 2);
    l.water_bodies.push_back({1, WaterKind::sea, 0, kSea, area[0], "sea"});
    l.water_bodies.push_back({2, WaterKind::lake, 0, 2500, area[1], "lake-test"});
    l.georef = albers();
    return l;
}

/// Five provinces on the land of make_physical():
///   1: x < 20, y 6..29 (coastal)        2: x < 20, y >= 30 (inland)
///   3: x 20..39, y 6..19 (coastal)      4: x 20..39, y >= 20 (touches the lake only)
///   5: x >= 40 (coastal)
std::uint16_t province_at(std::uint32_t x, std::uint32_t y) {
    if (y < 6) {
        return 0;
    }
    if (x >= 20 && x < 30 && y >= 20 && y < 28) {
        return 0;
    }
    if (x < 20) {
        return y < 30 ? 1 : 2;
    }
    if (x < 40) {
        return y < 20 ? 3 : 4;
    }
    return 5;
}

ProvinceLayer make_provinces(const PhysicalLayer& physical) {
    ProvinceLayer p;
    p.width = physical.width;
    p.height = physical.height;
    p.bind_key = coast_key(physical);
    p.province_id.resize(physical.pixel_count());
    for (std::uint32_t y = 0; y < p.height; ++y) {
        for (std::uint32_t x = 0; x < p.width; ++x) {
            p.province_id[static_cast<std::size_t>(y) * p.width + x] = province_at(x, y);
        }
    }
    p.entries = compute_province_geometry(p.width, p.height, p.province_id, 5, sea_mask(physical));
    for (ProvinceEntry& e : p.entries) {
        e.key = "region-" + std::to_string(e.id);
        e.color_rgb = 0x102030U * e.id + 0x040506U;
    }
    p.provenance = provenance();
    return p;
}

/// PMD1 map -> physical + provinces, as the migration of Geo-13 will do (seas = 4-connected water).
std::pair<PhysicalLayer, ProvinceLayer> split(const MapData& map) {
    PhysicalLayer l;
    l.width = map.width;
    l.height = map.height;
    l.sea_level = map.sea_level;
    l.metres_per_unit_milli = map.metres_per_unit_milli;
    l.seed = map.seed;
    l.heights = map.heights;
    l.materials = map.materials;
    l.water_body.assign(map.pixel_count(), 0);
    std::uint16_t next = 0;
    std::vector<std::uint32_t> stack;
    for (std::size_t s = 0; s < map.pixel_count(); ++s) {
        if (map.heights[s] > map.sea_level || l.water_body[s] != 0) {
            continue;
        }
        ++next;
        l.water_body[s] = next;
        stack.push_back(static_cast<std::uint32_t>(s));
        while (!stack.empty()) {
            const std::uint32_t i = stack.back();
            stack.pop_back();
            const std::uint32_t x = i % map.width;
            const auto go = [&](std::size_t j) {
                if (map.heights[j] <= map.sea_level && l.water_body[j] == 0) {
                    l.water_body[j] = next;
                    stack.push_back(static_cast<std::uint32_t>(j));
                }
            };
            if (x > 0) {
                go(i - 1);
            }
            if (x + 1 < map.width) {
                go(i + 1);
            }
            if (i >= map.width) {
                go(i - map.width);
            }
            if (i + map.width < map.pixel_count()) {
                go(i + map.width);
            }
        }
    }
    const auto area = count_water_area(l.water_body, next);
    for (std::uint16_t id = 1; id <= next; ++id) {
        l.water_bodies.push_back(
            {id, WaterKind::sea, 0, map.sea_level, area[id - 1U], "sea-" + std::to_string(id)});
    }

    ProvinceLayer p;
    p.width = map.width;
    p.height = map.height;
    p.bind_key = coast_key(l);
    p.province_id = map.provinces;
    for (const Province& src : map.province_list) {
        ProvinceEntry e;
        e.id = src.id;
        char key[16];
        std::snprintf(key, sizeof key, "p%04u", static_cast<unsigned>(src.id));
        e.key = key;
        e.color_rgb = src.color_rgb;
        e.area_px = src.area_px;
        e.min_x = src.min_x;
        e.min_y = src.min_y;
        e.max_x = src.max_x;
        e.max_y = src.max_y;
        e.centroid_x = src.centroid_x;
        e.centroid_y = src.centroid_y;
        e.coastal = src.coastal;
        e.neighbors = src.neighbors;
        p.entries.push_back(std::move(e));
    }
    return {std::move(l), std::move(p)};
}

std::vector<std::uint8_t> bytes_of(const ProvinceLayer& p) {
    auto r = serialize_provinces(p);
    REQUIRE_MESSAGE(r.has_value(), error_text(r));
    return r.value();
}

std::vector<std::uint8_t> bytes_of(const PhysicalLayer& l) {
    auto r = serialize_physical(l);
    REQUIRE_MESSAGE(r.has_value(), error_text(r));
    return r.value();
}

/// Hand-built provinces container for the reader tests.
struct Tweaks {
    Pmd2Kind kind = Pmd2Kind::provinces;
    std::optional<std::uint64_t> bind_key;
    std::optional<Pmd2ChunkId> skip;
    std::optional<std::vector<std::uint8_t>> ptbl;
    std::uint8_t pidx_shift = 0;
    bool unknown_optional = false;
    bool unknown_critical = false;
};

std::vector<std::uint8_t> build_file(const ProvinceLayer& l, const Tweaks& t) {
    Pmd2Writer w(t.kind, l.width, l.height);
    w.set_bind_key(t.bind_key.value_or(l.bind_key));
    Pmd2ChunkSpec spec;
    if (!(t.skip && *t.skip == kPidx)) {
        spec.id = kPidx;
        Pmd2RasterParams params;
        params.shift = t.pidx_shift;
        if (t.pidx_shift == 0) {
            REQUIRE(w.add_raster(spec, params, std::span<const std::uint16_t>(l.province_id)).has_value());
        } else {
            const std::vector<std::uint16_t> small((l.width / 2U) * (l.height / 2U), 1);
            REQUIRE(w.add_raster(spec, params, std::span<const std::uint16_t>(small)).has_value());
        }
    }
    if (!(t.skip && *t.skip == kPtbl)) {
        spec.id = kPtbl;
        REQUIRE(w.add_blob(spec, t.ptbl.value_or(encode_province_table(l.entries))).has_value());
    }
    if (l.provenance && !(t.skip && *t.skip == kProv)) {
        spec.id = kProv;
        spec.critical = false;
        REQUIRE(w.add_blob(spec, encode_provenance(*l.provenance)).has_value());
    }
    if (t.unknown_optional) {
        spec.id = kXtra;
        spec.critical = false;
        REQUIRE(w.add_blob(spec, std::vector<std::uint8_t>{1, 2, 3}).has_value());
    }
    if (t.unknown_critical) {
        spec.id = kXtra;
        spec.critical = true;
        REQUIRE(w.add_blob(spec, std::vector<std::uint8_t>{1, 2, 3}).has_value());
    }
    auto bytes = w.finish();
    REQUIRE(bytes.has_value());
    return bytes.value();
}

std::filesystem::path temp_path(const char* name) {
    return std::filesystem::temp_directory_path() / name;
}

}  // namespace

TEST_CASE("provinces: the sample pair is valid and has the expected geometry") {
    const PhysicalLayer physical = make_physical();
    const ProvinceLayer p = make_provinces(physical);
    CHECK_MESSAGE(validate_province_layer(p).has_value(), failure(validate_province_layer(p)));
    CHECK_MESSAGE(validate_provinces(physical, p).has_value(), failure(validate_provinces(physical, p)));
    REQUIRE(p.entries.size() == 5);
    // Coastal means "touches a sea": province 4 touches only the lake, 2 only other provinces.
    CHECK(p.entries[0].coastal);
    CHECK_FALSE(p.entries[1].coastal);
    CHECK(p.entries[2].coastal);
    CHECK_FALSE(p.entries[3].coastal);
    CHECK(p.entries[4].coastal);
    CHECK(p.entries[0].min_y == 6);
    CHECK(p.entries[4].max_x == 63);
    CHECK(p.entries[0].neighbors.size() == 3);  // 2, 3, 4
    std::uint32_t total = 0;
    for (const ProvinceEntry& e : p.entries) {
        total += e.area_px;
    }
    CHECK(total ==
          physical.pixel_count() - physical.water_bodies[0].area_px - physical.water_bodies[1].area_px);
}

TEST_CASE("provinces: round trip in memory and through a file") {
    const PhysicalLayer physical = make_physical();
    const ProvinceLayer p = make_provinces(physical);
    const auto bytes = bytes_of(p);
    auto back = read_provinces(bytes);
    REQUIRE_MESSAGE(back.has_value(), error_text(back));
    CHECK(back.value() == p);
    CHECK(back.value().bind_key == coast_key(physical));
    CHECK(read_provinces(bytes, {true, 2}).has_value());  // strict mode, two threads

    const auto path = temp_path("patriarch_province_layer_test.pmd");
    REQUIRE(write_provinces_file(path, p).has_value());
    auto from_file = read_provinces_file(path);
    auto table = load_province_table(path);
    std::error_code ec;
    std::filesystem::remove(path, ec);
    REQUIRE(from_file.has_value());
    CHECK(from_file.value() == p);
    REQUIRE(table.has_value());
    CHECK(table.value().entries == p.entries);
    CHECK_FALSE(read_provinces_file(path).has_value());  // removed: IO error, not a crash
    CHECK_FALSE(load_province_table(path).has_value());
}

TEST_CASE("provinces: output content does not depend on threads or codec") {
    const ProvinceLayer p = make_provinces(make_physical());
    Pmd2WriterOptions one;
    Pmd2WriterOptions many;
    many.threads = 4;
    Pmd2WriterOptions raw;
    raw.codec = Pmd2Codec::raw;
    auto a = serialize_provinces(p, one);
    auto b = serialize_provinces(p, many);
    auto c = serialize_provinces(p, raw);
    REQUIRE((a.has_value() && b.has_value() && c.has_value()));
    CHECK(a.value() == b.value());
    auto ra = load_province_table(a.value());
    auto rc = load_province_table(c.value());
    REQUIRE((ra.has_value() && rc.has_value()));
    CHECK(ra.value().content_id == rc.value().content_id);
    CHECK(read_provinces(c.value()).value() == p);
}

TEST_CASE("provinces: the table alone loads without decoding the raster") {
    const PhysicalLayer physical = make_physical();
    const ProvinceLayer p = make_provinces(physical);
    const auto bytes = bytes_of(p);
    auto table = load_province_table(bytes);
    REQUIRE(table.has_value());
    CHECK(table.value().entries == p.entries);
    CHECK(table.value().width == p.width);
    CHECK(table.value().height == p.height);
    CHECK(table.value().bind_key == coast_key(physical));
    // A damaged PIDX tile does not matter here (the raster is not decoded), while the full reader
    // catches it; damage to the table itself is caught when the container is opened.
    {
        Pmd2OpenOptions o;
        o.inspect = true;
        auto reader = Pmd2Reader::open(bytes, o);
        REQUIRE(reader.has_value());
        const Pmd2ChunkInfo* pidx = reader.value().find(kPidx);
        const Pmd2ChunkInfo* ptbl = reader.value().find(kPtbl);
        REQUIRE((pidx != nullptr && ptbl != nullptr));
        auto bad_tile = bytes;
        bad_tile[pidx->offset + pidx->stored_size - 1U] ^= 0x5AU;
        CHECK(load_province_table(bad_tile).has_value());
        CHECK_FALSE(read_provinces(bad_tile).has_value());
        auto bad_table = bytes;
        bad_table[ptbl->offset + ptbl->stored_size / 2U] ^= 0x5AU;
        CHECK_FALSE(load_province_table(bad_table).has_value());
    }
    // The table of a physical container is refused.
    CHECK(code_of(load_province_table(bytes_of(physical))) == province_error::kWrongKind);
    // Table-level problems are reported by the table loader too.
    ProvinceLayer dup = p;
    dup.entries[1].key = dup.entries[0].key;
    CHECK(code_of(load_province_table(build_file(dup, {}))) == province_error::kBadKey);
}

TEST_CASE("provinces: rebuilding the physical layer without changing the coast needs no new province file") {
    const PhysicalLayer physical = make_physical();
    const ProvinceLayer p = make_provinces(physical);
    const auto province_bytes = bytes_of(p);

    PhysicalLayer restyled = physical;  // new relief, materials, scale, seed and provenance
    for (std::size_t i = 0; i < restyled.heights.size(); ++i) {
        if (restyled.water_body[i] == 0) {
            restyled.heights[i] = static_cast<std::uint16_t>(restyled.heights[i] + 37U + (i % 5U));
        }
    }
    restyled.materials[300] = static_cast<std::uint8_t>(Material::Rock);
    restyled.metres_per_unit_milli = 500;
    restyled.seed = 9;
    restyled.provenance = provenance();
    const auto restyled_bytes = bytes_of(restyled);
    const auto original_bytes = bytes_of(physical);
    CHECK(coast_key(restyled) == coast_key(physical));
    CHECK(restyled_bytes != original_bytes);

    auto before = load_map_set(original_bytes, province_bytes);
    auto after = load_map_set(restyled_bytes, province_bytes);
    REQUIRE_MESSAGE(before.has_value(), error_text(before));
    REQUIRE_MESSAGE(after.has_value(), error_text(after));
    CHECK(after.value().provinces == before.value().provinces);
    CHECK(after.value().heights != before.value().heights);
    CHECK(after.value().province_list[0].mean_height != before.value().province_list[0].mean_height);
    CHECK(after.value().metres_per_unit_milli == 500);
    CHECK(after.value().seed == 9);
}

TEST_CASE("provinces: changing the coast is a binding error") {
    const PhysicalLayer physical = make_physical();
    const ProvinceLayer p = make_provinces(physical);
    const auto province_bytes = bytes_of(p);

    SUBCASE("one more water pixel (a new pond)") {
        PhysicalLayer other = physical;
        const std::size_t i = other.pixel_count() - 1;
        other.water_body[i] = 3;
        other.heights[i] = 10;
        other.water_bodies.push_back({3, WaterKind::lake, 0, 20, 1, "pond"});
        const auto r = load_map_set(bytes_of(other), province_bytes);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().code == province_error::kBindKey);
        CHECK(code_of(build_map_data(other, p)) == province_error::kBindKey);
        CHECK(code_of(validate_provinces(other, p)) == province_error::kBindKey);
    }
    SUBCASE("a different georeference") {
        PhysicalLayer other = physical;
        other.georef.pixel_size_x_mm += 1;
        const auto r = load_map_set(bytes_of(other), province_bytes);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().code == province_error::kBindKey);
    }
    SUBCASE("a province file stamped with a foreign key") {
        Tweaks t;
        t.bind_key = coast_key(physical) ^ 1U;
        const auto r = load_map_set(bytes_of(physical), build_file(p, t));
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().code == province_error::kBindKey);
    }
    SUBCASE("a different size") {
        PhysicalLayer other = make_physical();
        other.width = 65;
        other.height = 48;
        // Rebuild a consistent 65-wide layer is not needed: the size check comes before decoding.
        ProvinceLayer wide = p;
        wide.width = 65;
        CHECK(code_of(validate_provinces(other, p)) == province_error::kLayerSize);
        CHECK(code_of(validate_provinces(physical, wide)) == province_error::kLayerSize);
    }
}

TEST_CASE("provinces: province != 0 exactly where WBOD == 0") {
    const PhysicalLayer physical = make_physical();
    const ProvinceLayer base = make_provinces(physical);
    {
        ProvinceLayer p = base;
        p.province_id[2] = 1;  // a sea pixel (row 0) claimed by a province
        CHECK(code_of(validate_provinces(physical, p)) == province_error::kLandMask);
    }
    {
        ProvinceLayer p = base;
        const std::size_t lake = 22U * physical.width + 25U;  // a lake pixel
        REQUIRE(physical.water_body[lake] == 2);
        p.province_id[lake] = 4;
        CHECK(code_of(validate_provinces(physical, p)) == province_error::kLandMask);
    }
    {
        ProvinceLayer p = base;
        const std::size_t land = 30U * physical.width + 50U;  // a land pixel left unassigned
        p.province_id[land] = 0;
        CHECK(code_of(validate_provinces(physical, p)) == province_error::kLandMask);
    }
    {
        // The same raster is fine on its own (the mask needs WBOD), but the table must follow it.
        ProvinceLayer p = base;
        p.province_id[30U * physical.width + 50U] = 0;
        CHECK(code_of(validate_province_layer(p)) == province_error::kTableMismatch);
    }
}

TEST_CASE("provinces: key validation (owner decision 201)") {
    const ProvinceLayer base = make_provinces(make_physical());
    const auto with_key = [&](const std::string& key) {
        ProvinceLayer p = base;
        p.entries[2].key = key;
        return p;
    };
    CHECK(valid_province_key("p0001"));
    CHECK(valid_province_key("novgorod-guberniya"));
    CHECK(valid_province_key(std::string(kMaxProvinceKeyLength, 'a')));
    CHECK_FALSE(valid_province_key(""));
    CHECK_FALSE(valid_province_key(std::string(kMaxProvinceKeyLength + 1, 'a')));
    CHECK_FALSE(valid_province_key("Moscow"));
    CHECK_FALSE(valid_province_key("two words"));
    CHECK_FALSE(valid_province_key("under_score"));
    CHECK_FALSE(valid_province_key("caf\xC3\xA9"));

    CHECK(validate_province_layer(with_key("region-0003")).has_value());
    for (const std::string& bad : {std::string(),
                                   std::string("UPPER"),
                                   std::string("a b"),
                                   std::string("a_b"),
                                   std::string("a.b"),
                                   std::string(kMaxProvinceKeyLength + 1, 'x'),
                                   std::string("a\0b", 3)}) {
        CAPTURE(bad);
        CHECK(code_of(validate_province_layer(with_key(bad))) == province_error::kBadKey);
        CHECK_FALSE(serialize_provinces(with_key(bad)).has_value());
    }
    // Duplicate key.
    CHECK(code_of(validate_province_layer(with_key(base.entries[0].key))) == province_error::kBadKey);

    // A file that carries a bad key is refused on reading, too.
    ProvinceLayer broken = with_key("fine");
    broken.entries[2].key = "BAD";
    Tweaks t;
    t.ptbl = encode_province_table(broken.entries);
    CHECK(code_of(read_provinces(build_file(base, t))) == province_error::kBadKey);
}

TEST_CASE("provinces: table validation") {
    const ProvinceLayer base = make_provinces(make_physical());
    const auto check = [&](auto&& mutate, int code) {
        ProvinceLayer p = base;
        mutate(p);
        CAPTURE(code);
        CHECK(code_of(validate_province_layer(p)) == code);
    };
    check([](ProvinceLayer& p) { p.entries[1].id = 7; }, province_error::kBadId);
    check([](ProvinceLayer& p) { p.entries[1].color_rgb = p.entries[0].color_rgb; },
          province_error::kDuplicateColor);
    check([](ProvinceLayer& p) { p.entries[3].area_px += 1; }, province_error::kTableMismatch);
    check([](ProvinceLayer& p) { p.entries[0].centroid_x = 63; }, province_error::kBadTable);
    check([](ProvinceLayer& p) { p.entries[0].max_x = 200; }, province_error::kBadTable);
    check([](ProvinceLayer& p) { p.entries[0].area_px = 0; }, province_error::kBadTable);
    check([](ProvinceLayer& p) { p.entries[0].neighbors.pop_back(); },
          province_error::kBadTable);  // asymmetric
    check([](ProvinceLayer& p) { p.entries[0].neighbors[0].length_px += 1; }, province_error::kBadTable);
    check([](ProvinceLayer& p) { p.entries[0].neighbors[0].neighbor = 1; }, province_error::kBadTable);
    check([](ProvinceLayer& p) { std::swap(p.entries[0].neighbors[0], p.entries[0].neighbors[1]); },
          province_error::kBadTable);
    check([](ProvinceLayer& p) { p.entries.clear(); }, province_error::kBadTable);
    check([](ProvinceLayer& p) { p.province_id.pop_back(); }, province_error::kLayerSize);
    check([](ProvinceLayer& p) { p.province_id[100] = 9; }, province_error::kBadId);
    check([](ProvinceLayer& p) { p.provenance->builder = "bad\npath"; }, physical_error::kBadProvenance);

    // The coastal flag is checked against WBOD, so only with the physical layer.
    const PhysicalLayer physical = make_physical();
    ProvinceLayer p = base;
    p.entries[0].coastal = false;
    CHECK(validate_province_layer(p).has_value());
    CHECK(code_of(validate_provinces(physical, p)) == province_error::kTableMismatch);
    p = base;
    p.entries[3].coastal = true;  // touches the lake only
    CHECK(code_of(validate_provinces(physical, p)) == province_error::kTableMismatch);
}

TEST_CASE("provinces: split province, empty province and disconnected graph") {
    {
        // Province 5 also owns an island pixel cut off by province 4 (4-connectivity).
        PhysicalLayer physical = make_physical();
        ProvinceLayer p = make_provinces(physical);
        p.province_id[static_cast<std::size_t>(40) * physical.width + 30] = 5;
        p.entries = compute_province_geometry(p.width, p.height, p.province_id, 5, sea_mask(physical));
        for (ProvinceEntry& e : p.entries) {
            e.key = "region-" + std::to_string(e.id);
            e.color_rgb = 0x102030U * e.id + 0x040506U;
        }
        CHECK(code_of(validate_province_layer(p)) == province_error::kProvinceShape);
    }
    {
        // A sixth province in the table without pixels.
        ProvinceLayer p = make_provinces(make_physical());
        ProvinceEntry e = p.entries[4];
        e.id = 6;
        e.key = "ghost";
        e.color_rgb = 0xABCDEF;
        e.neighbors.clear();
        p.entries.push_back(e);
        CHECK_FALSE(validate_province_layer(p).has_value());
    }
    {
        // Two islands of provinces with no shared edge: the graph is not connected.
        PhysicalLayer l;
        l.width = 8;
        l.height = 4;
        l.sea_level = kSea;
        l.metres_per_unit_milli = 1000;
        l.heights.assign(32, 2000);
        l.water_body.assign(32, 0);
        l.materials.assign(32, static_cast<std::uint8_t>(Material::Plains));
        for (std::uint32_t y = 0; y < 4; ++y) {
            l.water_body[y * 8U + 3] = 1;
            l.water_body[y * 8U + 4] = 1;
            l.heights[y * 8U + 3] = l.heights[y * 8U + 4] = 500;
            l.materials[y * 8U + 3] = l.materials[y * 8U + 4] =
                static_cast<std::uint8_t>(Material::DeepWater);
        }
        l.water_bodies.push_back({1, WaterKind::sea, 0, kSea, 8, "strait"});
        ProvinceLayer p;
        p.width = 8;
        p.height = 4;
        p.bind_key = coast_key(l);
        p.province_id.assign(32, 0);
        for (std::uint32_t y = 0; y < 4; ++y) {
            for (std::uint32_t x = 0; x < 8; ++x) {
                if (x < 3) {
                    p.province_id[y * 8U + x] = 1;
                } else if (x > 4) {
                    p.province_id[y * 8U + x] = 2;
                }
            }
        }
        p.entries = compute_province_geometry(8, 4, p.province_id, 2, sea_mask(l));
        p.entries[0].key = "west";
        p.entries[0].color_rgb = 1;
        p.entries[1].key = "east";
        p.entries[1].color_rgb = 2;
        CHECK(code_of(validate_province_layer(p)) == province_error::kGraphDisconnected);
    }
}

TEST_CASE("provinces: structural errors of the reader") {
    const ProvinceLayer p = make_provinces(make_physical());
    REQUIRE(read_provinces(build_file(p, {})).has_value());

    SUBCASE("missing required chunks") {
        for (const Pmd2ChunkId id : {kPidx, kPtbl}) {
            Tweaks t;
            t.skip = id;
            CAPTURE(pmd2_chunk_id_string(id));
            CHECK(code_of(read_provinces(build_file(p, t))) == province_error::kMissingChunk);
            CHECK(code_of(load_province_table(build_file(p, t))) == province_error::kMissingChunk);
        }
    }
    SUBCASE("optional PROV may be absent") {
        Tweaks t;
        t.skip = kProv;
        const auto r = read_provinces(build_file(p, t));
        REQUIRE(r.has_value());
        CHECK_FALSE(r.value().provenance.has_value());
    }
    SUBCASE("wrong container kind") {
        Tweaks t;
        t.kind = Pmd2Kind::patches;
        CHECK(code_of(read_provinces(build_file(p, t))) == province_error::kWrongKind);
        CHECK(code_of(read_provinces(bytes_of(make_physical()))) == province_error::kWrongKind);
        CHECK_FALSE(read_physical(bytes_of(p)).has_value());
    }
    SUBCASE("PIDX must be a full-resolution raster") {
        Tweaks t;
        t.pidx_shift = 1;
        CHECK(code_of(read_provinces(build_file(p, t))) == province_error::kLayerSize);
    }
    SUBCASE("unknown optional chunks are skipped, unknown critical ones are errors") {
        Tweaks t;
        t.unknown_optional = true;
        const auto ok = read_provinces(build_file(p, t));
        REQUIRE(ok.has_value());
        CHECK(ok.value() == p);
        Tweaks c;
        c.unknown_critical = true;
        CHECK(code_of(read_provinces(build_file(p, c))) == pmd2_error::kUnknownCritical);
    }
    SUBCASE("a province file with a bad PTBL blob") {
        Tweaks t;
        auto blob = encode_province_table(p.entries);
        blob.push_back(0);
        t.ptbl = blob;
        CHECK(code_of(read_provinces(build_file(p, t))) == province_error::kBadChunk);
        blob.resize(blob.size() - 8);
        t.ptbl = blob;
        CHECK(code_of(read_provinces(build_file(p, t))) == province_error::kBadChunk);
    }
}

TEST_CASE("provinces: PTBL codec round trip and rejection of damage") {
    const ProvinceLayer p = make_provinces(make_physical());
    const auto blob = encode_province_table(p.entries);
    auto back = decode_province_table(blob);
    REQUIRE(back.has_value());
    CHECK(back.value() == p.entries);
    for (std::size_t n = 0; n < blob.size(); ++n) {
        const auto r = decode_province_table(std::span<const std::uint8_t>(blob.data(), n));
        CAPTURE(n);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().code == province_error::kBadChunk);
    }
    auto longer = blob;
    longer.push_back(1);
    CHECK(code_of(decode_province_table(longer)) == province_error::kBadChunk);
    CHECK(decode_province_table(longer, true).has_value());  // a newer minor may append fields
    // A count that promises more records than the bytes can hold must not allocate or crash.
    std::vector<std::uint8_t> huge = {0xFF, 0xFF, 0, 0};
    CHECK(code_of(decode_province_table(huge)) == province_error::kBadChunk);
    // Unknown flag bits.
    auto flagged = blob;
    // flags byte of record 1: 2 (count) + 2 (id) + 2 + key + 4 + 4 + 12 bytes in.
    const std::size_t flag_at = 2 + 2 + 2 + p.entries[0].key.size() + 4 + 4 + 12;
    flagged[flag_at] = 0x80;
    CHECK(code_of(decode_province_table(flagged)) == province_error::kBadTable);
}

TEST_CASE("provinces: every truncation and every flipped byte is an error, never a crash") {
    const PhysicalLayer physical = make_physical();
    const ProvinceLayer p = make_provinces(physical);
    const auto bytes = bytes_of(p);
    const auto physical_bytes = bytes_of(physical);
    for (std::size_t n = 0; n < bytes.size(); n += 3) {
        const auto r = read_provinces(std::span<const std::uint8_t>(bytes.data(), n));
        CAPTURE(n);
        CHECK_FALSE(r.has_value());
        CHECK_FALSE(load_province_table(std::span<const std::uint8_t>(bytes.data(), n)).has_value());
        CHECK_FALSE(load_map_set(physical_bytes, std::span<const std::uint8_t>(bytes.data(), n)).has_value());
    }
    for (std::size_t i = 0; i < bytes.size(); i += 2) {
        auto bad = bytes;
        bad[i] ^= 0x01U;
        CAPTURE(i);
        CHECK_FALSE(read_provinces(bad).has_value());
        CHECK_FALSE(load_map_set(physical_bytes, bad).has_value());
    }
}

TEST_CASE("provinces: load_map_set assembles MapData and recomputes mean_height") {
    const PhysicalLayer physical = make_physical();
    const ProvinceLayer p = make_provinces(physical);
    auto map = load_map_set(bytes_of(physical), bytes_of(p));
    REQUIRE_MESSAGE(map.has_value(), error_text(map));
    const MapData& m = map.value();
    CHECK(m.width == physical.width);
    CHECK(m.sea_level == kSea);
    CHECK(m.metres_per_unit_milli == 250);
    CHECK(m.seed == 77);
    CHECK(m.heights == physical.heights);
    CHECK(m.materials == physical.materials);
    CHECK(m.provinces == p.province_id);
    REQUIRE(m.province_list.size() == 5);
    for (const Province& pr : m.province_list) {
        std::uint64_t sum = 0;
        std::uint64_t count = 0;
        for (std::size_t i = 0; i < m.provinces.size(); ++i) {
            if (m.provinces[i] == pr.id) {
                sum += m.heights[i];
                ++count;
            }
        }
        CAPTURE(pr.id);
        CHECK(count == pr.area_px);
        CHECK(pr.mean_height == sum / count);
        const ProvinceEntry& e = p.entries[pr.id - 1U];
        CHECK(pr.color_rgb == e.color_rgb);
        CHECK(pr.coastal == e.coastal);
        CHECK(pr.neighbors == e.neighbors);
    }
    // The in-memory path gives the same map; the file path too.
    auto direct = build_map_data(physical, p);
    REQUIRE(direct.has_value());
    CHECK(direct.value() == m);
    const auto pp = temp_path("patriarch_province_set_physical.pmd");
    const auto pv = temp_path("patriarch_province_set_provinces.pmd");
    REQUIRE(write_physical_file(pp, physical).has_value());
    REQUIRE(write_provinces_file(pv, p).has_value());
    auto from_files = load_map_set(pp, pv, {true, 0});
    std::error_code ec;
    std::filesystem::remove(pp, ec);
    std::filesystem::remove(pv, ec);
    REQUIRE(from_files.has_value());
    CHECK(from_files.value() == m);
    CHECK_FALSE(load_map_set(pp, pv).has_value());
}

TEST_CASE("provinces: a PMD1-generated map split into two layers loads back equal") {
    struct Case {
        std::uint32_t w, h;
        std::uint64_t seed;
        std::uint32_t provinces;
    };
    for (const Case c : {Case{96, 48, 3, 8}, Case{256, 128, 5, 20}, Case{200, 120, 11, 14}}) {
        GenParams gp;
        gp.width = c.w;
        gp.height = c.h;
        gp.seed = c.seed;
        gp.province_count = c.provinces;
        gp.threads = 1;
        const MapData original = generate_map(gp);
        REQUIRE_MESSAGE(validate_map(original).has_value(), failure(validate_map(original)));
        const auto [physical, provinces] = split(original);
        CAPTURE(c.seed);
        REQUIRE_MESSAGE(validate_physical(physical).has_value(), failure(validate_physical(physical)));
        REQUIRE_MESSAGE(validate_provinces(physical, provinces).has_value(),
                        failure(validate_provinces(physical, provinces)));
        auto map = load_map_set(bytes_of(physical), bytes_of(provinces), {true, 2});
        REQUIRE_MESSAGE(map.has_value(), error_text(map));
        CHECK(map.value() == original);
        CHECK(validate_map(map.value()).has_value());
        // The geometry derived from the raster is what the generator stored.
        const auto geometry = compute_province_geometry(original.width,
                                                        original.height,
                                                        original.provinces,
                                                        original.province_list.size(),
                                                        sea_mask(physical));
        for (std::size_t i = 0; i < geometry.size(); ++i) {
            CHECK(geometry[i].area_px == provinces.entries[i].area_px);
            CHECK(geometry[i].coastal == provinces.entries[i].coastal);
            CHECK(geometry[i].neighbors == provinces.entries[i].neighbors);
        }
    }
}
