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
    config.world = {9, 7, 17, 3, sim::Date::from_ymd(1703, 1, 1)};
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
    config.world = {1, 0, 0, 0, sim::Date::from_ymd(1703, 1, 1)};
    config.armies = config.schemes = config.contracts = config.events = 0;
    REQUIRE(stress::generate(config));
    CHECK_FALSE(stress::profile_config("unknown"));
}
