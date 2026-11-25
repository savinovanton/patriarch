#pragma once

#include "pat/sim/world.hpp"

#include <doctest/doctest.h>

namespace pat::test::marriage {

using namespace pat::sim;

struct Scene {
    World world;
    DynastyId groom_house;
    DynastyId bride_house;
    DynastyId third_house;
    CharacterId groom;
    CharacterId bride;

    /// Older scenario rows were already scanned; tests of answers/weddings need no cursor work.
    void acknowledge_scenario_characters() {
        REQUIRE(
            world.set_family_godparent_cursor(static_cast<std::uint32_t>(world.characters().ids.capacity())));
    }

    CharacterId person(DynastyId house,
                       Sex sex,
                       Date birth = Date::from_ymd(1670, 1, 1),
                       CharacterId father = {},
                       CharacterId mother = {}) {
        CharacterInit init;
        init.dynasty = house;
        init.sex = sex;
        init.birth_date = birth;
        init.father = father;
        init.mother = mother;
        const auto id = world.create_character(init);
        REQUIRE(id.valid());
        return id;
    }

    explicit Scene(Date today = Date::from_ymd(1703, 5, 20),
                   Date groom_birth = Date::from_ymd(1670, 1, 1),
                   Date bride_birth = Date::from_ymd(1670, 1, 1))
        : world(today, 299) {
        REQUIRE(world.enable_dynasty_mode());
        groom_house = world.create_dynasty({});
        bride_house = world.create_dynasty({});
        third_house = world.create_dynasty({});
        groom = person(groom_house, Sex::Male, groom_birth);
        bride = person(bride_house, Sex::Female, bride_birth);
    }
};

}  // namespace pat::test::marriage
