#include "pat/core/telemetry.hpp"
#include "pat/sim/demo_scenario.hpp"
#include "pat/sim/runner.hpp"
#include "pat/sim/save.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>

namespace core = pat::core;
namespace sim = pat::sim;

const core::MetricSummary& metric(const core::TelemetrySnapshot& snapshot, std::string_view name) {
    const auto found = std::find_if(snapshot.metrics.begin(), snapshot.metrics.end(), [&](const auto& value) {
        return value.name == name;
    });
    REQUIRE(found != snapshot.metrics.end());
    return *found;
}

TEST_CASE("telemetry: fixed tick window, inactive ticks and repeated scopes have distinct statistics") {
    auto created = core::Telemetry::create(3, 2);
    REQUIRE(created.has_value());
    auto& telemetry = *created.value();
    const auto slow = telemetry.register_metric("monthly");
    const auto unused = telemetry.register_metric("unused");
    REQUIRE(slow.has_value());
    REQUIRE(unused.has_value());
    CHECK(telemetry.register_metric("monthly").value() == slow.value());
    CHECK_FALSE(telemetry.register_metric("overflow"));
    CHECK_FALSE(telemetry.record(slow.value(), 1));
    REQUIRE(telemetry.begin_frame(10));
    CHECK_FALSE(telemetry.begin_frame(11));
    CHECK_FALSE(telemetry.register_metric("inside"));
    CHECK(telemetry.record(slow.value(), 999));  // Overwritten by the fourth frame.
    telemetry.end_frame();
    REQUIRE(telemetry.begin_frame(11));
    CHECK(telemetry.record(slow.value(), 40));
    CHECK(telemetry.record(slow.value(), 60));
    telemetry.end_frame();
    REQUIRE(telemetry.begin_frame(12));
    telemetry.end_frame();
    REQUIRE(telemetry.begin_frame(13));
    CHECK(telemetry.record(slow.value(), 300));
    CHECK_FALSE(telemetry.record(core::MetricId(), 1));
    telemetry.end_frame();
    const auto snapshot = telemetry.snapshot(true);
    CHECK(snapshot.frame_ids == std::vector<std::int64_t>{11, 12, 13});
    CHECK(snapshot.frames == 3);
    CHECK(snapshot.first_frame == 11);
    CHECK(snapshot.last_frame == 13);
    const auto& m = metric(snapshot, "monthly");
    CHECK(m.calls == 3);
    CHECK(m.active_ticks == 2);
    CHECK(m.history[0].duration_ns == 100);
    CHECK(m.history[1].calls == 0);
    CHECK(m.per_tick.samples == 3);
    CHECK(m.per_tick.total_ns == 400);
    CHECK(m.per_tick.mean_ns == 133);
    CHECK(m.per_tick.p50_ns == 100);
    CHECK(m.per_tick.p95_ns == 300);
    CHECK(m.active.samples == 2);
    CHECK(m.active.mean_ns == 200);
    CHECK(m.active.p50_ns == 100);  // nearest rank, not interpolation.
    CHECK(metric(snapshot, "unused").active.samples == 0);
    telemetry.clear();
    CHECK(telemetry.snapshot().frames == 0);
    CHECK(snapshot.frames == 3);  // Detached snapshot was not cleared with the collector.
}

TEST_CASE("telemetry: bounded setup, saturating durations and escaped exports") {
    CHECK_FALSE(core::Telemetry::create(0));
    CHECK_FALSE(core::Telemetry::create(16385));
    CHECK_FALSE(core::Telemetry::create(16384, 128));
    auto created = core::Telemetry::create(2, 1);
    REQUIRE(created.has_value());
    auto& telemetry = *created.value();
    const auto id = telemetry.register_metric("quote\",line\n");
    REQUIRE(id.has_value());
    REQUIRE(telemetry.begin_frame(1));
    CHECK(telemetry.record(id.value(), std::numeric_limits<std::uint64_t>::max()));
    CHECK(telemetry.record(id.value(), 1));
    telemetry.end_frame();
    const auto snapshot = telemetry.snapshot();
    CHECK(snapshot.saturated);
    CHECK(snapshot.metrics[0].last.duration_ns == std::numeric_limits<std::uint64_t>::max());
    CHECK(core::telemetry_json(snapshot).find("quote\\\",line\\u000a") != std::string::npos);
    CHECK(core::telemetry_csv(snapshot).find("\"quote\"\",line\n\"") != std::string::npos);
}

TEST_CASE("telemetry: RAII timers finalize before a frame and cadence tags share one duration") {
    auto created = core::Telemetry::create(4, 2);
    REQUIRE(created.has_value());
    auto& telemetry = *created.value();
    const auto total = telemetry.register_metric("total").value();
    const auto monthly = telemetry.register_metric("monthly").value();
    {
        core::TelemetryFrame frame(&telemetry, 1);
        PAT_SCOPED_TIMER(&telemetry, total, "test.telemetry");
        core::ScopedTimer tagged(&telemetry, monthly);
        tagged.include(total);
        tagged.include(total);  // One tag must not record twice.
    }
    const auto snapshot = telemetry.snapshot();
    CHECK(metric(snapshot, "total").calls == 2);
    CHECK(metric(snapshot, "monthly").calls == 1);
    CHECK(metric(snapshot, "total").last.duration_ns >= metric(snapshot, "monthly").last.duration_ns);
    core::ScopedTimer disabled(nullptr, total);
}

TEST_CASE("telemetry: enabling observation preserves world, WU, commands and save bytes") {
    auto reference = sim::demo::make_simulation({7});
    auto observed = sim::demo::make_simulation({7});
    REQUIRE(observed.enable_telemetry(64).has_value());
    std::uint64_t monthly = 0, yearly = 0;
    for (int day = 0; day < 400; ++day) {
        const auto& a = reference.tick();
        const auto& b = observed.tick();
        CHECK(a.wu_used == b.wu_used);
        CHECK(a.events == b.events);
        CHECK(a.internal_commands.applied == b.internal_commands.applied);
        if (day >= 336) {
            monthly += b.events.new_month;
            yearly += b.events.new_year;
        }
    }
    CHECK(reference.world().compute_hash() == observed.world().compute_hash());
    CHECK(reference.command_log() == observed.command_log());
    CHECK(sim::serialize_save(reference.world(), reference.pending_internal()) ==
          sim::serialize_save(observed.world(), observed.pending_internal()));
    const auto snapshot = observed.telemetry_snapshot();
    REQUIRE(snapshot);
    CHECK(snapshot->frames == 64);
    CHECK(snapshot->first_frame == 337);
    CHECK(snapshot->last_frame == 400);
    CHECK(metric(*snapshot, "sim.tick").calls == 64);
    CHECK(metric(*snapshot, "system.demo.ageing").calls == 64);
    CHECK(metric(*snapshot, "system.demo.births").calls == monthly);
    CHECK(metric(*snapshot, "sim.tick.monthly").calls == monthly);
    CHECK(metric(*snapshot, "sim.tick.yearly").calls == yearly);
    observed.restore_state(reference.world(), {});
    CHECK(observed.telemetry_snapshot()->frames == 0);
}

TEST_CASE("telemetry: runner publishes detached summaries for another thread") {
    auto simulation = sim::demo::make_simulation({7});
    REQUIRE(simulation.enable_telemetry(16).has_value());
    sim::SimulationRunner runner(std::move(simulation));
    const auto initial = runner.latest_telemetry();
    REQUIRE(initial);
    CHECK(initial->frames == 0);
    runner.start();
    runner.step(80);
    const bool reached = runner.wait_for_ticks(80, std::chrono::seconds(5));
    runner.stop();
    REQUIRE(reached);
    const auto latest = runner.latest_telemetry();
    REQUIRE(latest);
    CHECK(latest->frames == 16);
    CHECK(latest->last_frame == 80);
    CHECK(initial->frames == 0);
}
