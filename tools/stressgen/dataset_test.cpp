#include "pat/sim/save.hpp"
#include "pat/stress/dataset.hpp"

#include <doctest/doctest.h>

#include <utility>

using namespace pat;

TEST_CASE("stressgen: profiles are valid reproducible datasets that round-trip through both files") {
    for (const auto name : {"year1", "year60", "year60x3"}) {
        auto preset = stress::profile_config(name);
        REQUIRE(preset);
        auto config = preset.value();
        config.world.seed = 7;
        auto a = stress::generate(config);
        auto b = stress::generate(config);
        REQUIRE(a);
        REQUIRE(b);
        const auto& world = a.value().world;
        const auto& load = a.value().workload;
        CHECK(world.dynasties().size() == config.world.dynasties);
        CHECK(world.provinces().size() == config.world.provinces);
        CHECK(world.characters().ids.alive_count() == config.world.characters);
        CHECK(load.armies.owner.size() == config.armies);
        CHECK(load.schemes.actor.size() == config.schemes);
        CHECK(load.contracts.first.size() == config.contracts);
        CHECK(load.events.character.size() == config.events);
        CHECK(world.check_invariants().empty());
        CHECK(stress::check_workload(world, load).empty());
        CHECK(stress::compute_hash(a.value()) == stress::compute_hash(b.value()));
        world.characters().ids.for_each_alive([&](sim::CharacterId id) {
            const auto& c = world.characters();
            CHECK(c.birth_date[id.index()] <= world.today());
            for (const auto parent : {c.father[id.index()], c.mother[id.index()]}) {
                if (parent.valid()) {
                    REQUIRE(world.is_alive(parent));
                    CHECK(c.birth_date[parent.index()] < c.birth_date[id.index()]);
                }
            }
        });

        auto saved = sim::deserialize_save(sim::serialize_save(world));
        REQUIRE(saved);
        auto bytes = stress::serialize_workload(world, load);
        auto loaded = stress::deserialize_workload(bytes, saved.value().world);
        REQUIRE(loaded);
        stress::Dataset restored{std::move(saved.value().world), std::move(loaded).value()};
        CHECK(stress::compute_hash(restored) == stress::compute_hash(a.value()));
        CHECK(stress::serialize_workload(b.value().world, b.value().workload) == bytes);

        config.world.seed = 8;
        auto different = stress::generate(config);
        REQUIRE(different);
        CHECK(stress::compute_hash(different.value()) != stress::compute_hash(a.value()));
        CHECK_FALSE(stress::deserialize_workload(bytes, different.value().world));
        bytes.back() ^= 1;
        CHECK_FALSE(stress::deserialize_workload(bytes, world));
    }
}

TEST_CASE("stressgen: exact counts and invalid workload references are handled") {
    stress::Config config;
    config.world = {9, 7, 17, 3, sim::Date::from_ymd(1703, 1, 1), true, 0};
    config.armies = 4;
    config.schemes = 5;
    config.contracts = 6;
    config.events = 7;
    auto generated = stress::generate(config);
    REQUIRE(generated);
    CHECK(generated.value().world.characters().ids.alive_count() == 17);
    CHECK(generated.value().world.members_of(sim::DynastyId(0)).size() == 6);
    CHECK(generated.value().world.members_of(sim::DynastyId(2)).size() == 5);
    generated.value().workload.armies.owner[0] = sim::DynastyId(99);
    CHECK_FALSE(stress::check_workload(generated.value().world, generated.value().workload).empty());
    const auto invalid = stress::serialize_workload(generated.value().world, generated.value().workload);
    CHECK_FALSE(stress::deserialize_workload(invalid, generated.value().world));
    generated.value().workload.armies.owner.pop_back();
    CHECK_FALSE(stress::check_workload(generated.value().world, generated.value().workload).empty());

    config.world.characters = 0;
    CHECK_FALSE(stress::generate(config));
    config.world.characters = 1'000'001;
    CHECK_FALSE(stress::generate(config));
    config.world = {1, 0, 0, 0, sim::Date::from_ymd(1703, 1, 1), true, 0};
    config.armies = config.schemes = config.contracts = config.events = 0;
    REQUIRE(stress::generate(config));
    CHECK_FALSE(stress::profile_config("unknown"));
}

TEST_CASE("stressgen: worlds use the E7-T02 columns, in dynasty mode by default") {
    stress::Config config;
    config.world = {5, 20, 300, 10, sim::Date::from_ymd(1703, 1, 1), true, 0};
    config.armies = config.schemes = config.contracts = config.events = 0;
    auto generated = stress::generate(config);
    REQUIRE(generated);
    const auto& world = generated.value().world;
    CHECK(world.dynasty_mode());
    CHECK(world.check_invariants().empty());
    CHECK(world.branches().size() >= 10);
    CHECK(world.living().size() == 300);
    CHECK(world.life_queue().size() >= 300);  // one birthday per living character, plus planned deaths
    const auto hash = world.compute_hash();
    CHECK(hash.ext[static_cast<std::size_t>(sim::ExtPart::Mode)] != 0);
    CHECK(hash.ext[static_cast<std::size_t>(sim::ExtPart::CharacterExt)] != 0);
    CHECK(hash.ext[static_cast<std::size_t>(sim::ExtPart::DynastyExt)] != 0);
    // E7-T03: every house has a married founding couple whose children fill the family tree.
    CHECK(hash.ext[static_cast<std::size_t>(sim::ExtPart::Kinship)] != 0);
    const auto& table = world.characters();
    std::size_t married = 0;
    for (const auto id : world.living()) {
        if (table.spouse[id.index()].valid()) {
            ++married;
            CHECK(table.spouse[table.spouse[id.index()].index()] == id);
        }
    }
    CHECK(married == 20);  // ten houses, two spouses each

    // The generated world survives the save container, hash included.
    auto loaded = sim::deserialize_save(sim::serialize_save(world));
    REQUIRE(loaded);
    CHECK(loaded.value().world.compute_hash() == hash);

    // The E1 shape is still available and does not use the mode part.
    config.world.dynasty_mode = false;
    auto old_shape = stress::generate(config);
    REQUIRE(old_shape);
    CHECK_FALSE(old_shape.value().world.dynasty_mode());
    CHECK(old_shape.value().world.life_queue().empty());
    CHECK(old_shape.value().world.compute_hash().ext[static_cast<std::size_t>(sim::ExtPart::Mode)] == 0);
}

TEST_CASE("stressgen: an archive of the dead can be requested (E7-T04)") {
    stress::Config config;
    config.world = {5, 20, 300, 10, sim::Date::from_ymd(1703, 1, 1), true, 600};
    config.armies = config.schemes = config.contracts = config.events = 50;
    auto generated = stress::generate(config);
    REQUIRE(generated);
    const auto& world = generated.value().world;
    CHECK(world.living().size() == 300);
    CHECK(world.archived_count() == 600);
    CHECK(world.characters().ids.capacity() == 900);
    CHECK(world.check_invariants().empty());
    CHECK(stress::check_workload(world, generated.value().workload).empty());  // the workload uses the living
    const auto hash = world.compute_hash();
    CHECK(hash.ext[static_cast<std::size_t>(sim::ExtPart::Archive)] != 0);
    for (std::uint32_t d = 0; d < 10; ++d) {
        CHECK(world.members_of(sim::DynastyId(d)).size() == 30);  // the dead are not members
    }
    CHECK(world.archived_person(sim::CharacterId(300))->death_date == world.today());
    CHECK(world.archived_person(sim::CharacterId(300))->father.valid());

    // The same seed gives the same world; the living part does not change with the archive.
    auto again = stress::generate(config);
    REQUIRE(again);
    CHECK(again.value().world.compute_hash() == hash);
    stress::Config plain = config;
    plain.world.archived = 0;
    auto without = stress::generate(plain);
    REQUIRE(without);
    CHECK(without.value().world.compute_hash().ext[static_cast<std::size_t>(sim::ExtPart::Archive)] == 0);

    auto loaded = sim::deserialize_save(sim::serialize_save(world));
    REQUIRE(loaded);
    CHECK(loaded.value().world.compute_hash() == hash);

    // The E1 shape has no archive and refuses to make one.
    config.world.dynasty_mode = false;
    CHECK_FALSE(stress::generate(config));
}
