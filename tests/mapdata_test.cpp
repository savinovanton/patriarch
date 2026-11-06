#include "pat/mapdata/map_format.hpp"
#include "pat/mapdata/map_gen.hpp"
#include "pat/mapdata/map_stats.hpp"
#include "pat/mapdata/map_validate.hpp"

#include <doctest/doctest.h>

#include <array>
#include <filesystem>

using namespace pat::mapdata;

namespace {

GenParams small_params(std::uint64_t seed, unsigned threads = 2) {
    GenParams p;
    p.width = 384;
    p.height = 192;
    p.seed = seed;
    p.province_count = 40;
    p.threads = threads;
    return p;
}

}  // namespace

TEST_CASE("mapgen: generated maps satisfy every invariant for many seeds") {
    for (std::uint64_t seed = 1; seed <= 6; ++seed) {
        CAPTURE(seed);
        const MapData map = generate_map(small_params(seed));
        const auto valid = validate_map(map);
        CHECK_MESSAGE(valid.has_value(), (valid.has_value() ? "" : valid.error().message));
        CHECK(map.province_list.size() == 40);
        CHECK(reachable_province_count(map) == 40);
    }
}

TEST_CASE("mapgen: land coverage is sensible and the sea is present") {
    const MapData map = generate_map(small_params(3));
    std::size_t land = 0;
    for (std::size_t i = 0; i < map.pixel_count(); ++i) {
        land += map.is_land(i) ? 1U : 0U;
    }
    const double fraction = static_cast<double>(land) / static_cast<double>(map.pixel_count());
    CHECK(fraction > 0.2);
    CHECK(fraction < 0.8);
    // The map edge is open water (the continent does not touch the border).
    for (std::uint32_t x = 0; x < map.width; ++x) {
        CHECK_FALSE(map.is_land(map.index(x, 0)));
        CHECK_FALSE(map.is_land(map.index(x, map.height - 1)));
    }
    // Several materials are in use, including water and mountains or hills.
    std::array<bool, kMaterialCount> used{};
    for (const std::uint8_t m : map.materials) {
        used[m] = true;
    }
    CHECK(used[static_cast<std::size_t>(Material::DeepWater)]);
    CHECK(used[static_cast<std::size_t>(Material::ShallowWater)]);
    std::size_t kinds = 0;
    for (const bool b : used) {
        kinds += b ? 1U : 0U;
    }
    CHECK(kinds >= 5);
}

TEST_CASE("mapgen: same seed gives the same map regardless of thread count") {
    const MapData a = generate_map(small_params(11, 1));
    const MapData b = generate_map(small_params(11, 3));
    const MapData c = generate_map(small_params(11, 8));
    CHECK(a == b);
    CHECK(a == c);
    const MapData other = generate_map(small_params(12, 2));
    CHECK_FALSE(a == other);
}

TEST_CASE("map format: serialize / deserialize round trip") {
    const MapData map = generate_map(small_params(5));
    const auto bytes = serialize_map(map);
    const auto loaded = deserialize_map(bytes);
    REQUIRE(loaded.has_value());
    CHECK(loaded.value() == map);
    CHECK(validate_map(loaded.value()).has_value());
    // The file is much smaller than the raw layers.
    CHECK(bytes.size() < map.pixel_count() * 5 / 2);
}

TEST_CASE("map format: files are written atomically and read back") {
    const MapData map = generate_map(small_params(6));
    const auto path = std::filesystem::temp_directory_path() / "patriarch_map_test.pmd";
    REQUIRE(write_map_file(path, map).has_value());
    CHECK_FALSE(std::filesystem::exists(path.string() + ".tmp"));
    const auto loaded = read_map_file(path);
    REQUIRE(loaded.has_value());
    CHECK(loaded.value() == map);
    std::filesystem::remove(path);
    CHECK_FALSE(read_map_file(path).has_value());
}

TEST_CASE("map format: corrupt input is rejected without crashing") {
    const MapData map = generate_map(small_params(7));
    const auto good = serialize_map(map);

    auto bad = good;
    bad[0] = 'X';
    CHECK_FALSE(deserialize_map(bad).has_value());  // magic

    bad = good;
    bad[4] = 99;  // format version
    CHECK_FALSE(deserialize_map(bad).has_value());

    bad = good;
    bad[6] = 1;  // flags must be zero
    CHECK_FALSE(deserialize_map(bad).has_value());

    // Truncation at many points.
    for (std::size_t len : {std::size_t{0},
                            std::size_t{3},
                            std::size_t{20},
                            std::size_t{40},
                            good.size() / 3,
                            good.size() / 2,
                            good.size() - 1}) {
        const std::vector<std::uint8_t> cut(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(len));
        CHECK_FALSE(deserialize_map(cut).has_value());
    }

    // A flipped bit in every part of the payload must be detected (hash or zstd failure).
    for (std::size_t pos = 60; pos < good.size(); pos += good.size() / 23 + 1) {
        bad = good;
        bad[pos] ^= 0x10;
        CHECK_FALSE(deserialize_map(bad).has_value());
    }

    bad = good;
    bad.push_back(0);  // trailing garbage
    CHECK_FALSE(deserialize_map(bad).has_value());
}

TEST_CASE("validate_map: detects broken maps") {
    const MapData good = generate_map(small_params(8));
    REQUIRE(validate_map(good).has_value());

    SUBCASE("province on water") {
        MapData m = good;
        for (std::size_t i = 0; i < m.pixel_count(); ++i) {
            if (!m.is_land(i)) {
                m.provinces[i] = 1;
                break;
            }
        }
        CHECK_FALSE(validate_map(m).has_value());
    }
    SUBCASE("land without province") {
        MapData m = good;
        for (std::size_t i = 0; i < m.pixel_count(); ++i) {
            if (m.is_land(i)) {
                m.provinces[i] = 0;
                break;
            }
        }
        CHECK_FALSE(validate_map(m).has_value());
    }
    SUBCASE("table does not match the pixels") {
        MapData m = good;
        m.province_list[3].area_px += 1;
        CHECK_FALSE(validate_map(m).has_value());
    }
    SUBCASE("duplicate colours") {
        MapData m = good;
        m.province_list[1].color_rgb = m.province_list[0].color_rgb;
        CHECK_FALSE(validate_map(m).has_value());
    }
    SUBCASE("invalid material") {
        MapData m = good;
        m.materials[0] = static_cast<std::uint8_t>(kMaterialCount);
        CHECK_FALSE(validate_map(m).has_value());
    }
    SUBCASE("layer size mismatch") {
        MapData m = good;
        m.heights.pop_back();
        CHECK_FALSE(validate_map(m).has_value());
    }
    SUBCASE("a province split into two regions") {
        // Give one pixel inside province A the id of a non-adjacent province B.
        MapData m = good;
        std::size_t target = m.pixel_count();
        for (std::size_t i = 0; i < m.pixel_count(); ++i) {
            if (m.provinces[i] == 1) {
                target = i;
                break;
            }
        }
        REQUIRE(target != m.pixel_count());
        const Province* a = m.province(1);
        std::uint16_t far_id = 0;
        for (const Province& p : m.province_list) {
            bool adjacent = p.id == a->id;
            for (const Border& b : a->neighbors) {
                adjacent = adjacent || b.neighbor == p.id;
            }
            if (!adjacent) {
                far_id = p.id;
                break;
            }
        }
        REQUIRE(far_id != 0);
        m.provinces[target] = far_id;
        rebuild_province_stats(m);
        CHECK_FALSE(validate_map(m).has_value());  // far_id now has two regions (or A lost its pixel)
    }
}

TEST_CASE("province stats are symmetric and consistent") {
    const MapData map = generate_map(small_params(9));
    std::uint64_t total = 0;
    for (const Province& p : map.province_list) {
        total += p.area_px;
        CHECK(p.area_px > 0);
        CHECK(p.min_x <= p.centroid_x);
        CHECK(p.centroid_x <= p.max_x);
        CHECK(p.min_y <= p.centroid_y);
        CHECK(p.centroid_y <= p.max_y);
        CHECK_FALSE(p.neighbors.empty());
        for (const Border& b : p.neighbors) {
            CHECK(b.length_px > 0);
            const Province* other = map.province(b.neighbor);
            REQUIRE(other != nullptr);
            bool found = false;
            for (const Border& back : other->neighbors) {
                if (back.neighbor == p.id) {
                    found = true;
                    CHECK(back.length_px == b.length_px);
                }
            }
            CHECK(found);
        }
    }
    std::uint64_t land = 0;
    for (std::size_t i = 0; i < map.pixel_count(); ++i) {
        land += map.is_land(i) ? 1U : 0U;
    }
    CHECK(total == land);
}

TEST_CASE("compute_normals: flat ground points up, slopes lean downhill") {
    MapData m;
    m.width = 8;
    m.height = 8;
    m.sea_level = 100;
    m.metres_per_unit_milli = 1000;  // 1 m per height unit
    m.heights.assign(64, 500);
    m.provinces.assign(64, 1);
    m.materials.assign(64, 0);

    auto flat = compute_normals(m, 1.0);
    REQUIRE(flat.size() == 128);
    for (const std::int8_t v : flat) {
        CHECK(v == 0);
    }

    // Height rises to the east (+x): the surface normal leans west (negative x).
    for (std::uint32_t y = 0; y < 8; ++y) {
        for (std::uint32_t x = 0; x < 8; ++x) {
            m.heights[m.index(x, y)] = static_cast<std::uint16_t>(500 + 10 * x);
        }
    }
    const auto ramp = compute_normals(m, 1.0);
    const std::size_t centre = m.index(4, 4) * 2;
    CHECK(ramp[centre] < -100);  // strong lean towards -x (slope 10:1)
    CHECK(ramp[centre + 1] == 0);

    // Rising to the south (+y): normal leans north (negative y, image rows go south).
    for (std::uint32_t y = 0; y < 8; ++y) {
        for (std::uint32_t x = 0; x < 8; ++x) {
            m.heights[m.index(x, y)] = static_cast<std::uint16_t>(500 + 10 * y);
        }
    }
    const auto ramp_y = compute_normals(m, 1.0);
    CHECK(ramp_y[centre] == 0);
    CHECK(ramp_y[centre + 1] < -100);
}

TEST_CASE("map data helpers") {
    MapData m;
    m.width = 4;
    m.height = 2;
    m.sea_level = 1000;
    m.metres_per_unit_milli = 500;
    CHECK(m.pixel_count() == 8);
    CHECK(m.index(3, 1) == 7);
    CHECK(m.elevation_m(1000) == doctest::Approx(0.0));
    CHECK(m.elevation_m(1200) == doctest::Approx(100.0));
    CHECK(m.elevation_m(800) == doctest::Approx(-100.0));
    CHECK(m.province(0) == nullptr);
    CHECK(m.province(1) == nullptr);
}
