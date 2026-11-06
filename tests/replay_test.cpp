#include "pat/core/hash_stream.hpp"
#include "pat/sim/demo_scenario.hpp"
#include "pat/sim/replay.hpp"

#include <doctest/doctest.h>

#include <vector>

using namespace pat::sim;

namespace {

demo::Config config_for(std::uint64_t seed) {
    demo::Config c;
    c.seed = seed;
    c.dynasties = 5;
    c.provinces = 30;
    c.characters_per_dynasty = 5;
    return c;
}

/// Plays a scripted session: external commands at fixed days, recorded into a replay.
Replay record_session(std::uint64_t seed, int days) {
    Simulation sim = demo::make_simulation(config_for(seed));
    ReplayRecorder recorder(seed, demo::kScenarioId, sim.world().today(), 30);
    for (int day = 0; day < days; ++day) {
        if (day % 17 == 0) {
            REQUIRE(sim.submit(
                Command{DynastyId(), cmd::AdjustHonor{DynastyId(static_cast<std::uint32_t>(day % 5)), 7}}));
        }
        if (day % 29 == 0) {
            // owner of province 0 changes hands now and then (accepted by the system actor)
            REQUIRE(sim.submit(Command{
                DynastyId(),
                cmd::SetProvinceOwner{ProvinceId(0), DynastyId(static_cast<std::uint32_t>(day % 4 + 1))}}));
        }
        if (day % 41 == 0) {
            // always rejected: a dynasty may not change its own honor
            REQUIRE(sim.submit(Command{DynastyId(2), cmd::AdjustHonor{DynastyId(2), 1000}}));
        }
        (void)sim.tick();
        recorder.after_tick(sim);
    }
    return recorder.finish(sim);
}

}  // namespace

TEST_CASE("replay: recorder captures accepted external commands and checkpoints") {
    const Replay replay = record_session(1, 365);
    CHECK(replay.total_ticks == 365);
    CHECK(replay.seed == 1);
    CHECK(replay.scenario_id == demo::kScenarioId);
    CHECK(replay.start == Date::from_ymd(1703, 1, 1));
    CHECK_FALSE(replay.commands.empty());

    // Rejected commands never enter a replay.
    for (const ReplayCommand& c : replay.commands) {
        CHECK(c.command.actor == DynastyId());
    }
    // Checkpoints every 30 ticks plus the closing one.
    REQUIRE_FALSE(replay.checkpoints.empty());
    CHECK(replay.checkpoints.front().tick == 30);
    CHECK(replay.checkpoints.back().tick == 365);
    for (std::size_t i = 1; i < replay.checkpoints.size(); ++i) {
        CHECK(replay.checkpoints[i].tick > replay.checkpoints[i - 1].tick);
    }
}

TEST_CASE("replay: playing a recording reproduces every checkpoint") {
    const Replay replay = record_session(2, 400);

    Simulation fresh = demo::make_simulation(config_for(2));
    const ReplayResult result = play_replay(fresh, replay);
    CHECK(result.error.empty());
    CHECK(result.ok);
    CHECK(result.ticks_run == 400);
    CHECK_FALSE(result.divergence.has_value());
}

TEST_CASE("replay: a different seed or a changed command is detected at the right place") {
    const Replay replay = record_session(3, 300);

    {  // wrong seed: the very first checkpoint already differs
        Simulation other = demo::make_simulation(config_for(4));
        const ReplayResult result = play_replay(other, replay);
        REQUIRE_FALSE(result.ok);
        REQUIRE(result.divergence.has_value());
        CHECK(result.divergence->tick == 30);
        CHECK_FALSE(result.divergence->first_differing_part.empty());
    }
    {  // tampered replay: one honor change removed -> divergence appears at the next checkpoint after it
        Replay tampered = replay;
        REQUIRE(tampered.commands.size() > 5);
        const Date removed_day = tampered.commands[5].date;
        tampered.commands.erase(tampered.commands.begin() + 5);

        Simulation sim = demo::make_simulation(config_for(3));
        const ReplayResult result = play_replay(sim, tampered);
        REQUIRE_FALSE(result.ok);
        REQUIRE(result.divergence.has_value());
        CHECK(result.divergence->tick >= (removed_day - tampered.start));
        CHECK(result.divergence->tick - (removed_day - tampered.start) <= tampered.checkpoint_interval);
    }
}

TEST_CASE("replay: preconditions are enforced") {
    const Replay replay = record_session(5, 60);

    Simulation used = demo::make_simulation(config_for(5));
    used.run_days(3);
    CHECK_FALSE(play_replay(used, replay).ok);
    CHECK_FALSE(play_replay(used, replay).error.empty());

    Replay wrong_start = replay;
    wrong_start.start = Date::from_ymd(1704, 1, 1);
    Simulation fresh = demo::make_simulation(config_for(5));
    CHECK_FALSE(play_replay(fresh, wrong_start).error.empty());
}

TEST_CASE("replay: serialization round trip is exact and canonical") {
    const Replay replay = record_session(6, 200);
    const auto bytes = serialize_replay(replay);
    CHECK(bytes == serialize_replay(replay));

    const auto decoded = deserialize_replay(bytes);
    REQUIRE(decoded.has_value());
    CHECK(decoded.value() == replay);
    CHECK(serialize_replay(decoded.value()) == bytes);

    // A deserialized replay plays back just as well.
    Simulation fresh = demo::make_simulation(config_for(6));
    CHECK(play_replay(fresh, decoded.value()).ok);
}

TEST_CASE("replay: damaged files are rejected") {
    const auto good = serialize_replay(record_session(7, 120));
    for (std::size_t i = 0; i < good.size(); i += 5) {
        auto bad = good;
        bad[i] ^= 0x21;
        CHECK_FALSE(deserialize_replay(bad).has_value());
    }
    for (std::size_t n = 0; n < good.size(); n += 9) {
        CHECK_FALSE(deserialize_replay(std::span<const std::uint8_t>(good.data(), n)).has_value());
    }
    CHECK_FALSE(deserialize_replay({}).has_value());
}

TEST_CASE("replay: the divergence report names the first differing subsystem") {
    WorldHash a;
    a.calendar = 1;
    a.rng = 2;
    a.dynasties = 3;
    a.provinces = 4;
    a.characters = 5;
    WorldHash b = a;
    CHECK(first_differing_part(a, b).empty());
    b.characters = 99;
    CHECK(first_differing_part(a, b) == "characters");
    b.provinces = 99;
    CHECK(first_differing_part(a, b) == "provinces");
    b.rng = 99;
    CHECK(first_differing_part(a, b) == "rng");
    b.calendar = 99;
    CHECK(first_differing_part(a, b) == "calendar");
}
