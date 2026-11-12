#include "pat/game/world_view.hpp"
#include "pat/sim/economy/tables.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace {

using pat::core::Fixed32;
using pat::core::StringId;
using pat::sim::Date;
using pat::sim::DynastyId;
using pat::sim::ProvinceId;
using pat::sim::World;
using pat::sim::economy::Group;
using pat::sim::economy::index_of;
using pat::sim::economy::kGroupCount;

World palette_world(std::uint32_t count, bool economy = true) {
    World world(Date::from_ymd(1703, 1, 1), 7);
    const DynastyId dynasty = world.create_dynasty(StringId(1));
    for (std::uint32_t p = 0; p < count; ++p) {
        (void)world.create_province(StringId(100 + p), dynasty, Fixed32::zero());
    }
    if (economy) {
        REQUIRE(world.enable_economy());
    }
    return world;
}

void set_people(World& world, std::uint32_t province, Group group, std::uint32_t people, int mood) {
    REQUIRE(world.set_group_size(ProvinceId(province), group, people) == people);
    world.economy_columns_mut().mood[province * kGroupCount + index_of(group)] = Fixed32::from_int(mood);
}

std::uint32_t red(std::uint32_t color) {
    return color & 0xFFU;
}

std::uint32_t green(std::uint32_t color) {
    return (color >> 8U) & 0xFFU;
}

}  // namespace

TEST_CASE("economy palettes: mood and unrest channels are monotone through their whole ranges") {
    World world = palette_world(201);
    for (std::uint32_t p = 0; p < 201; ++p) {
        set_people(world, p, Group::Peasants, 1, static_cast<int>(p) - 100);
        if (p <= 100) {
            world.economy_columns_mut().unrest[p] = Fixed32::from_int(static_cast<int>(p));
        }
    }
    const auto before = world.compute_hash();
    const auto mood = pat::game::mood_palette(world, 201);
    const auto unrest = pat::game::unrest_palette(world, 201);
    REQUIRE(mood.size() == 202);
    REQUIRE(unrest.size() == 202);
    CHECK(mood[0] == pat::game::kUnownedColor);
    CHECK(unrest[0] == pat::game::kUnownedColor);
    for (std::size_t p = 1; p < mood.size(); ++p) {
        CHECK((mood[p] >> 24U) == 0xFFU);
        CHECK((unrest[p] >> 24U) == 0xFFU);
        if (p > 1) {
            CHECK(red(mood[p]) <= red(mood[p - 1]));
            CHECK(green(mood[p]) >= green(mood[p - 1]));
        }
    }
    for (std::size_t p = 2; p <= 101; ++p) {
        CHECK(red(unrest[p]) >= red(unrest[p - 1]));
        CHECK(green(unrest[p]) <= green(unrest[p - 1]));
    }
    CHECK(red(mood[1]) > green(mood[1]));
    CHECK(green(mood[201]) > red(mood[201]));
    CHECK(red(mood[101]) < red(mood[1]));
    CHECK(red(mood[101]) > red(mood[201]));
    CHECK(green(mood[101]) > green(mood[1]));
    CHECK(green(mood[101]) < green(mood[201]));
    CHECK(unrest[1] == mood[201]);
    CHECK(unrest[101] == mood[1]);
    CHECK(world.compute_hash() == before);
}

TEST_CASE("economy palettes: mood uses population weights and ignores the mood of empty groups") {
    World world = palette_world(6);
    set_people(world, 0, Group::Peasants, 1, -100);
    set_people(world, 0, Group::Nobility, 3, 100);
    set_people(world, 0, Group::Natives, 0, -100);
    set_people(world, 1, Group::Peasants, 4, 50);
    set_people(world, 2, Group::Peasants, 1, -100);
    set_people(world, 2, Group::Townsfolk, 1, 100);
    set_people(world, 3, Group::Peasants, 2, 0);
    set_people(world, 4, Group::Peasants, pat::sim::economy::kMaxProvincePersons, 100);
    set_people(world, 5, Group::Peasants, 1, 100);
    const auto before = world.compute_hash();
    const auto palette = pat::game::mood_palette(world, 6);
    CHECK(palette[1] == palette[2]);  // (1 * -100 + 3 * 100) / 4 == 50
    CHECK(palette[3] == palette[4]);  // opposite groups balance at zero
    CHECK(palette[5] == palette[6]);  // multiplication at the population bound must not overflow
    CHECK(palette[1] != palette[3]);
    CHECK(world.compute_hash() == before);
}

TEST_CASE("economy palettes: explicit map bindings reorder provinces and leave holes neutral") {
    World world = palette_world(3);
    for (std::uint32_t p = 0; p < 3; ++p) {
        set_people(world, p, Group::Peasants, 100, static_cast<int>(p) * 100 - 100);
        world.economy_columns_mut().unrest[p] = Fixed32::from_int(static_cast<int>(p) * 50);
    }
    const std::array<ProvinceId, 6> mapping{
        ProvinceId(), ProvinceId(2), ProvinceId(0), ProvinceId(1), ProvinceId(), ProvinceId(99)};
    const auto default_mood = pat::game::mood_palette(world, 3);
    const auto default_unrest = pat::game::unrest_palette(world, 3);
    const auto mood = pat::game::mood_palette(world, 6, mapping);
    const auto unrest = pat::game::unrest_palette(world, 6, mapping);
    REQUIRE(mood.size() == 7);
    REQUIRE(unrest.size() == 7);
    CHECK(mood[1] == default_mood[3]);
    CHECK(mood[2] == default_mood[1]);
    CHECK(mood[3] == default_mood[2]);
    CHECK(unrest[1] == default_unrest[3]);
    CHECK(unrest[2] == default_unrest[1]);
    CHECK(unrest[3] == default_unrest[2]);
    for (const std::size_t map_id : {0U, 4U, 5U, 6U}) {
        CHECK(mood[map_id] == pat::game::kUnownedColor);
        CHECK(unrest[map_id] == pat::game::kUnownedColor);
    }
    CHECK(pat::game::mood_palette(world, 5)[5] == pat::game::kUnownedColor);
    CHECK(pat::game::unrest_palette(world, 5)[5] == pat::game::kUnownedColor);
    CHECK(pat::game::mood_palette(world, 0).size() == 1);
    CHECK(pat::game::unrest_palette(world, 0).size() == 1);
}

TEST_CASE("economy palettes: missing economy and empty provinces are neutral") {
    const World plain = palette_world(2, false);
    for (const auto color : pat::game::mood_palette(plain, 4)) {
        CHECK(color == pat::game::kUnownedColor);
    }
    for (const auto color : pat::game::unrest_palette(plain, 4)) {
        CHECK(color == pat::game::kUnownedColor);
    }

    World world = palette_world(2);
    set_people(world, 0, Group::Natives, 0, 100);
    world.economy_columns_mut().unrest[0] = Fixed32::from_int(100);
    set_people(world, 1, Group::Peasants, 1, 0);
    CHECK(pat::game::mood_palette(world, 2)[1] == pat::game::kUnownedColor);
    CHECK(pat::game::unrest_palette(world, 2)[1] == pat::game::kUnownedColor);
    CHECK(pat::game::mood_palette(world, 2)[2] != pat::game::kUnownedColor);
    CHECK(pat::game::unrest_palette(world, 2)[2] != pat::game::kUnownedColor);
}

TEST_CASE("economy palettes: out-of-range stored values stay at the nearest endpoint") {
    World world = palette_world(4);
    set_people(world, 0, Group::Peasants, 1, -150);
    set_people(world, 1, Group::Peasants, 1, -100);
    set_people(world, 2, Group::Peasants, 1, 150);
    set_people(world, 3, Group::Peasants, 1, 100);
    auto columns = world.economy_columns_mut();
    columns.unrest[0] = Fixed32::from_int(-20);
    columns.unrest[1] = Fixed32::zero();
    columns.unrest[2] = Fixed32::from_int(120);
    columns.unrest[3] = Fixed32::from_int(100);
    const auto mood = pat::game::mood_palette(world, 4);
    const auto unrest = pat::game::unrest_palette(world, 4);
    CHECK(mood[1] == mood[2]);
    CHECK(mood[3] == mood[4]);
    CHECK(unrest[1] == unrest[2]);
    CHECK(unrest[3] == unrest[4]);
}
