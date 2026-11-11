#include "pat/core/bytes.hpp"
#include "pat/sim/dynasty_standing.hpp"
#include "pat/sim/save.hpp"
#include "pat/sim/simulation.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

namespace {

using namespace pat::sim;
using pat::core::Fixed32;

constexpr Fixed32 points(int value) {
    return Fixed32::from_int(value);
}

struct StandingFixture {
    World world{Date::from_ymd(1703, 1, 1), 7};
    DynastyId dynasty;

    StandingFixture() {
        REQUIRE(world.enable_dynasty_mode());
        dynasty = world.create_dynasty({});
    }

    void apply_command(const Command& command) {
        REQUIRE(validate(world, command).has_value());
        apply(world, command);
    }
};

}  // namespace

TEST_CASE("dynasty standing: changes are bounded and only effective deltas enter the journal") {
    StandingFixture f;
    const auto id = f.dynasty;
    const Command unity{{}, cmd::AdjustUnity{id, points(80), 12}};
    const Command prestige{{}, cmd::AdjustPrestige{id, points(-80), 13}};
    const Command honor{{}, cmd::AdjustHonorWithReason{id, std::numeric_limits<std::int32_t>::max(), 14}};
    const Command legacy{{}, cmd::AdjustHonor{id, std::numeric_limits<std::int32_t>::min()}};

    CHECK(static_cast<std::uint8_t>(type_of(unity)) == 17);
    CHECK(static_cast<std::uint8_t>(type_of(prestige)) == 20);
    CHECK(static_cast<std::uint8_t>(type_of(honor)) == 21);
    CHECK(static_cast<std::uint8_t>(type_of(legacy)) == 5);
    CHECK_FALSE(validate(f.world, Command{id, unity.payload}).has_value());
    CHECK_FALSE(
        validate(f.world, Command{{}, cmd::AdjustPrestige{DynastyId(99), points(1), 13}}).has_value());

    f.apply_command(unity);
    f.apply_command(unity);  // Already at the upper bound.
    f.apply_command(prestige);
    f.apply_command(prestige);  // Already at the lower bound.
    f.apply_command(honor);
    CHECK(f.world.dynasties().honor[id.index()] == 10000);
    f.apply_command(legacy);

    CHECK(f.world.dynasties().unity[id.index()] == points(100));
    CHECK(f.world.dynasties().prestige[id.index()] == points(0));
    CHECK(f.world.dynasties().honor[id.index()] == -1000);
    const auto changes = f.world.standing_changes(id);
    REQUIRE(changes.size() == 4);
    CHECK(changes[0].date == f.world.today());
    CHECK(changes[0].metric == StandingMetric::Unity);
    CHECK(changes[0].reason == 12);
    CHECK(changes[0].delta == points(50));
    CHECK(changes[1].metric == StandingMetric::Prestige);
    CHECK(changes[1].reason == 13);
    CHECK(changes[1].delta == points(-50));
    CHECK(changes[2].metric == StandingMetric::Honor);
    CHECK(changes[2].reason == 14);
    CHECK(changes[2].delta == points(10000));
    CHECK(changes[3].metric == StandingMetric::Honor);
    CHECK(changes[3].reason == 0);
    CHECK(changes[3].delta == points(-11000));
    CHECK(f.world.check_invariants().empty());
}

TEST_CASE("dynasty standing: the journal keeps the newest entries in chronological order") {
    StandingFixture f;
    const auto start = f.world.today();
    for (std::size_t i = 0; i < kMaxStandingChanges + 2; ++i) {
        const auto reason = static_cast<StandingReason>(10 + i);
        f.apply_command(Command{{}, cmd::AdjustHonorWithReason{f.dynasty, 1, reason}});
        (void)f.world.advance_day();
    }
    const auto changes = f.world.standing_changes(f.dynasty);
    REQUIRE(changes.size() == kMaxStandingChanges);
    for (std::size_t i = 0; i < changes.size(); ++i) {
        CHECK(changes[i].date == start + static_cast<int>(i + 2));
        CHECK(changes[i].reason == static_cast<StandingReason>(i + 12));
        CHECK(changes[i].delta == points(1));
    }
    CHECK(f.world.check_invariants().empty());
}

TEST_CASE("dynasty standing: unity drifts once per month in each dynasty's slot") {
    StandingFixture f;
    const auto first = f.dynasty;                    // Day 1.
    const auto second = f.world.create_dynasty({});  // Day 2.
    f.apply_command(Command{{}, cmd::AdjustUnity{first, points(2), 12}});
    f.apply_command(Command{{}, cmd::AdjustUnity{second, Fixed32::from_ratio(-3, 2), 12}});

    Simulation sim(std::move(f.world));
    sim.add_system(make_dynasty_standing_system());
    sim.run_days(1);
    CHECK(sim.world().dynasties().unity[first.index()] == points(52));
    CHECK(sim.world().dynasties().unity[second.index()] == Fixed32::from_ratio(99, 2));
    sim.run_days(Date::from_ymd(1703, 2, 1) - sim.world().today());
    CHECK(sim.world().dynasties().unity[first.index()] == points(51));
    CHECK(sim.world().dynasties().unity[second.index()] == Fixed32::from_ratio(99, 2));
    sim.run_days(1);
    CHECK(sim.world().dynasties().unity[second.index()] == points(50));
    const auto second_changes = sim.world().standing_changes(second);
    REQUIRE(second_changes.size() == 3);
    CHECK(second_changes[1].date == Date::from_ymd(1703, 1, 2));
    CHECK(second_changes[1].reason == kStandingReasonMonthlyDrift);
    CHECK(second_changes[1].delta == points(1));
    CHECK(second_changes[2].date == Date::from_ymd(1703, 2, 2));
    CHECK(second_changes[2].reason == kStandingReasonMonthlyDrift);
    CHECK(second_changes[2].delta == Fixed32::from_ratio(1, 2));
    CHECK(sim.world().check_invariants().empty());
}

TEST_CASE("dynasty standing: journal, prestige and pending commands survive a save") {
    StandingFixture f;
    const auto id = f.dynasty;
    f.apply_command(Command{{}, cmd::AdjustPrestige{id, points(17), 22}});
    f.apply_command(Command{{}, cmd::AdjustUnity{id, points(-9), 23}});
    const std::array pending{
        Command{{}, cmd::AdjustUnity{id, points(3), 24}},
        Command{{}, cmd::AdjustPrestige{id, points(-5), 25}},
        Command{{}, cmd::AdjustHonorWithReason{id, 6, 26}},
    };
    auto loaded = deserialize_save(serialize_save(f.world, pending));
    REQUIRE(loaded.has_value());
    auto& restored = loaded.value().world;
    CHECK(restored.compute_hash() == f.world.compute_hash());
    CHECK(restored.dynasties().prestige[id.index()] == points(67));
    const auto changes = restored.standing_changes(id);
    REQUIRE(changes.size() == 2);
    CHECK(changes[0].metric == StandingMetric::Prestige);
    CHECK(changes[0].reason == 22);
    CHECK(changes[0].delta == points(17));
    CHECK(changes[1].metric == StandingMetric::Unity);
    CHECK(changes[1].reason == 23);
    CHECK(changes[1].delta == points(-9));
    REQUIRE(loaded.value().pending_internal.size() == pending.size());
    for (std::size_t i = 0; i < pending.size(); ++i) {
        const auto& command = loaded.value().pending_internal[i];
        CHECK(command == pending[i]);
        REQUIRE(validate(restored, command).has_value());
        apply(restored, command);
    }
    CHECK(restored.dynasties().unity[id.index()] == points(44));
    CHECK(restored.dynasties().prestige[id.index()] == points(62));
    CHECK(restored.dynasties().honor[id.index()] == 6);
}

TEST_CASE("dynasty standing: a schema-7 state defaults prestige and the journal") {
    StandingFixture f;
    pat::core::ByteWriter out;
    f.world.save_state(out);
    auto payload = out.take();
    REQUIRE(payload.size() >= 4);
    payload[0] = 7;  // The optional standing section is absent at the defaults.
    pat::core::ByteReader in(payload);
    auto loaded = World::load_state(in);
    REQUIRE(loaded.has_value());
    CHECK(in.at_end());
    CHECK(loaded.value().dynasties().prestige[f.dynasty.index()] == points(50));
    CHECK(loaded.value().standing_changes(f.dynasty).empty());
    CHECK(loaded.value().compute_hash() == f.world.compute_hash());
}
