// AutosaveScheduler (E13-T06, #371): the interval, the rotation of three files (the oldest is replaced),
// skipping an unchanged world, the save on exit, the quick save round trip. A fake clock (milliseconds
// passed in) and a fake wall clock for `saved_at`; no real-time thresholds, everything waits on wait_idle().

#include "pat/content/session_assembly.hpp"
#include "pat/game/autosave.hpp"
#include "pat/game/save_catalog.hpp"
#include "pat/game/save_loader.hpp"
#include "pat/game/save_service.hpp"
#include "pat/sim/demo_scenario.hpp"
#include "pat/sim/save.hpp"

#include <doctest/doctest.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <utility>

#include "test_files.hpp"

using namespace pat;
using game::AutosaveScheduler;
using game::AutosaveSettings;
using game::RequestOutcome;
using game::SaveContext;
using game::SaveMark;
using game::SaveService;

namespace {

constexpr std::int64_t kMinute = 60'000;

struct Fixture {
    explicit Fixture(const char* name) : dir(name) {
        sim::demo::Config config;
        config.seed = 5;
        simulation.emplace(sim::demo::make_simulation(config));
        game::SaveServiceOptions options;
        options.busy_retry_delay_ms = 0;
        options.clock = [this] { return wall.load(); };
        service.emplace(options);
    }

    SaveContext context() const {
        SaveContext c;
        c.summary.scenario = "m1";
        c.mark = {simulation->world().calendar().tick_count(), commands};
        return c;
    }

    /// Writes what the scheduler asked for (the simulation thread's tick boundary) and waits.
    void flush() {
        service->capture(simulation->snapshot(), simulation->pending_internal());
        service->wait_idle();
    }

    void advance(int days) { simulation->run_days(days); }

    pat::test::TempDir dir;
    std::optional<sim::Simulation> simulation;
    std::optional<SaveService> service;
    std::atomic<std::int64_t> wall{1'700'000'000};
    std::uint64_t commands = 0;
};

sim::SaveKind kind_of(const std::filesystem::path& path) {
    auto info = sim::read_save_summary(path);
    REQUIRE(info.has_value());
    REQUIRE(info.value().summary.has_value());
    return info.value().summary->kind;
}

}  // namespace

TEST_CASE("autosave: nothing before the interval, a save after it") {
    Fixture f("autosave_interval");
    AutosaveScheduler scheduler(*f.service, f.dir.path(), AutosaveSettings{5, true});
    scheduler.start(0, f.context().mark);
    f.advance(3);

    CHECK_FALSE(scheduler.poll(5 * kMinute - 1, f.context()).has_value());
    const auto outcome = scheduler.poll(5 * kMinute, f.context());
    REQUIRE(outcome.has_value());
    CHECK(*outcome == RequestOutcome::Queued);
    f.flush();
    const auto file = game::autosave_slot_path(f.dir.path(), 1);
    REQUIRE(std::filesystem::exists(file));
    CHECK(kind_of(file) == sim::SaveKind::Auto);
}

TEST_CASE("autosave: interval 0 never saves; switching it on later takes effect") {
    Fixture f("autosave_off");
    AutosaveScheduler scheduler(*f.service, f.dir.path(), AutosaveSettings{0, true});
    scheduler.start(0, f.context().mark);
    f.advance(3);
    CHECK_FALSE(scheduler.poll(1000 * kMinute, f.context()).has_value());
    scheduler.set_settings(AutosaveSettings{20, true});
    CHECK(scheduler.poll(1000 * kMinute, f.context()).has_value());
}

TEST_CASE("autosave: the rotation replaces the oldest of three files") {
    Fixture f("autosave_rotation");
    AutosaveScheduler scheduler(*f.service, f.dir.path(), AutosaveSettings{5, true});
    scheduler.start(0, f.context().mark);

    std::int64_t now = 0;
    for (int round = 0; round < 4; ++round) {
        f.advance(2);
        f.wall = 1'700'000'000 + 100 * (round + 1);
        now += 5 * kMinute;
        const auto outcome = scheduler.poll(now, f.context());
        REQUIRE(outcome.has_value());
        f.flush();
    }
    // Rounds 0, 1, 2 fill slots 1, 2, 3; round 3 replaces the oldest, slot 1.
    std::int64_t times[3];
    for (int slot = 1; slot <= 3; ++slot) {
        const auto path = game::autosave_slot_path(f.dir.path(), slot);
        REQUIRE(std::filesystem::exists(path));
        auto info = sim::read_save_summary(path);
        REQUIRE(info.has_value());
        times[slot - 1] = info.value().summary->saved_at;
    }
    CHECK(times[0] == 1'700'000'400);
    CHECK(times[1] == 1'700'000'200);
    CHECK(times[2] == 1'700'000'300);
    CHECK(game::next_autosave_slot(f.dir.path()) == 2);
    CHECK_FALSE(std::filesystem::exists(f.dir.path() / "_auto4.psv"));
    CHECK_FALSE(std::filesystem::exists(game::backup_path_of(game::autosave_slot_path(f.dir.path(), 1))));
}

TEST_CASE("autosave: a world that did not change since the last save is skipped") {
    Fixture f("autosave_unchanged");
    AutosaveScheduler scheduler(*f.service, f.dir.path(), AutosaveSettings{5, true});
    scheduler.start(0, f.context().mark);

    CHECK_FALSE(scheduler.poll(5 * kMinute, f.context()).has_value());  // paused since the start
    CHECK_FALSE(std::filesystem::exists(game::autosave_slot_path(f.dir.path(), 1)));

    f.advance(1);
    CHECK_FALSE(scheduler.poll(6 * kMinute, f.context()).has_value());  // the timer restarted at minute 5
    CHECK(scheduler.poll(10 * kMinute, f.context()).has_value());
}

TEST_CASE("autosave: a command on a paused world counts as a change") {
    Fixture f("autosave_command");
    AutosaveScheduler scheduler(*f.service, f.dir.path(), AutosaveSettings{5, true});
    scheduler.start(0, f.context().mark);
    f.commands = 1;
    CHECK(scheduler.poll(5 * kMinute, f.context()).has_value());
}

TEST_CASE("autosave: dropped while another save is queued, asked again afterwards") {
    Fixture f("autosave_busy");
    AutosaveScheduler scheduler(*f.service, f.dir.path(), AutosaveSettings{5, true});
    scheduler.start(0, f.context().mark);
    f.advance(2);

    game::SaveRequest manual;
    manual.path = f.dir.path() / "manual.psv";
    manual.summary.kind = sim::SaveKind::Manual;
    manual.summary.name = "M";
    REQUIRE(f.service->request(std::move(manual)) == RequestOutcome::Queued);

    const auto first = scheduler.poll(5 * kMinute, f.context());
    REQUIRE(first.has_value());
    CHECK(*first == RequestOutcome::Skipped);
    f.flush();
    const auto second = scheduler.poll(5 * kMinute + 1, f.context());
    REQUIRE(second.has_value());
    CHECK(*second == RequestOutcome::Queued);
}

TEST_CASE("autosave: on exit a separate file outside the rotation, only when something changed") {
    Fixture f("autosave_exit");
    AutosaveScheduler scheduler(*f.service, f.dir.path(), AutosaveSettings{5, true});
    scheduler.start(0, f.context().mark);
    CHECK_FALSE(scheduler.save_on_exit(f.context()).has_value());  // loaded and left untouched

    f.advance(4);
    const auto outcome = scheduler.save_on_exit(f.context());
    REQUIRE(outcome.has_value());
    CHECK(*outcome == RequestOutcome::Queued);
    f.flush();
    const auto file = game::autosave_exit_path(f.dir.path());
    REQUIRE(std::filesystem::exists(file));
    CHECK(kind_of(file) == sim::SaveKind::AutoExit);
    CHECK_FALSE(std::filesystem::exists(game::autosave_slot_path(f.dir.path(), 1)));
    CHECK_FALSE(scheduler.save_on_exit(f.context()).has_value());  // already saved
}

TEST_CASE("autosave: on exit off writes nothing") {
    Fixture f("autosave_exit_off");
    AutosaveScheduler scheduler(*f.service, f.dir.path(), AutosaveSettings{5, false});
    scheduler.start(0, f.context().mark);
    f.advance(4);
    CHECK_FALSE(scheduler.save_on_exit(f.context()).has_value());
}

TEST_CASE("quick save: F5 writes `_quick`, F9 loads it back to the same world, even when unchanged") {
    Fixture f("autosave_quick");
    AutosaveScheduler scheduler(*f.service, f.dir.path(), AutosaveSettings{0, false});
    scheduler.start(0, f.context().mark);
    CHECK_FALSE(scheduler.quick_load_path().has_value());  // nothing saved yet

    f.advance(30);
    const sim::WorldHash saved = f.simulation->world().compute_hash();
    REQUIRE(scheduler.quick_save(f.context()) == RequestOutcome::Queued);
    f.flush();
    REQUIRE(f.service->status().state == game::SaveState::Done);
    CHECK(kind_of(game::quick_save_path(f.dir.path())) == sim::SaveKind::Quick);

    f.advance(20);  // play on, then go back
    const auto path = scheduler.quick_load_path();
    REQUIRE(path.has_value());
    auto loaded = game::load_game(*path, content::SessionContent::demo());
    REQUIRE(loaded.outcome == game::LoadOutcome::Ok);
    CHECK(loaded.simulation->world().compute_hash() == saved);

    // The player pressed F5 again without playing: still written.
    REQUIRE(scheduler.quick_save(f.context()) == RequestOutcome::Queued);
    f.flush();
    CHECK(f.service->status().state == game::SaveState::Done);
}

TEST_CASE("autosave: an unreadable slot is the first to be replaced") {
    pat::test::TempDir dir("autosave_corrupt_slot");
    for (int slot = 1; slot <= 3; ++slot) {
        std::ofstream(game::autosave_slot_path(dir.path(), slot), std::ios::binary) << "not a save";
    }
    CHECK(game::next_autosave_slot(dir.path()) == 1);
    CHECK(game::next_autosave_slot(dir.path() / "missing") == 1);
}
