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
#include <thread>

#include "ui_loc_fixture.hpp"

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

TEST_CASE("format_date: Julian dates with month names and layout from the string table") {
    const loc::Localizer& loc = test::ui_localizer();
    CHECK(game::format_date(loc, sim::Date::from_ymd(1703, 1, 1)) == "1 января 1703");
    CHECK(game::format_date(loc, sim::Date::from_ymd(1703, 5, 16)) == "16 мая 1703");
    CHECK(game::format_date(loc, sim::Date::from_ymd(1704, 2, 29)) == "29 февраля 1704");  // Julian leap year
    CHECK(game::format_date(loc, sim::Date::from_ymd(1725, 12, 31)) == "31 декабря 1725");
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
        const std::string text = game::format_date(loc, sim::Date::from_ymd(1710, m, 15));
        CHECK(text == "15 " + std::string(kExpected[m - 1]) + " 1710");
    }

    // Another language changes the month name and the order of the parts.
    loc::Localizer english = test::load_ui_localizer_with_english();
    const std::string_view chain[] = {"en", "ru"};
    REQUIRE(english.set_chain(chain) == 2);
    CHECK(game::format_date(english, sim::Date::from_ymd(1703, 5, 16)) == "May 16, 1703");
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
        const ui::CharacterSnapshot s = game::character_snapshot(test::ui_localizer(), world, id);
        CHECK_FALSE(s.given_name.empty());
        CHECK_FALSE(s.patronymic.empty());
        CHECK_FALSE(s.surname.empty());
        CHECK(s.age == game::age_in_years(world.characters().birth_date[i], world.today()));
        CHECK(s.age >= 0);
        CHECK(test::ui_localizer().resolves(s.health));  // keys of the string table, not text
        CHECK(test::ui_localizer().resolves(s.role));
        CHECK_FALSE(s.family.empty());  // at least "род: ..."
        CHECK(s.family.back().find("[[") == std::string::npos);
        CHECK(s.relations.front().find("[[") == std::string::npos);
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
        if (s.role == "ui.character.role.head") {
            ++heads;
        }
        // Deterministic.
        CHECK(game::character_snapshot(test::ui_localizer(), world, id) == s);
        ++checked;
    }
    CHECK(checked >= 30);
    CHECK(heads >= 1);
    CHECK(heads <= 6);
}

TEST_CASE("dynasty character snapshots use trait queries and explain displayed rounded skills") {
    const loc::Localizer& loc = test::ui_localizer();
    sim::World world(sim::Date::from_ymd(1703, 1, 1), 7);
    REQUIRE(world.enable_dynasty_mode());
    sim::CharacterInit init;
    init.dynasty = world.create_dynasty(core::StringId(0));
    init.birth_date = sim::Date::from_ymd(1670, 1, 1);
    init.health = core::Fixed32::from_int(90);
    init.skills[0] = core::Fixed32::from_ratio(37, 4);
    init.skills[1] = core::Fixed32::from_int(18);
    const sim::CharacterId id = world.create_character(init);
    REQUIRE(id.valid());

    sim::TraitCatalog catalog;
    sim::TraitDef trait;
    trait.id = 1;
    trait.key = "ambitious";
    trait.name_key = "trait.ambitious.name";
    trait.desc_key = "trait.ambitious.desc";
    trait.skill_modifiers[0] = core::Fixed32::from_ratio(5, 4);
    trait.skill_modifiers[1] = core::Fixed32::from_int(10);
    trait.attribute_modifiers[static_cast<std::size_t>(sim::CharacterAttribute::Health)] =
        core::Fixed32::from_int(-20);
    trait.health_multiplier_permille = 500;
    catalog.definitions.push_back(trait);
    REQUIRE(sim::validate(catalog).has_value());
    REQUIRE(sim::grant_trait(world, catalog, id, trait.id) == sim::TraitStatus::Ok);
    const auto hash = world.compute_hash();

    const ui::CharacterSnapshot snapshot = game::character_snapshot(loc, world, id, &catalog);
    CHECK(snapshot.skills[0] == 11);  // 9.25 base + 1.25 trait, rounded from 10.5.
    CHECK(snapshot.skills[1] == 20);  // The effective simulation value is clamped.
    REQUIRE(snapshot.traits.size() == 1);
    CHECK(snapshot.traits[0] == loc.format(trait.name_key));
    CHECK(snapshot.health == "ui.health.weak");  // (90 - 20) * 0.5 = 35.
    REQUIRE(snapshot.skill_explanations[0].terms.size() == 3);
    CHECK(snapshot.skill_explanations[0].label == loc.format(ui::kSkillKeys[0]));
    CHECK(snapshot.skill_explanations[0].terms[0].label == loc.format("ui.explain.base"));
    CHECK(snapshot.skill_explanations[0].terms[0].value == "+9.25");
    CHECK(snapshot.skill_explanations[0].terms[1].label ==
          loc.format("ui.explain.trait_named", {loc::Arg("name", loc.format(trait.name_key))}));
    CHECK(snapshot.skill_explanations[0].terms[1].value == "+1.25");
    CHECK(snapshot.skill_explanations[0].terms[2].label == loc.format("ui.explain.rounding"));
    CHECK(snapshot.skill_explanations[0].terms[2].value == "+0.5");
    CHECK(snapshot.skill_explanations[1].terms.back().label == loc.format("ui.explain.clamp"));
    for (std::size_t skill = 0; skill < ui::kSkillCount; ++skill) {
        const ui::Explanation& explanation = snapshot.skill_explanations[skill];
        CHECK(explanation.value == std::to_string(snapshot.skills[skill]));
        double sum = 0;
        for (const ui::Explanation& term : explanation.terms) {
            sum += std::stod(term.value);
        }
        CHECK(sum == doctest::Approx(snapshot.skills[skill]));
    }

    const ui::Explanation health = game::character_explanation(
        loc,
        catalog,
        sim::attribute_breakdown(world, catalog, id, sim::CharacterAttribute::Health),
        "ui.health.weak");
    CHECK(health.value == "35");
    REQUIRE(health.terms.size() == 3);
    CHECK(health.terms.back().value == "-35");
    CHECK(health.terms.back().note == loc.format("ui.explain.multiplier", {loc::Arg("factor", "0.5")}));
    CHECK(world.compute_hash() == hash);
    CHECK(world.characters().skills[id.index()][0] == init.skills[0]);
    CHECK(world.characters().health[id.index()] == init.health);
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
    std::size_t different_versions = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while ((different_versions < 3 || last_tick <= 100) && std::chrono::steady_clock::now() < deadline) {
        const std::shared_ptr<const sim::World> world = runner.latest_snapshot();
        const std::uint64_t version = runner.snapshot_version();

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
                const ui::CharacterSnapshot s = game::character_snapshot(test::ui_localizer(), *world, c);
                CHECK(s.age == game::age_in_years(world->characters().birth_date[c.index()], world->today()));
            }
        }
        last_version = version;
        last_tick = tick;
        std::this_thread::yield();  // leave the publisher CPU time while waiting for progress
    }
    runner.stop();
    CHECK(different_versions >= 3);  // the simulation really advanced while we were reading
    CHECK(last_tick > 100);
}
