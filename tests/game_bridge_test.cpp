// The simulation -> UI bridge (E3-T12): palettes, window data, dates, and the guarantee that readers get
// consistent snapshots while the simulation thread runs.

#include "pat/game/world_view.hpp"
#include "pat/sim/demo_scenario.hpp"
#include "pat/sim/runner.hpp"
#include "pat/sim/simulation.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <set>

using namespace pat;

namespace {

sim::World demo_world(std::uint32_t provinces = 100, std::uint32_t dynasties = 10) {
    sim::demo::Config cfg;
    cfg.seed = 3;
    cfg.provinces = provinces;
    cfg.dynasties = dynasties;
    cfg.characters_per_dynasty = 6;
    return sim::demo::make_world(cfg);
}

}  // namespace

TEST_CASE("format_date_ru: Julian dates with Russian month names") {
    CHECK(game::format_date_ru(sim::Date::from_ymd(1703, 1, 1)) == "1 января 1703");
    CHECK(game::format_date_ru(sim::Date::from_ymd(1703, 5, 16)) == "16 мая 1703");
    CHECK(game::format_date_ru(sim::Date::from_ymd(1704, 2, 29)) == "29 февраля 1704");  // Julian leap year
    CHECK(game::format_date_ru(sim::Date::from_ymd(1725, 12, 31)) == "31 декабря 1725");
    static constexpr const char* kExpected[12] = {"января",
                                                  "февраля",
                                                  "марта",
                                                  "апреля",
                                                  "мая",
                                                  "июня",
                                                  "июля",
                                                  "августа",
                                                  "сентября",
                                                  "октября",
                                                  "ноября",
                                                  "декабря"};
    for (int m = 1; m <= 12; ++m) {
        const std::string text = game::format_date_ru(sim::Date::from_ymd(1710, m, 15));
        CHECK(text == "15 " + std::string(kExpected[m - 1]) + " 1710");
    }
}

TEST_CASE("dynasty colours are distinct, opaque and stable") {
    std::set<std::uint32_t> colors;
    for (std::uint32_t i = 0; i < 64; ++i) {
        const std::uint32_t c = game::dynasty_color(sim::DynastyId(i));
        CHECK((c >> 24U) == 0xFFU);
        colors.insert(c);
        CHECK(c == game::dynasty_color(sim::DynastyId(i)));
    }
    CHECK(colors.size() >= 60);  // essentially all distinct
    CHECK(game::dynasty_color(sim::DynastyId{}) == game::kUnownedColor);
}

TEST_CASE("owner palette follows province ownership") {
    sim::World world = demo_world(100, 10);
    auto palette = game::owner_palette(world, 100);
    REQUIRE(palette.size() == 101);
    for (std::size_t i = 0; i < 100; ++i) {
        CHECK(palette[i + 1] == game::dynasty_color(world.provinces().owner[i]));
    }

    // Changing one owner changes exactly that entry.
    const sim::DynastyId before = world.provinces().owner[41];
    const sim::DynastyId other(before.valid() && before.value() == 3 ? 4U : 3U);
    world.set_province_owner(sim::ProvinceId(41), other);
    const auto changed = game::owner_palette(world, 100);
    for (std::size_t i = 0; i < palette.size(); ++i) {
        if (i == 42) {
            CHECK(changed[i] == game::dynasty_color(other));
            CHECK(changed[i] != palette[i]);
        } else {
            CHECK(changed[i] == palette[i]);
        }
    }

    // A map with more provinces than the world has leaves the extras unowned; fewer is fine too.
    CHECK(game::owner_palette(world, 150)[150] == game::kUnownedColor);
    CHECK(game::owner_palette(world, 20).size() == 21);
}

TEST_CASE("development palette maps values to a red-to-green ramp") {
    const sim::World world = demo_world(60, 6);
    const auto palette = game::development_palette(world, 60);
    REQUIRE(palette.size() == 61);
    for (std::size_t i = 0; i < 60; ++i) {
        CHECK(palette[i + 1] != 0U);
        CHECK((palette[i + 1] >> 24U) == 0xFFU);
    }
}

TEST_CASE("character snapshots describe real characters of a world") {
    sim::World world = demo_world(60, 6);
    const std::span<const std::uint8_t> alive = world.characters().ids.alive_flags();
    std::size_t checked = 0;
    std::size_t heads = 0;
    for (std::size_t i = 0; i < alive.size(); ++i) {
        if (alive[i] == 0) {
            continue;
        }
        const sim::CharacterId id(static_cast<std::uint32_t>(i));
        const ui::CharacterSnapshot s = game::character_snapshot(world, id);
        CHECK_FALSE(s.given_name.empty());
        CHECK_FALSE(s.patronymic.empty());
        CHECK_FALSE(s.surname.empty());
        CHECK(s.age == game::age_in_years(world.characters().birth_date[i], world.today()));
        CHECK(s.age >= 0);
        CHECK_FALSE(s.health.empty());
        CHECK_FALSE(s.family.empty());  // at least "род: ..."
        CHECK(s.relations.size() <= 400);
        CHECK(s.relations.size() >= 5);  // kin and acquaintances
        for (const int v : s.skills) {
            CHECK(v >= 0);
            CHECK(v <= ui::kSkillMax);
        }
        for (std::size_t k = 0; k < ui::kSkillCount; ++k) {
            CHECK(std::stoi(s.skill_explanations[k].value) == s.skills[k]);
        }
        // Women get the feminine surname form.
        if (world.characters().sex[i] == sim::Sex::Female) {
            const std::string& sn = s.surname;
            CHECK(((sn.size() >= 2 && sn.compare(sn.size() - 2, 2, "а") == 0) ||
                   sn.find("ая") != std::string::npos));
        }
        if (s.role == "глава рода") {
            ++heads;
        }
        // Deterministic.
        CHECK(game::character_snapshot(world, id) == s);
        ++checked;
    }
    CHECK(checked >= 30);
    CHECK(heads >= 1);
    CHECK(heads <= 6);
}

TEST_CASE("province heads and random characters") {
    const sim::World world = demo_world(60, 6);
    std::size_t with_head = 0;
    for (std::size_t p = 1; p <= 60; ++p) {
        const sim::CharacterId head = game::head_for_province(world, p);
        if (head.valid()) {
            ++with_head;
            CHECK(world.is_alive(head));
            // The head belongs to the dynasty that owns the province.
            CHECK(world.characters().dynasty[head.index()] == world.provinces().owner[p - 1]);
        }
    }
    CHECK(with_head >= 30);
    CHECK_FALSE(game::head_for_province(world, 0).valid());
    CHECK_FALSE(game::head_for_province(world, 9999).valid());

    std::set<std::uint32_t> picked;
    for (std::uint32_t seed = 0; seed < 50; ++seed) {
        const sim::CharacterId c = game::pick_character(world, seed);
        REQUIRE(c.valid());
        CHECK(world.is_alive(c));
        CHECK(game::pick_character(world, seed) == c);
        picked.insert(c.value());
    }
    CHECK(picked.size() > 10);
}

TEST_CASE("age_in_years uses Julian years") {
    const sim::Date born = sim::Date::from_ymd(1680, 6, 1);
    CHECK(game::age_in_years(born, sim::Date::from_ymd(1703, 5, 31)) == 22);
    CHECK(game::age_in_years(born, sim::Date::from_ymd(1703, 6, 2)) == 23);
    CHECK(game::age_in_years(sim::Date::from_ymd(1710, 1, 1), sim::Date::from_ymd(1703, 1, 1)) ==
          0);  // never negative
}

TEST_CASE("the UI can read snapshots while the simulation thread runs, and every snapshot is consistent") {
    sim::demo::Config cfg;
    cfg.seed = 9;
    cfg.provinces = 120;
    cfg.dynasties = 12;
    cfg.characters_per_dynasty = 8;
    sim::SimulationRunner runner(sim::demo::make_simulation(cfg));
    runner.start();
    runner.set_speed(sim::Speed::Unlimited);

    std::uint64_t last_version = 0;
    std::int64_t last_tick = -1;
    std::chrono::nanoseconds slowest{0};
    std::size_t different_versions = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto t0 = std::chrono::steady_clock::now();
        const std::shared_ptr<const sim::World> world = runner.latest_snapshot();
        const std::uint64_t version = runner.snapshot_version();
        slowest = std::max(slowest, std::chrono::steady_clock::now() - t0);

        REQUIRE(world != nullptr);
        CHECK(version >= last_version);
        // The snapshot is a whole, valid world: invariants hold and everything the UI derives from it agrees.
        const std::int64_t tick = world->calendar().tick_count();
        CHECK(tick >= last_tick);
        if (version != last_version) {
            ++different_versions;
            CHECK(world->check_invariants().empty());
            const auto palette = game::owner_palette(*world, 120);
            for (std::size_t i = 0; i < 120; ++i) {
                CHECK(palette[i + 1] == game::dynasty_color(world->provinces().owner[i]));
            }
            const sim::CharacterId c = game::pick_character(*world, 5);
            if (c.valid()) {
                const ui::CharacterSnapshot s = game::character_snapshot(*world, c);
                CHECK(s.age == game::age_in_years(world->characters().birth_date[c.index()], world->today()));
            }
        }
        last_version = version;
        last_tick = tick;
    }
    runner.stop();
    CHECK(different_versions >= 3);  // the simulation really advanced while we were reading
    CHECK(last_tick > 100);
    // Reading the latest snapshot is a pointer copy: it never waited for a tick.
    CHECK(slowest < std::chrono::milliseconds(20));
}
