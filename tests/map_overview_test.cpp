// The data behind the overlays of the map screen (UI-D05, #260): the season of a date, the SVG silhouette of
// the map, the player's provinces, the camera footprint and the length of the scale label.

#include "pat/game/map_overview.hpp"
#include "pat/sim/demo_scenario.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <string>
#include <vector>

namespace {

using namespace pat;

mapdata::MapData tiny_map() {
    // 4 x 2 texels: province 1 on the left half, province 2 on the right, water in the corner.
    mapdata::MapData map;
    map.width = 4;
    map.height = 2;
    map.provinces = {1, 1, 2, 0, 1, 1, 2, 2};
    return map;
}

}  // namespace

TEST_CASE("map overview: seasons follow the months of the Julian calendar") {
    using ui::Season;
    CHECK(game::season_of(sim::Date::from_ymd(1703, 1, 1)) == Season::Winter);
    CHECK(game::season_of(sim::Date::from_ymd(1703, 2, 28)) == Season::Winter);
    CHECK(game::season_of(sim::Date::from_ymd(1703, 3, 1)) == Season::Spring);
    CHECK(game::season_of(sim::Date::from_ymd(1703, 5, 31)) == Season::Spring);
    CHECK(game::season_of(sim::Date::from_ymd(1703, 6, 1)) == Season::Summer);
    CHECK(game::season_of(sim::Date::from_ymd(1703, 8, 31)) == Season::Summer);
    CHECK(game::season_of(sim::Date::from_ymd(1703, 9, 1)) == Season::Autumn);
    CHECK(game::season_of(sim::Date::from_ymd(1703, 11, 30)) == Season::Autumn);
    CHECK(game::season_of(sim::Date::from_ymd(1703, 12, 1)) == Season::Winter);
}

TEST_CASE("map overview: the silhouette merges the cells of a row into rectangles") {
    const mapdata::MapData map = tiny_map();
    const std::string all = game::build_overview_svg(map, 4, "red");
    CHECK(all.find("viewBox=\"0 0 4 2\"") != std::string::npos);
    CHECK(all.find("fill=\"red\"") != std::string::npos);
    // Row 0: cells 0..2 are land (one run of 3), cell 3 is water; row 1: all four cells.
    CHECK(all.find("M0 0h3v1h-3z") != std::string::npos);
    CHECK(all.find("M0 1h4v1h-4z") != std::string::npos);

    // Only province 2: row 0 has the single cell 2, row 1 the cells 2 and 3.
    const std::vector<std::uint8_t> only_two = {0, 0, 1};
    const std::string owned = game::build_overview_svg(map, 4, "green", only_two);
    CHECK(owned.find("M2 0h1v1h-1z") != std::string::npos);
    CHECK(owned.find("M2 1h2v1h-2z") != std::string::npos);
    CHECK(owned.find("M0 0") == std::string::npos);

    // A coarser grid keeps the proportions of the map.
    const std::string coarse = game::build_overview_svg(map, 2, "red");
    CHECK(coarse.find("viewBox=\"0 0 2 1\"") != std::string::npos);

    // An empty map is a valid, empty picture.
    const std::string empty = game::build_overview_svg(mapdata::MapData{}, 8, "red");
    CHECK(empty.find("d=\"\"") != std::string::npos);
}

TEST_CASE("map overview: the provinces of the player's dynasty") {
    sim::demo::Config cfg;
    cfg.seed = 3;
    cfg.provinces = 50;
    cfg.dynasties = 5;
    cfg.characters_per_dynasty = 4;
    sim::World world = sim::demo::make_world(cfg);
    const sim::DynastyId dynasty(2);
    const std::vector<std::uint8_t> owned = game::owned_provinces(world, 50, {}, dynasty);
    REQUIRE(owned.size() == 51);
    CHECK(owned[0] == 0);
    std::size_t count = 0;
    for (std::size_t i = 0; i < 50; ++i) {
        const bool mine = world.provinces().owner[i] == dynasty;
        CHECK((owned[i + 1] != 0) == mine);
        count += mine ? 1 : 0;
    }
    CHECK(count > 0);
    // No dynasty, nothing owned; a map with more provinces than the world leaves the extras alone.
    for (std::uint8_t v : game::owned_provinces(world, 50, {}, sim::DynastyId{})) {
        CHECK(v == 0);
    }
    CHECK(game::owned_provinces(world, 80, {}, dynasty)[80] == 0);
}

TEST_CASE("map overview: the footprint of the camera and the scale label") {
    // Looking straight down at the middle of a 2000 x 1000 map from a height that shows its whole height.
    const float fov = 0.8f;
    const float distance = 1000.0f / (2.0f * std::tan(fov * 0.5f));
    const ui::MinimapView full = game::minimap_view(1000.0f, 500.0f, distance, fov, 2.0f, 2000.0f, 1000.0f);
    CHECK(full.center_x == doctest::Approx(0.5f));
    CHECK(full.center_y == doctest::Approx(0.5f));
    CHECK(full.width == doctest::Approx(1.0f));
    CHECK(full.height == doctest::Approx(1.0f));
    // Twice as close: half the size; the centre follows the focus.
    const ui::MinimapView near =
        game::minimap_view(500.0f, 250.0f, distance * 0.5f, fov, 2.0f, 2000.0f, 1000.0f);
    CHECK(near.center_x == doctest::Approx(0.25f));
    CHECK(near.center_y == doctest::Approx(0.25f));
    CHECK(near.width == doctest::Approx(0.5f));
    CHECK(near.height == doctest::Approx(0.5f));
    // Further away than the map: clipped to the whole map.
    CHECK(game::minimap_view(0.0f, 0.0f, distance * 9.0f, fov, 2.0f, 2000.0f, 1000.0f).width == 1.0f);
    CHECK(game::minimap_view(0.0f, 0.0f, 10.0f, fov, 2.0f, 0.0f, 0.0f).width == 1.0f);  // no map: harmless

    CHECK(game::round_distance_km(0.4) == 0);
    CHECK(game::round_distance_km(1.0) == 1);
    CHECK(game::round_distance_km(1.9) == 1);
    CHECK(game::round_distance_km(2.0) == 2);
    CHECK(game::round_distance_km(7.0) == 5);
    CHECK(game::round_distance_km(99.0) == 50);
    CHECK(game::round_distance_km(100.0) == 100);
    CHECK(game::round_distance_km(250.0) == 200);
    CHECK(game::round_distance_km(2999.0) == 2000);
    CHECK(game::round_distance_km(5000.0) == 5000);

    // A fifth of the visible width: the whole 4000 km map gives 800 -> 500 km; a tenth of it 80 -> 50 km.
    CHECK(game::scale_label_km(full, 4000) == 500);
    ui::MinimapView tenth;
    tenth.width = 0.1f;
    CHECK(game::scale_label_km(tenth, 4000) == 50);
}
