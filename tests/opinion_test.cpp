#include "pat/core/bytes.hpp"
#include "pat/sim/opinion.hpp"
#include "pat/sim/save.hpp"
#include "pat/sim/simulation.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace {

using namespace pat::sim;
using pat::core::Fixed32;

struct OpinionFixture {
    World world{Date::from_ymd(1703, 1, 1), 7};
    DynastyId dynasty;

    OpinionFixture() {
        REQUIRE(world.enable_dynasty_mode());
        dynasty = world.create_dynasty({});
    }

    CharacterId person() {
        CharacterInit init;
        init.dynasty = dynasty;
        init.birth_date = Date::from_ymd(1670, 1, 1);
        const auto id = world.create_character(init);
        REQUIRE(id.valid());
        return id;
    }
};

constexpr Fixed32 points(int value) {
    return Fixed32::from_int(value);
}

Fixed32 sum(std::span<const OpinionSource> sources) {
    Fixed32 total{};
    for (const auto& source : sources) {
        total += source.value;
    }
    return total;
}

Command adjust(CharacterId from, CharacterId to, int delta, OpinionReason reason) {
    return Command{{}, cmd::AdjustOpinion{from, to, points(delta), reason}};
}

std::uint64_t opinion_hash(const World& world) {
    return world.compute_hash().ext[static_cast<std::size_t>(ExtPart::Opinion)];
}

}  // namespace

TEST_CASE("opinion: bounded sources explain the total and opposing changes cancel") {
    OpinionFixture f;
    const auto holder = f.person();
    const auto target = f.person();
    REQUIRE(f.world.add_opinion(holder, target, points(25), 1));
    REQUIRE(f.world.add_opinion(holder, target, points(20), 2));
    REQUIRE(f.world.add_opinion(holder, target, points(-22), 2));
    const auto remaining = f.world.opinion_sources(holder, target);
    REQUIRE(remaining.size() == 1);
    CHECK(remaining.front().reason == 1);
    CHECK(remaining.front().value == points(23));

    REQUIRE(f.world.add_opinion(holder, target, points(-125), 3));
    const auto negative = f.world.opinion_sources(holder, target);
    REQUIRE(negative.size() == 1);
    CHECK(negative.front().reason == 3);
    CHECK(negative.front().value == points(-100));
    REQUIRE(f.world.add_opinion(holder, target, points(100), 3));
    CHECK(f.world.opinion_sources(holder, target).empty());
    CHECK(f.world.opinions_from(holder).empty());

    for (OpinionReason reason = 1; reason <= 4; ++reason) {
        REQUIRE(f.world.add_opinion(holder, target, points(20), reason));
    }
    REQUIRE(f.world.add_opinion(holder, target, points(60), 4));
    const auto sources = f.world.opinion_sources(holder, target);
    CHECK(sources.size() == kMaxOpinionSources);
    CHECK(std::ranges::any_of(sources, [](const OpinionSource& source) { return source.reason == 0; }));
    CHECK(sum(sources) == points(100));
    CHECK(f.world.get_opinion(holder, target) == points(100));
    CHECK(f.world.get_opinion(target, holder) == points(0));
    CHECK(f.world.check_invariants().empty());
}

TEST_CASE("opinion: a full row evicts the weakest absolute value and the smallest tied target") {
    OpinionFixture f;
    const auto holder = f.person();
    const auto smaller_id = f.person();
    std::vector<CharacterId> targets;
    for (std::size_t i = 0; i < kMaxOpinions; ++i) {
        targets.push_back(f.person());
    }
    REQUIRE(targets.size() >= 2);
    for (std::size_t i = targets.size(); i > 2; --i) {
        REQUIRE(f.world.add_opinion(holder, targets[i - 1], points(5), 1));
    }
    REQUIRE(f.world.add_opinion(holder, targets[1], points(2), 1));
    REQUIRE(f.world.add_opinion(holder, targets[0], points(-2), 1));

    (void)f.world.add_opinion(holder, smaller_id, points(2), 1);
    CHECK(f.world.get_opinion(holder, smaller_id) == points(0));
    CHECK(f.world.get_opinion(holder, targets[0]) == points(-2));
    const auto larger_id = f.person();
    (void)f.world.add_opinion(holder, larger_id, points(2), 1);
    const auto weak = f.person();
    (void)f.world.add_opinion(holder, weak, points(-1), 1);
    const auto row = f.world.opinions_from(holder);
    CHECK(row.size() == kMaxOpinions);
    CHECK(std::ranges::is_sorted(row, {}, &OpinionEntry::target));
    CHECK(f.world.get_opinion(holder, targets[0]) == points(0));
    CHECK(f.world.get_opinion(holder, targets[1]) == points(2));
    CHECK(f.world.get_opinion(holder, larger_id) == points(2));
    CHECK(f.world.get_opinion(holder, weak) == points(0));
    CHECK(f.world.check_invariants().empty());
}

TEST_CASE("opinion: monthly slots decay toward zero and clear archived links") {
    OpinionFixture f;
    const auto target = f.person();
    const auto holder = f.person();  // Day 2.
    const auto peer = f.person();    // Day 3.
    const auto deceased = f.person();
    REQUIRE(f.world.add_opinion(holder, target, points(2), 1));
    REQUIRE(f.world.add_opinion(holder, target, Fixed32::from_ratio(1, 2), 2));
    REQUIRE(f.world.add_opinion(peer, target, Fixed32::from_ratio(-1, 2), 1));
    REQUIRE(f.world.add_opinion(holder, deceased, points(10), 1));
    REQUIRE(f.world.add_opinion(deceased, holder, points(-5), 1));
    REQUIRE(f.world.die(deceased, CauseOfDeath::Illness));
    CHECK(f.world.opinions_from(deceased).empty());
    CHECK(f.world.get_opinion(holder, deceased) == points(0));
    CHECK(f.world.opinion_sources(holder, deceased).empty());
    REQUIRE(f.world.check_invariants().empty());

    Simulation sim(std::move(f.world));
    sim.add_system(make_opinion_system());
    sim.run_days(1);
    CHECK(sim.world().get_opinion(holder, target) == Fixed32::from_ratio(3, 2));
    CHECK(sum(sim.world().opinion_sources(holder, target)) == Fixed32::from_ratio(3, 2));
    CHECK(sim.world().opinions_from(holder).size() == 1);
    CHECK(sim.world().get_opinion(peer, target) == Fixed32::from_ratio(-1, 2));
    sim.run_days(1);
    CHECK(sim.world().opinions_from(peer).empty());
    sim.run_days(Date::from_ymd(1703, 2, 1) - sim.world().today());
    CHECK(sim.world().get_opinion(holder, target) == Fixed32::from_ratio(3, 2));
    sim.run_days(1);
    CHECK(sim.world().get_opinion(holder, target) == Fixed32::from_ratio(1, 2));
    sim.run_days(Date::from_ymd(1703, 3, 2) - sim.world().today());
    CHECK(sim.world().opinions_from(holder).empty());
    CHECK(opinion_hash(sim.world()) == 0);
    CHECK(sim.world().check_invariants().empty());
}

TEST_CASE("opinion: state and pending system adjustments survive a save") {
    OpinionFixture f;
    const auto holder = f.person();
    const auto target = f.person();
    const Command command = adjust(holder, target, 10, 1);
    CHECK(static_cast<std::uint8_t>(type_of(command)) == 19);
    CHECK_FALSE(validate(f.world, Command{f.dynasty, command.payload}).has_value());
    CHECK_FALSE(validate(f.world, adjust(holder, holder, 1, 2)).has_value());
    CHECK_FALSE(validate(f.world, adjust(holder, CharacterId(100), 1, 2)).has_value());
    REQUIRE(validate(f.world, command).has_value());
    apply(f.world, command);
    REQUIRE(f.world.add_opinion(holder, target, points(4), 2));

    const Command pending = adjust(holder, target, -2, 2);
    auto loaded = deserialize_save(serialize_save(f.world, std::span(&pending, 1)));
    REQUIRE(loaded.has_value());
    REQUIRE(loaded.value().pending_internal.size() == 1);
    CHECK(loaded.value().pending_internal.front() == pending);
    CHECK(loaded.value().world.compute_hash() == f.world.compute_hash());
    CHECK(opinion_hash(f.world) != 0);
    const auto saved_sources = loaded.value().world.opinion_sources(holder, target);
    REQUIRE(saved_sources.size() == 2);
    CHECK(saved_sources[0].reason == 1);
    CHECK(saved_sources[0].value == points(10));
    CHECK(saved_sources[1].reason == 2);
    CHECK(saved_sources[1].value == points(4));
    REQUIRE(validate(loaded.value().world, loaded.value().pending_internal.front()).has_value());
    apply(loaded.value().world, loaded.value().pending_internal.front());
    CHECK(loaded.value().world.get_opinion(holder, target) == points(12));
}

TEST_CASE("opinion: a schema-6 state opens without opinion rows") {
    OpinionFixture f;
    const auto holder = f.person();
    pat::core::ByteWriter out;
    f.world.save_state(out);
    auto payload = out.take();
    REQUIRE(payload.size() >= 4);
    payload[0] = 6;  // The optional opinion section is absent, so the layout is unchanged.
    pat::core::ByteReader in(payload);
    auto loaded = World::load_state(in);
    REQUIRE(loaded.has_value());
    CHECK(in.at_end());
    CHECK(loaded.value().opinions_from(holder).empty());
    CHECK(loaded.value().compute_hash() == f.world.compute_hash());
}
