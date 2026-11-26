#include "pat/core/bytes.hpp"
#include "pat/mapdata/map_gen.hpp"
#include "pat/mapdata/physical_layer.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "map_fixture.hpp"

using namespace pat::mapdata;

namespace {

constexpr std::uint16_t kSea = 1000;

constexpr Pmd2ChunkId kGrid = pmd2_chunk_id('G', 'R', 'I', 'D');
constexpr Pmd2ChunkId kGeor = pmd2_chunk_id('G', 'E', 'O', 'R');
constexpr Pmd2ChunkId kHght = pmd2_chunk_id('H', 'G', 'H', 'T');
constexpr Pmd2ChunkId kWbod = pmd2_chunk_id('W', 'B', 'O', 'D');
constexpr Pmd2ChunkId kWtbl = pmd2_chunk_id('W', 'T', 'B', 'L');
constexpr Pmd2ChunkId kMatl = pmd2_chunk_id('M', 'A', 'T', 'L');
constexpr Pmd2ChunkId kProv = pmd2_chunk_id('P', 'R', 'O', 'V');
constexpr Pmd2ChunkId kXtra = pmd2_chunk_id('X', 'T', 'R', 'A');

GeoRef albers() {
    GeoRef g;
    g.projection = Projection::albers;
    g.datum = Datum::wgs84;
    g.lat_origin_udeg = 40'000'000;
    g.lon_central_udeg = 100'000'000;
    g.lat_std1_udeg = 50'000'000;
    g.lat_std2_udeg = 65'000'000;
    g.false_easting_mm = -1'500'000'000;
    g.false_northing_mm = 250'000'000;
    g.origin_x_mm = -4'000'000'000'000;
    g.origin_y_mm = 3'500'000'000'000;
    g.pixel_size_x_mm = 2'000'000;
    g.pixel_size_y_mm = 2'000'000;
    g.bbox_min_lon_udeg = 19'000'000;
    g.bbox_max_lon_udeg = 179'000'000;
    g.bbox_min_lat_udeg = 41'000'000;
    g.bbox_max_lat_udeg = 82'000'000;
    g.epsg = 0;
    g.proj_text = "+proj=aea +lat_1=50 +lat_2=65 +lat_0=40 +lon_0=100";
    return g;
}

Provenance provenance() {
    Provenance p;
    p.builder = "mapgen 0123456789ab";
    p.manifest_version = 3;
    ProvSource s;
    s.source_id = "copernicus-dem-glo30";
    s.source_version = "2021";
    ProvFile f;
    f.path = "dem/tile-n55e037.tif";
    for (std::size_t i = 0; i < f.sha256.size(); ++i) {
        f.sha256[i] = static_cast<std::uint8_t>(i * 7U + 1U);
    }
    s.files.push_back(f);
    p.sources.push_back(s);
    p.steps.push_back({"reproject:albers-v1", 0x1122334455667788ULL});
    p.steps.push_back({"resample:64x48", 42});
    p.corrections.push_back({"fix-azov", 0xABCDEF0123456789ULL});
    return p;
}

/// 64x48: sea band on top (id 1), a lake above sea level (id 2), land elsewhere (see map_fixture.hpp).
PhysicalLayer make_layer() {
    PhysicalLayer l = pat::test::make_test_terrain(/*varied_lake_bed=*/true);
    l.georef = albers();
    l.provenance = provenance();
    return l;
}

pat::core::Result<Pmd2Reader, pat::core::Error> open_any(std::span<const std::uint8_t> bytes) {
    Pmd2OpenOptions o;
    o.inspect = true;
    return Pmd2Reader::open(bytes, o);
}

std::string failure(const pat::core::Result<void, pat::core::Error>& r) {
    return r.has_value() ? "" : r.error().message;
}

int code_of(const pat::core::Result<void, pat::core::Error>& r) {
    return r.has_value() ? 0 : r.error().code;
}

/// Hand-built file for the reader tests: lets a test tamper with one chunk or the header.
struct Tweaks {
    Pmd2Kind kind = Pmd2Kind::physical;
    std::optional<std::uint64_t> bind_key;
    std::optional<Pmd2ChunkId> skip;
    std::optional<std::vector<std::uint8_t>> grid;
    std::optional<std::vector<std::uint8_t>> wtbl;
    std::optional<std::vector<std::uint8_t>> geor;
    std::optional<std::vector<std::uint8_t>> prov;
    std::optional<std::uint16_t> matl_version;
    std::optional<std::uint32_t> header_width;
    bool unknown_optional = false;
    bool unknown_critical = false;
};

std::vector<std::uint8_t> grid_blob(const PhysicalLayer& l, std::uint32_t width, std::uint16_t version = 1) {
    pat::core::ByteWriter w;
    w.u16(version);
    w.u32(width);
    w.u32(l.height);
    w.u16(l.sea_level);
    w.u32(l.metres_per_unit_milli);
    w.u64(l.seed);
    w.u16(l.material_version);
    return w.take();
}

std::vector<std::uint8_t> build_file(const PhysicalLayer& l, const Tweaks& t) {
    Pmd2WriterOptions o;
    Pmd2Writer w(t.kind, l.width, l.height, o);
    w.set_bind_key(t.bind_key.value_or(coast_key(l)));
    Pmd2ChunkSpec spec;
    const Pmd2RasterParams params;
    const auto blob = [&](Pmd2ChunkId id, const std::vector<std::uint8_t>& bytes, bool critical = true) {
        if (t.skip && *t.skip == id) {
            return;
        }
        spec.id = id;
        spec.critical = critical;
        spec.version = 1;
        REQUIRE(w.add_blob(spec, bytes).has_value());
    };
    blob(kGrid, t.grid.value_or(grid_blob(l, l.width)));
    blob(kGeor, t.geor.value_or(encode_georef(l.georef)));
    blob(kWtbl, t.wtbl.value_or(encode_water_table(l.water_bodies)));
    if (!(t.skip && *t.skip == kHght)) {
        spec.id = kHght;
        spec.critical = true;
        spec.version = 1;
        REQUIRE(w.add_raster(spec, params, std::span<const std::uint16_t>(l.heights)).has_value());
    }
    if (!(t.skip && *t.skip == kWbod)) {
        spec.id = kWbod;
        spec.critical = true;
        spec.version = 1;
        REQUIRE(w.add_raster(spec, params, std::span<const std::uint16_t>(l.water_body)).has_value());
    }
    if (!(t.skip && *t.skip == kMatl)) {
        spec.id = kMatl;
        spec.critical = true;
        spec.version = t.matl_version.value_or(1);
        REQUIRE(w.add_raster(spec, params, std::span<const std::uint8_t>(l.materials)).has_value());
    }
    if (t.prov) {
        blob(kProv, *t.prov, false);
    } else if (l.provenance) {
        blob(kProv, encode_provenance(*l.provenance), false);
    }
    if (t.unknown_optional) {
        blob(kXtra, {1, 2, 3}, false);
    }
    if (t.unknown_critical) {
        blob(kXtra, {1, 2, 3}, true);
    }
    auto bytes = w.finish();
    REQUIRE(bytes.has_value());
    return bytes.value();
}

PhysicalLayer from_procedural(const MapData& map) {
    PhysicalLayer l;
    l.width = map.width;
    l.height = map.height;
    l.sea_level = map.sea_level;
    l.metres_per_unit_milli = map.metres_per_unit_milli;
    l.seed = map.seed;
    l.heights = map.heights;
    l.materials = map.materials;
    l.water_body.assign(map.pixel_count(), 0);
    // Water is height <= sea_level; label the 4-connected components as separate seas.
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
    return l;
}

}  // namespace

TEST_CASE("physical: a valid layer passes validate_physical") {
    const PhysicalLayer l = make_layer();
    CHECK_MESSAGE(validate_physical(l).has_value(), failure(validate_physical(l)));
}

TEST_CASE("physical: round trip through memory keeps every field") {
    const PhysicalLayer l = make_layer();
    auto bytes = serialize_physical(l);
    REQUIRE(bytes.has_value());
    PhysicalReadOptions strict;
    strict.verify_raw = true;
    auto back = read_physical(bytes.value(), strict);
    REQUIRE_MESSAGE(back.has_value(), (back.has_value() ? "" : back.error().message));
    CHECK(back.value() == l);

    auto reader = open_any(bytes.value());
    REQUIRE(reader.has_value());
    CHECK(reader.value().header().kind == Pmd2Kind::physical);
    CHECK(reader.value().header().bind_key == coast_key(l));
    auto from_table = coast_key(reader.value());
    REQUIRE(from_table.has_value());
    CHECK(from_table.value() == coast_key(l));
    // MATL carries the material set version; PROV is optional and not critical.
    CHECK(reader.value().find(kMatl)->version == kMaterialSetVersion);
    CHECK_FALSE(reader.value().find(kProv)->critical());
    CHECK(reader.value().find(kGrid)->critical());
}

TEST_CASE("physical: output is independent of threads and equal in content for every codec") {
    const PhysicalLayer l = make_layer();
    Pmd2WriterOptions one;
    one.threads = 1;
    Pmd2WriterOptions many;
    many.threads = 3;
    Pmd2WriterOptions raw;
    raw.codec = Pmd2Codec::raw;
    const auto a = serialize_physical(l, one);
    const auto b = serialize_physical(l, many);
    const auto c = serialize_physical(l, raw);
    REQUIRE((a.has_value() && b.has_value() && c.has_value()));
    CHECK(a.value() == b.value());
    const auto id_a = open_any(a.value()).value().header().content_id;
    const auto id_c = open_any(c.value()).value().header().content_id;
    CHECK(id_a == id_c);
    CHECK(read_physical(c.value()).value() == l);
}

TEST_CASE("physical: file round trip") {
    const PhysicalLayer l = make_layer();
    const auto path = std::filesystem::temp_directory_path() / "patriarch_physical_layer_test.pmd";
    REQUIRE(write_physical_file(path, l).has_value());
    auto back = read_physical_file(path);
    std::error_code ec;
    std::filesystem::remove(path, ec);
    REQUIRE(back.has_value());
    CHECK(back.value() == l);
    CHECK_FALSE(read_physical_file(path).has_value());  // removed: IO error, not a crash
}

TEST_CASE("physical: procedural map keeps GEOR projection none and round trips") {
    GenParams p;
    p.width = 256;
    p.height = 128;
    p.seed = 5;
    p.province_count = 20;
    p.threads = 2;
    const PhysicalLayer l = from_procedural(generate_map(p));
    CHECK(l.georef.projection == Projection::none);
    CHECK(l.georef == GeoRef{});
    CHECK_MESSAGE(validate_physical(l).has_value(), failure(validate_physical(l)));
    auto bytes = serialize_physical(l);
    REQUIRE(bytes.has_value());
    auto back = read_physical(bytes.value());
    REQUIRE(back.has_value());
    CHECK(back.value() == l);
    CHECK(back.value().georef.projection == Projection::none);
    CHECK_FALSE(back.value().provenance.has_value());  // procedural maps need no PROV
    CHECK(open_any(bytes.value()).value().find(kProv) == nullptr);
}

TEST_CASE("physical: coast key follows WBOD and GEOR, nothing else") {
    const PhysicalLayer base = make_layer();
    const std::uint64_t key = coast_key(base);
    {
        PhysicalLayer l = base;
        l.heights[500] = static_cast<std::uint16_t>(l.heights[500] + 1);
        l.materials[500] = static_cast<std::uint8_t>(Material::Rock);
        l.provenance.reset();
        l.seed = 1;
        l.metres_per_unit_milli = 999;
        CHECK(coast_key(l) == key);
    }
    {
        PhysicalLayer l = base;
        l.georef.pixel_size_x_mm += 1;
        CHECK(coast_key(l) != key);
    }
    {
        PhysicalLayer l = base;
        const std::size_t i = l.water_body.size() - 1;  // one more water pixel (a separate lake)
        l.water_body[i] = 3;
        l.heights[i] = 10;
        l.water_bodies.push_back({3, WaterKind::lake, 0, 20, 1, "pond"});
        CHECK(coast_key(l) != key);
    }
    // A rebuilt file with only heights changed binds to the same key (no new province file needed).
    PhysicalLayer l = base;
    l.heights[600] = static_cast<std::uint16_t>(l.heights[600] + 5);
    auto a = serialize_physical(base);
    auto b = serialize_physical(l);
    REQUIRE((a.has_value() && b.has_value()));
    CHECK(open_any(a.value()).value().header().bind_key == open_any(b.value()).value().header().bind_key);
    CHECK(open_any(a.value()).value().header().content_id != open_any(b.value()).value().header().content_id);
    CHECK(coast_key(base.width, base.height, 1, 2) != coast_key(base.width, base.height, 2, 1));
}

TEST_CASE("validate_physical: detects broken layers") {
    const PhysicalLayer good = make_layer();
    REQUIRE(validate_physical(good).has_value());

    const auto expect = [&](int code, const char* what, auto&& mutate) {
        PhysicalLayer l = good;
        mutate(l);
        const auto r = validate_physical(l);
        CAPTURE(what);
        CHECK_FALSE(r.has_value());
        CHECK(code_of(r) == code);
        CHECK_FALSE(serialize_physical(l).has_value());  // the writer refuses invalid layers
    };

    expect(physical_error::kBadDimensions, "zero width", [](PhysicalLayer& l) { l.width = 0; });
    expect(physical_error::kLayerSize, "short heights", [](PhysicalLayer& l) { l.heights.pop_back(); });
    expect(physical_error::kLayerSize, "short water", [](PhysicalLayer& l) { l.water_body.pop_back(); });
    expect(physical_error::kLayerSize, "long materials", [](PhysicalLayer& l) { l.materials.push_back(0); });
    expect(physical_error::kBadGrid, "zero scale", [](PhysicalLayer& l) { l.metres_per_unit_milli = 0; });
    expect(physical_error::kBadGrid, "material set", [](PhysicalLayer& l) { l.material_version = 2; });
    expect(physical_error::kBadMaterial, "material 10", [](PhysicalLayer& l) { l.materials[7] = 10; });
    expect(physical_error::kBadWaterTable, "id gap", [](PhysicalLayer& l) { l.water_bodies[1].id = 5; });
    expect(physical_error::kBadWaterTable, "kind 0", [](PhysicalLayer& l) {
        l.water_bodies[1].kind = static_cast<WaterKind>(0);
    });
    expect(physical_error::kBadWaterTable, "kind 4", [](PhysicalLayer& l) {
        l.water_bodies[1].kind = static_cast<WaterKind>(4);
    });
    expect(physical_error::kBadWaterTable, "flags", [](PhysicalLayer& l) { l.water_bodies[0].flags = 1; });
    expect(
        physical_error::kBadWaterTable, "empty key", [](PhysicalLayer& l) { l.water_bodies[0].key.clear(); });
    expect(physical_error::kBadWaterTable, "upper-case key", [](PhysicalLayer& l) {
        l.water_bodies[0].key = "Sea";
    });
    expect(physical_error::kBadWaterTable, "long key", [](PhysicalLayer& l) {
        l.water_bodies[0].key = std::string(kMaxWaterKeyLength + 1, 'a');
    });
    expect(physical_error::kBadWaterTable, "duplicate key", [](PhysicalLayer& l) {
        l.water_bodies[1].key = l.water_bodies[0].key;
    });
    expect(physical_error::kSeaLevel, "sea above sea_level", [](PhysicalLayer& l) {
        l.water_bodies[0].surface_level = kSea + 1;
    });
    expect(physical_error::kBadWaterId, "unknown id in raster", [](PhysicalLayer& l) {
        l.water_body[3000] = 9;
    });
    expect(physical_error::kBedAboveSurface, "lake bed above surface", [](PhysicalLayer& l) {
        l.heights[static_cast<std::size_t>(22) * l.width + 25] = 2501;
    });
    expect(physical_error::kBedAboveSurface, "sea bed above sea level", [](PhysicalLayer& l) {
        l.heights[3] = kSea + 1;
    });
    expect(physical_error::kWaterArea, "area mismatch", [](PhysicalLayer& l) {
        l.water_bodies[1].area_px += 1;
    });
    expect(physical_error::kWaterArea, "empty body", [](PhysicalLayer& l) {
        l.water_bodies.push_back({3, WaterKind::lake, 0, 3000, 0, "dry"});
    });
    expect(physical_error::kWaterSplit, "lake split in two", [](PhysicalLayer& l) {
        // A second lake pixel far from the lake, same id: area stays consistent, connectivity breaks.
        const std::size_t i = static_cast<std::size_t>(40) * l.width + 50;
        l.water_body[i] = 2;
        l.heights[i] = 2400;
        l.water_bodies[1].area_px += 1;
    });
    expect(physical_error::kBadGeoref, "none with data", [](PhysicalLayer& l) {
        l.georef = GeoRef{};
        l.georef.epsg = 4326;
    });
    expect(physical_error::kBadProvenance, "empty source id", [](PhysicalLayer& l) {
        l.provenance->sources[0].source_id.clear();
    });
    expect(physical_error::kBadProvenance, "control char", [](PhysicalLayer& l) {
        l.provenance->builder = "a\nb";
    });
}

TEST_CASE("validate_georef: consistency with the dimensions") {
    const GeoRef good = albers();
    REQUIRE(validate_georef(good, 64, 48).has_value());
    REQUIRE(validate_georef(GeoRef{}, 64, 48).has_value());

    const auto expect_bad = [&](const char* what, auto&& mutate) {
        GeoRef g = good;
        mutate(g);
        CAPTURE(what);
        CHECK(code_of(validate_georef(g, 64, 48)) == physical_error::kBadGeoref);
    };
    expect_bad("version", [](GeoRef& g) { g.version = 2; });
    expect_bad("no datum", [](GeoRef& g) { g.datum = Datum::none; });
    expect_bad("projection 9", [](GeoRef& g) { g.projection = static_cast<Projection>(9); });
    expect_bad("lat origin", [](GeoRef& g) { g.lat_origin_udeg = 90'000'001; });
    expect_bad("lon central", [](GeoRef& g) { g.lon_central_udeg = -180'000'001; });
    expect_bad("std1", [](GeoRef& g) { g.lat_std1_udeg = -91'000'000; });
    expect_bad("zero pixel", [](GeoRef& g) { g.pixel_size_x_mm = 0; });
    expect_bad("negative pixel", [](GeoRef& g) { g.pixel_size_y_mm = -1; });
    expect_bad("huge pixel", [](GeoRef& g) { g.pixel_size_x_mm = 2'000'000'000; });
    expect_bad("origin far", [](GeoRef& g) { g.origin_x_mm = 200'000'000'000'000; });
    expect_bad("extent leaves the plane", [](GeoRef& g) {
        g.origin_x_mm = 99'999'999'999'000;
        g.pixel_size_x_mm = 1'000'000;
    });
    expect_bad("empty bbox", [](GeoRef& g) { g.bbox_max_lat_udeg = g.bbox_min_lat_udeg; });
    expect_bad("bbox lon out of range", [](GeoRef& g) { g.bbox_max_lon_udeg = 181'000'000; });
    expect_bad("long text", [](GeoRef& g) { g.proj_text = std::string(kMaxProjTextLength + 1, 'x'); });
    expect_bad("empty lon range", [](GeoRef& g) { g.bbox_max_lon_udeg = g.bbox_min_lon_udeg; });
    // A box across the antimeridian (min_lon > max_lon, RFC 7946): 15 E .. 165 W is valid ...
    {
        GeoRef g = good;
        g.bbox_min_lon_udeg = 15'000'000;
        g.bbox_max_lon_udeg = -165'000'000;
        CHECK(validate_georef(g, 64, 48).has_value());
        // ... and survives the blob round trip unchanged.
        const auto back = decode_georef(encode_georef(g));
        REQUIRE(back.has_value());
        CHECK(back.value() == g);
        g.bbox_min_lon_udeg = 179'999'999;
        g.bbox_max_lon_udeg = -179'999'999;
        CHECK(validate_georef(g, 64, 48).has_value());
    }
    // ... but not when an edge is on +-180 (that box has a plain spelling).
    expect_bad("crossing from 180", [](GeoRef& g) {
        g.bbox_min_lon_udeg = 180'000'000;
        g.bbox_max_lon_udeg = -165'000'000;
    });
    expect_bad("crossing to -180", [](GeoRef& g) {
        g.bbox_min_lon_udeg = 15'000'000;
        g.bbox_max_lon_udeg = -180'000'000;
    });
    // The full range in plain spelling stays valid (older stage-1 output wrote it).
    {
        GeoRef g = good;
        g.bbox_min_lon_udeg = -180'000'000;
        g.bbox_max_lon_udeg = 180'000'000;
        CHECK(validate_georef(g, 64, 48).has_value());
    }
    // projection none: every field must be zero, including the pixel size.
    GeoRef none;
    none.pixel_size_x_mm = 1;
    CHECK(code_of(validate_georef(none, 64, 48)) == physical_error::kBadGeoref);
    none = GeoRef{};
    none.datum = Datum::wgs84;
    CHECK(code_of(validate_georef(none, 64, 48)) == physical_error::kBadGeoref);
}

TEST_CASE("physical: blob codecs round trip and reject truncation and trailing bytes") {
    const GeoRef g = albers();
    const auto geor = encode_georef(g);
    REQUIRE(decode_georef(geor).has_value());
    CHECK(decode_georef(geor).value() == g);
    GeoRef negative = g;
    negative.false_easting_mm = std::numeric_limits<std::int64_t>::min() / 1000;
    negative.lat_origin_udeg = -45'000'000;
    CHECK(decode_georef(encode_georef(negative)).value() == negative);

    const PhysicalLayer l = make_layer();
    const auto wtbl = encode_water_table(l.water_bodies);
    CHECK(decode_water_table(wtbl).value() == l.water_bodies);
    const auto prov = encode_provenance(*l.provenance);
    CHECK(decode_provenance(prov).value() == *l.provenance);

    const auto check_all = [](const std::vector<std::uint8_t>& blob, auto&& decode) {
        for (std::size_t len = 0; len < blob.size(); ++len) {
            const std::span<const std::uint8_t> cut(blob.data(), len);
            CHECK_FALSE(decode(cut, false));
        }
        auto longer = blob;
        longer.push_back(0);
        CHECK_FALSE(decode(std::span<const std::uint8_t>(longer), false));
        CHECK(decode(std::span<const std::uint8_t>(longer), true));  // newer minor may append
    };
    check_all(geor, [](std::span<const std::uint8_t> b, bool t) { return decode_georef(b, t).has_value(); });
    check_all(wtbl,
              [](std::span<const std::uint8_t> b, bool t) { return decode_water_table(b, t).has_value(); });
    check_all(prov,
              [](std::span<const std::uint8_t> b, bool t) { return decode_provenance(b, t).has_value(); });

    // Unknown projection/datum/kind values in a blob are rejected on decode.
    auto bad_projection = geor;
    bad_projection[2] = 9;
    CHECK_FALSE(decode_georef(bad_projection).has_value());
    auto bad_kind = wtbl;
    bad_kind[2 + 2] = 7;  // count, id, then the kind byte
    CHECK_FALSE(decode_water_table(bad_kind).has_value());
}

TEST_CASE("physical reader: structural errors") {
    const PhysicalLayer l = make_layer();
    const auto read = [](const std::vector<std::uint8_t>& bytes) { return read_physical(bytes); };
    REQUIRE(read(build_file(l, {})).has_value());

    SUBCASE("missing required chunks") {
        for (const Pmd2ChunkId id : {kGrid, kGeor, kHght, kWbod, kWtbl, kMatl}) {
            Tweaks t;
            t.skip = id;
            const auto r = read(build_file(l, t));
            CAPTURE(pmd2_chunk_id_string(id));
            REQUIRE_FALSE(r.has_value());
            CHECK(r.error().code == physical_error::kMissingChunk);
        }
    }
    SUBCASE("optional PROV may be absent") {
        Tweaks t;
        t.skip = kProv;
        const auto r = read(build_file(l, t));
        REQUIRE(r.has_value());
        CHECK_FALSE(r.value().provenance.has_value());
    }
    SUBCASE("wrong container kind") {
        Tweaks t;
        t.kind = Pmd2Kind::provinces;
        const auto r = read(build_file(l, t));
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().code == physical_error::kWrongKind);
    }
    SUBCASE("bind_key must be the coast key") {
        Tweaks t;
        t.bind_key = coast_key(l) ^ 1U;
        const auto r = read(build_file(l, t));
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().code == physical_error::kBindKey);
    }
    SUBCASE("GRID size differs from the header") {
        Tweaks t;
        t.grid = grid_blob(l, l.width + 1);
        const auto r = read(build_file(l, t));
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().code == physical_error::kGridHeader);
    }
    SUBCASE("GRID version") {
        Tweaks t;
        t.grid = grid_blob(l, l.width, 2);
        const auto r = read(build_file(l, t));
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().code == physical_error::kBadGrid);
    }
    SUBCASE("MATL chunk version differs from the GRID material set") {
        Tweaks t;
        // The chunk is written as version 1; a GRID that claims another set is a mismatch.
        t.grid = [&] {
            auto b = grid_blob(l, l.width);
            b[b.size() - 2] = 2;  // material_version field (last u16)
            return b;
        }();
        const auto r = read(build_file(l, t));
        REQUIRE_FALSE(r.has_value());
    }
    SUBCASE("MATL newer than the reader knows is a container error") {
        Tweaks t;
        t.matl_version = 2;
        const auto r = read(build_file(l, t));
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().code == pmd2_error::kUnsupportedChunk);
    }
    SUBCASE("unknown optional chunks are skipped, unknown critical ones are errors") {
        Tweaks t;
        t.unknown_optional = true;
        const auto ok = read(build_file(l, t));
        REQUIRE(ok.has_value());
        CHECK(ok.value() == l);
        Tweaks c;
        c.unknown_critical = true;
        const auto bad = read(build_file(l, c));
        REQUIRE_FALSE(bad.has_value());
        CHECK(bad.error().code == pmd2_error::kUnknownCritical);
    }
    SUBCASE("a table that contradicts the raster is a validation error") {
        auto bodies = l.water_bodies;
        bodies[1].area_px += 3;
        Tweaks t;
        t.wtbl = encode_water_table(bodies);
        const auto r = read(build_file(l, t));
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().code == physical_error::kWaterArea);
    }
    SUBCASE("a bad GEOR blob is rejected") {
        GeoRef g = l.georef;
        g.pixel_size_x_mm = 0;
        Tweaks t;
        t.geor = encode_georef(g);
        t.bind_key = coast_key(l);
        const auto r = read(build_file(l, t));
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().code == physical_error::kBadGeoref);
    }
    SUBCASE("a bad PROV blob is rejected") {
        Tweaks t;
        auto blob = encode_provenance(*l.provenance);
        blob.pop_back();
        t.prov = blob;
        const auto r = read(build_file(l, t));
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().code == physical_error::kBadChunk);
    }
    SUBCASE("garbage and truncation never crash") {
        const auto file = build_file(l, {});
        CHECK_FALSE(read({}).has_value());
        CHECK_FALSE(read(std::vector<std::uint8_t>(10, 0)).has_value());
        for (std::size_t len = 0; len < file.size(); len += file.size() / 40 + 1) {
            const std::span<const std::uint8_t> cut(file.data(), len);
            CHECK_FALSE(read_physical(cut).has_value());
        }
        // Flipping one bit anywhere in the file is detected (hashes cover header, table and bodies).
        for (std::size_t pos = 0; pos < file.size(); pos += file.size() / 60 + 1) {
            auto copy = file;
            copy[pos] = static_cast<std::uint8_t>(copy[pos] ^ 0x10U);
            PhysicalReadOptions strict;
            strict.verify_raw = true;
            CAPTURE(pos);
            CHECK_FALSE(read_physical(copy, strict).has_value());
        }
    }
}
