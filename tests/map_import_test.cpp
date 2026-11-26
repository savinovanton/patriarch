// mapgen import (Geo-3): stage-1 geodata -> PMD2 set, on small synthetic inputs.

#include "pat/mapdata/map_import.hpp"
#include "pat/mapdata/map_migrate.hpp"
#include "pat/mapdata/province_layer.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "test_files.hpp"

using namespace pat::mapdata;

namespace {

constexpr std::uint32_t kW = 64;
constexpr std::uint32_t kH = 32;

template <class T>
int code_of(const pat::core::Result<T, pat::core::Error>& r) {
    return r.has_value() ? 0 : r.error().code;
}

GeoRef test_georef() {
    GeoRef g;
    g.projection = Projection::lambert_conformal;
    g.datum = Datum::wgs84;
    g.lat_origin_udeg = 58'000'000;
    g.lon_central_udeg = 100'000'000;
    g.lat_std1_udeg = 47'000'000;
    g.lat_std2_udeg = 69'000'000;
    g.origin_x_mm = -3'200'000'000;  // 64 x 32 pixels of 100 km around the central meridian
    g.origin_y_mm = 2'000'000'000;
    g.pixel_size_x_mm = 100'000'000;
    g.pixel_size_y_mm = 100'000'000;
    g.bbox_min_lon_udeg = 15'000'000;
    g.bbox_max_lon_udeg = -165'000'000;  // across the antimeridian
    g.bbox_min_lat_udeg = 28'000'000;
    g.bbox_max_lat_udeg = 82'000'000;
    g.proj_text = "+proj=lcc +lat_0=58 +lon_0=100 +lat_1=47 +lat_2=69";
    return g;
}

std::size_t at(std::uint32_t x, std::uint32_t y) {
    return static_cast<std::size_t>(y) * kW + x;
}

/// Sea in the west (columns 0..7) with a 2x2 island, a sea pocket inside the land, a lake, a block
/// of land without DEM data, one pixel below sea level and one too high to represent.
Stage1Data synthetic() {
    Stage1Data d;
    d.width = kW;
    d.height = kH;
    d.projection_id = "lcc";
    d.georef = test_georef();
    d.provenance.manifest_version = 1;
    d.provenance.sources.push_back({"copernicus-dem-glo90", "", {{"tile.tif", {}}}});
    d.provenance.steps.push_back({"reproject:lcc-v1", 0x1234});
    d.height_m.assign(static_cast<std::size_t>(kW) * kH, 0);
    d.water.assign(static_cast<std::size_t>(kW) * kH, 0);
    for (std::uint32_t y = 0; y < kH; ++y) {
        for (std::uint32_t x = 0; x < kW; ++x) {
            const std::size_t i = at(x, y);
            if (x < 8) {
                d.water[i] = 1;
                d.height_m[i] = kStage1NoData;
            } else {
                d.height_m[i] = static_cast<std::int16_t>(100 + 10 * x + 3 * y);
            }
        }
    }
    for (std::uint32_t y = 5; y <= 6; ++y) {  // island in the sea
        for (std::uint32_t x = 3; x <= 4; ++x) {
            d.water[at(x, y)] = 0;
            d.height_m[at(x, y)] = 12;
        }
    }
    for (std::uint32_t y = 10; y <= 12; ++y) {  // lake
        for (std::uint32_t x = 30; x <= 33; ++x) {
            d.water[at(x, y)] = 2;
            d.height_m[at(x, y)] = 50;
        }
    }
    for (std::uint32_t y = 25; y <= 26; ++y) {  // sea pocket (a second sea body)
        for (std::uint32_t x = 50; x <= 51; ++x) {
            d.water[at(x, y)] = 1;
        }
    }
    for (std::uint32_t y = 3; y <= 6; ++y) {  // land without DEM data
        for (std::uint32_t x = 40; x <= 44; ++x) {
            d.height_m[at(x, y)] = kStage1NoData;
        }
    }
    d.height_m[at(20, 20)] = -30;  // land below sea level (a Caspian-like depression)
    d.height_m[at(60, 2)] = 9500;  // above the representable range
    return d;
}

ImportParams params(unsigned threads) {
    ImportParams p;
    p.province_count = 4;
    p.seed = 7;
    p.threads = threads;
    return p;
}

std::string descriptor(const std::string& height_path = "height.i16", const char* sha = "null") {
    return std::string(R"({
  "format": "patriarch-geo-stage1", "format_version": 1,
  "projection_id": "lcc", "width": 64, "height": 32,
  "area": {"west_udeg": 15000000, "south_udeg": 28000000, "east_udeg": 195000000, "north_udeg": 82000000},
  "georef": {"version": 1, "projection": 2, "datum": 1, "lat_origin_udeg": 58000000,
             "lon_central_udeg": 100000000, "lat_std1_udeg": 47000000, "lat_std2_udeg": 69000000,
             "false_easting_mm": 0, "false_northing_mm": 0, "origin_x_mm": -3200000000,
             "origin_y_mm": 2000000000, "pixel_size_x_mm": 100000000, "pixel_size_y_mm": 100000000,
             "bbox_min_lon_udeg": 15000000, "bbox_max_lon_udeg": -165000000,
             "bbox_min_lat_udeg": 28000000, "bbox_max_lat_udeg": 82000000, "epsg": 0,
             "proj_text": "+proj=lcc +lat_0=58 +lon_0=100 +lat_1=47 +lat_2=69"},
  "provenance": {"manifest_version": 1,
    "sources": [{"id": "copernicus-dem-glo90", "version": null, "files": [{"path": "tile.tif", "sha256": )") +
           sha + R"(}]}],
    "steps": [{"id": "reproject:lcc-v1", "params_hash": "0x0000000000001234"}],
    "tools": {"gdal": "3.13.3", "proj": "9.9.0"}},
  "files": {"height": {"path": ")" +
           height_path + R"(", "type": "int16le"}, "water": {"path": "water.u8", "type": "uint8"}}
})";
}

void write_rasters(const std::filesystem::path& dir, const Stage1Data& d) {
    std::ofstream h(dir / "height.i16", std::ios::binary);
    for (const std::int16_t v : d.height_m) {
        const auto u = static_cast<std::uint16_t>(v);
        h.put(static_cast<char>(u & 0xFFU));
        h.put(static_cast<char>(u >> 8U));
    }
    std::ofstream w(dir / "water.u8", std::ios::binary);
    w.write(reinterpret_cast<const char*>(d.water.data()), static_cast<std::streamsize>(d.water.size()));
}

}  // namespace

TEST_CASE("import: a synthetic stage-1 input gives a valid PMD2 pair") {
    const Stage1Data in = synthetic();
    const auto built = import_stage1(in, params(1));
    REQUIRE_MESSAGE(built.has_value(), (built.has_value() ? "" : built.error().message));
    const PhysicalLayer& l = built.value().layers.physical;
    const ProvinceLayer& p = built.value().layers.provinces;
    const ImportReport& r = built.value().report;
    CHECK(validate_physical(l).has_value());
    CHECK(validate_provinces(l, p).has_value());
    CHECK(l.georef == in.georef);
    CHECK(l.sea_level == kImportSeaLevel);
    CHECK(l.metres_per_unit_milli == kImportMetresPerUnitMilli);

    // Water: bodies in raster order, seas at sea level, the lake flat at its lowest shore.
    REQUIRE(l.water_bodies.size() == 3);
    CHECK(l.water_bodies[0].key == "sea");
    CHECK(l.water_bodies[1].key == "lake");
    CHECK(l.water_bodies[1].kind == WaterKind::lake);
    CHECK(l.water_bodies[2].key == "sea-2");
    CHECK(r.seas == 2);
    CHECK(r.lakes == 1);
    // Shore heights are 100 + 10x + 3y metres; the lowest is at (29, 10): 100 + 290 + 30 = 420 m.
    const std::uint16_t lake_surface = l.water_bodies[1].surface_level;
    CHECK(lake_surface == kImportSeaLevel + 2800);  // 420 m / 0.15
    for (std::uint32_t y = 10; y <= 12; ++y) {
        for (std::uint32_t x = 30; x <= 33; ++x) {
            CHECK(l.heights[at(x, y)] == lake_surface);
            CHECK(l.materials[at(x, y)] == static_cast<std::uint8_t>(Material::ShallowWater));
        }
    }
    CHECK(l.heights[at(7, 20)] < kImportSeaLevel);       // sea bed next to the coast
    CHECK(l.heights[at(0, 20)] < l.heights[at(7, 20)]);  // deeper away from it

    // Land: DEM kept (rounded to 0.15 m units), below-sea land stays land, the top is clamped, holes filled.
    CHECK(l.heights[at(10, 0)] == kImportSeaLevel + 1333);  // 200 m
    CHECK(l.water_body[at(20, 20)] == 0);
    CHECK(l.heights[at(20, 20)] == kImportSeaLevel - 200);  // -30 m
    CHECK(l.heights[at(60, 2)] == 65535);
    CHECK(r.clamped_px == 1);
    CHECK(r.nodata_filled_px == 20);
    CHECK(l.heights[at(42, 4)] > kImportSeaLevel);
    for (std::size_t i = 0; i < l.pixel_count(); ++i) {
        const auto m = static_cast<Material>(l.materials[i]);
        const bool water_material = m == Material::DeepWater || m == Material::ShallowWater;
        CHECK(water_material == (l.water_body[i] != 0));
    }

    // Provinces: every land pixel is covered, the island is a province of its own.
    CHECK(r.land_px == p.pixel_count() - 8U * kH + 4U - 12U - 4U);
    CHECK(p.entries.size() == r.placeholder_provinces + r.island_provinces);
    const std::uint16_t island = p.province_id[at(3, 5)];
    REQUIRE(island != 0);
    CHECK(p.entries[island - 1U].area_px == 4);
    CHECK(p.entries[island - 1U].coastal);
    CHECK(p.entries[island - 1U].neighbors.empty());
    CHECK(p.entries[0].key == "p0001");
    REQUIRE(l.provenance.has_value());
    CHECK(l.provenance->builder == "mapgen import");
    CHECK(l.provenance->steps.back().step_id == "import:stage2-v1");
    REQUIRE(p.provenance.has_value());
    CHECK(p.provenance->steps.back().step_id == "provinces:placeholder-v1");
}

TEST_CASE("import: the result does not depend on the number of threads") {
    const Stage1Data in = synthetic();
    const auto a = import_stage1(in, params(1));
    const auto b = import_stage1(in, params(4));
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    CHECK(a.value().layers.physical == b.value().layers.physical);
    CHECK(a.value().layers.provinces == b.value().layers.provinces);
}

TEST_CASE("import: the written set loads back through load_map") {
    const auto built = import_stage1(synthetic(), params(2));
    REQUIRE(built.has_value());
    pat::test::TempDir dir("map_import");
    REQUIRE(write_map_set(dir.path(), built.value().layers).has_value());
    const auto expected = build_map_data(built.value().layers.physical, built.value().layers.provinces);
    REQUIRE(expected.has_value());
    for (const char* name : {kPhysicalFileName, kProvincesFileName}) {
        CAPTURE(name);
        const auto back = load_map(dir.path() / name);
        REQUIRE_MESSAGE(back.has_value(), (back.has_value() ? "" : back.error().message));
        CHECK(back.value() == expected.value());
    }
}

TEST_CASE("import: invalid inputs are errors") {
    {
        Stage1Data d = synthetic();
        d.water[at(20, 20)] = 3;
        CHECK(code_of(import_stage1(d, params(1))) == import_error::kBadWaterCode);
    }
    {
        Stage1Data d = synthetic();
        d.water.assign(d.water.size(), 1);
        CHECK(code_of(import_stage1(d, params(1))) == import_error::kNoLand);
    }
    {
        Stage1Data d = synthetic();
        d.height_m.pop_back();
        CHECK(code_of(import_stage1(d, params(1))) == import_error::kRasterSize);
    }
    {
        Stage1Data d = synthetic();
        d.georef.bbox_max_lon_udeg = d.georef.bbox_min_lon_udeg;
        CHECK(code_of(import_stage1(d, params(1))) == physical_error::kBadGeoref);
    }
}

TEST_CASE("import: the stage-1 descriptor") {
    const auto d = parse_stage1_descriptor(descriptor());
    REQUIRE_MESSAGE(d.has_value(), (d.has_value() ? "" : d.error().message));
    CHECK(d.value().width == kW);
    CHECK(d.value().height == kH);
    CHECK(d.value().projection_id == "lcc");
    CHECK(d.value().georef == test_georef());
    CHECK(d.value().unverified_files == 1);
    CHECK(d.value().height_file == "height.i16");
    REQUIRE(d.value().provenance.steps.size() == 1);
    CHECK(d.value().provenance.steps[0].params_hash == 0x1234U);
    REQUIRE(d.value().provenance.sources.size() == 1);
    CHECK(d.value().provenance.sources[0].source_version.empty());

    const auto hashed = parse_stage1_descriptor(
        descriptor("height.i16", "\"00112233445566778899aabbccddeeff00112233445566778899AABBCCDDEEFF\""));
    REQUIRE(hashed.has_value());
    CHECK(hashed.value().unverified_files == 0);
    CHECK(hashed.value().provenance.sources[0].files[0].sha256[1] == 0x11);
    CHECK(hashed.value().provenance.sources[0].files[0].sha256[31] == 0xFF);

    CHECK(code_of(parse_stage1_descriptor("{}")) == import_error::kBadDescriptor);
    CHECK(code_of(parse_stage1_descriptor("not json")) == import_error::kBadDescriptor);
    CHECK(code_of(parse_stage1_descriptor(descriptor("../height.i16"))) == import_error::kBadDescriptor);
    CHECK(code_of(parse_stage1_descriptor(descriptor("height.i16", "\"abc\""))) ==
          import_error::kBadDescriptor);
    std::string v2 = descriptor();
    v2.replace(v2.find("\"format_version\": 1"), 19, "\"format_version\": 2");
    CHECK(code_of(parse_stage1_descriptor(v2)) == import_error::kBadDescriptor);
    std::string big = descriptor();
    big.replace(big.find("\"datum\": 1"), 10, "\"datum\": 300");
    CHECK(code_of(parse_stage1_descriptor(big)) == import_error::kBadDescriptor);
}

TEST_CASE("import: read_stage1 reads the descriptor and the rasters") {
    const Stage1Data in = synthetic();
    pat::test::TempDir dir("stage1");
    dir.write("stage1.json", descriptor());
    write_rasters(dir.path(), in);
    const auto read = read_stage1(dir.path());
    REQUIRE_MESSAGE(read.has_value(), (read.has_value() ? "" : read.error().message));
    CHECK(read.value().height_m == in.height_m);
    CHECK(read.value().water == in.water);

    {
        std::ofstream cut(dir.path() / "water.u8", std::ios::binary);
        cut.put(0);
    }
    CHECK(code_of(read_stage1(dir.path())) == import_error::kRasterSize);
    pat::test::TempDir empty("stage1_empty");
    CHECK(code_of(read_stage1(empty.path())) == import_error::kIo);
}
