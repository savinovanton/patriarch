#include "pat/content/m1_scenario.hpp"
#include "pat/content/session_assembly.hpp"
#include "pat/game/save_catalog.hpp"
#include "pat/game/save_loader.hpp"
#include "pat/game/save_service.hpp"
#include "pat/mapdata/map_gen.hpp"
#include "pat/sim/demo_scenario.hpp"
#include "pat/sim/save.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <utility>
#include <vector>

#include "test_files.hpp"

using namespace pat;
using game::LoadOutcome;
using game::LoadReason;
using game::LoadStage;

namespace {

content::SessionContent demo_session() {
    return content::SessionContent::demo();
}

sim::Simulation demo_sim(int days) {
    sim::demo::Config config;
    config.seed = 9;
    sim::Simulation simulation = sim::demo::make_simulation(config);
    simulation.run_days(days);
    return simulation;
}

void write_save(const std::filesystem::path& path,
                const sim::Simulation& simulation,
                std::int64_t saved_at = 0) {
    sim::SaveSummary summary = sim::summary_from_world(simulation.world());
    summary.saved_at = saved_at;
    const auto bytes = sim::serialize_save(simulation.world(), simulation.pending_internal(), summary);
    REQUIRE(sim::write_file_atomic(path, bytes).has_value());
}

std::vector<std::uint8_t> read_bytes(const std::filesystem::path& path) {
    auto bytes = sim::read_file(path);
    REQUIRE(bytes.has_value());
    return std::move(bytes).value();
}

void write_bytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

}  // namespace

TEST_CASE("load_game: a save in the middle continues identically to a straight run") {
    pat::test::TempDir dir("loader_continue");
    const auto path = dir.path() / "mid.psv";
    sim::Simulation first = demo_sim(40);
    write_save(path, first);

    auto result = game::load_game(path, demo_session());
    REQUIRE(result.outcome == LoadOutcome::Ok);
    REQUIRE(result.simulation.has_value());
    CHECK(result.summary.has_value());
    CHECK(result.simulation->world().compute_hash() == first.world().compute_hash());

    result.simulation->run_days(50);
    first.run_days(50);
    CHECK(result.simulation->world().compute_hash() == first.world().compute_hash());
}

TEST_CASE("load_game: progress goes through the stages and never goes back") {
    pat::test::TempDir dir("loader_progress");
    const auto path = dir.path() / "a.psv";
    write_save(path, demo_sim(10));

    std::vector<game::LoadProgress> seen;
    game::LoadOptions options;
    options.on_progress = [&](const game::LoadProgress& p) { seen.push_back(p); };
    const auto result = game::load_game(path, demo_session(), options);
    REQUIRE(result.outcome == LoadOutcome::Ok);
    REQUIRE_FALSE(seen.empty());
    CHECK(seen.front().stage == LoadStage::ReadingHistory);
    CHECK(seen.back().stage == LoadStage::CheckingIntegrity);
    CHECK(seen.back().percent == 100);
    for (std::size_t i = 1; i < seen.size(); ++i) {
        CHECK(seen[i].percent >= seen[i - 1].percent);
        CHECK(seen[i].stage >= seen[i - 1].stage);
    }
}

TEST_CASE("load_game: cancelling between stages returns Cancelled and builds nothing") {
    pat::test::TempDir dir("loader_cancel");
    const auto path = dir.path() / "a.psv";
    write_save(path, demo_sim(10));

    for (int cancel_at = 1; cancel_at <= 3; ++cancel_at) {
        int polls = 0;
        game::LoadOptions options;
        options.cancelled = [&] { return ++polls == cancel_at; };
        const auto result = game::load_game(path, demo_session(), options);
        CHECK(result.outcome == LoadOutcome::Cancelled);
        CHECK_FALSE(result.simulation.has_value());
        CHECK(std::filesystem::exists(path));
    }
    // The file is untouched and loads normally afterwards.
    CHECK(game::load_game(path, demo_session()).outcome == LoadOutcome::Ok);
}

TEST_CASE("load_game: damaged, truncated and missing files are Corrupt, never a crash") {
    pat::test::TempDir dir("loader_corrupt");
    const auto good = dir.path() / "good.psv";
    write_save(good, demo_sim(10));
    const auto bytes = read_bytes(good);

    auto flipped = bytes;
    flipped[flipped.size() - 5] ^= 0x5A;
    write_bytes(dir.path() / "flipped.psv", flipped);
    auto result = game::load_game(dir.path() / "flipped.psv", demo_session());
    CHECK(result.outcome == LoadOutcome::Corrupt);
    CHECK(result.reason == LoadReason::Damaged);
    CHECK_FALSE(result.simulation.has_value());
    CHECK_FALSE(result.backup_available);

    write_bytes(dir.path() / "cut.psv", std::vector<std::uint8_t>(bytes.begin(), bytes.begin() + 30));
    CHECK(game::load_game(dir.path() / "cut.psv", demo_session()).outcome == LoadOutcome::Corrupt);

    write_bytes(dir.path() / "empty.psv", {});
    CHECK(game::load_game(dir.path() / "empty.psv", demo_session()).outcome == LoadOutcome::Corrupt);

    result = game::load_game(dir.path() / "missing.psv", demo_session());
    CHECK(result.outcome == LoadOutcome::Corrupt);
    CHECK(result.reason == LoadReason::UnreadableDisk);

    // A `.bak` next to a damaged file is reported.
    write_bytes(game::backup_path_of(dir.path() / "flipped.psv"), bytes);
    CHECK(game::load_game(dir.path() / "flipped.psv", demo_session()).backup_available);
}

TEST_CASE("load_game: a save of a newer container version needs a newer game") {
    pat::test::TempDir dir("loader_newer");
    const auto path = dir.path() / "new.psv";
    write_save(path, demo_sim(5));
    auto bytes = read_bytes(path);
    bytes[4] = 99;  // the u16 version after the magic
    bytes[5] = 0;
    write_bytes(path, bytes);

    const auto result = game::load_game(path, demo_session());
    CHECK(result.outcome == LoadOutcome::NeedsNewerGame);
    CHECK(result.reason == LoadReason::NewerFormat);
    CHECK_FALSE(result.simulation.has_value());
}

TEST_CASE("load_game: another content set is refused with a reason, --dev skips the check") {
    const auto map = mapdata::generate_map(content::m1_map_params());
    auto scenario = content::load_m1_scenario(PATRIARCH_DATA_DIR, map, 7);
    REQUIRE(scenario.has_value());
    const auto session = content::SessionContent::from_scenario(scenario.value());
    sim::Simulation simulation = content::assemble_session(session, scenario.value().world);
    simulation.run_days(20);

    pat::test::TempDir dir("loader_content");
    const auto path = dir.path() / "m1.psv";
    write_save(path, simulation);

    CHECK(game::load_game(path, session).outcome == LoadOutcome::Ok);

    auto other = session;
    other.content_hash ^= 0x10;
    const auto refused = game::load_game(path, other);
    CHECK(refused.outcome == LoadOutcome::Incompatible);
    CHECK(refused.reason == LoadReason::DifferentContent);
    CHECK_FALSE(refused.simulation.has_value());

    game::LoadOptions dev;
    dev.ignore_content_hash = true;
    CHECK(game::load_game(path, other, dev).outcome == LoadOutcome::Ok);

    // An M1 save does not fit the demo systems, and a demo save does not fit M1 content.
    CHECK(game::load_game(path, demo_session()).outcome == LoadOutcome::Incompatible);
    const auto demo_path = dir.path() / "demo.psv";
    write_save(demo_path, demo_sim(5));
    CHECK(game::load_game(demo_path, session).outcome == LoadOutcome::Incompatible);
}

TEST_CASE("continue_game: loads the newest compatible save and reports when there is none") {
    pat::test::TempDir dir("loader_continue_pick");
    CHECK(game::continue_game(dir.path(), demo_session()).outcome == LoadOutcome::NothingToLoad);

    sim::Simulation older = demo_sim(10);
    sim::Simulation newer = demo_sim(30);
    write_save(dir.path() / "older.psv", older, 1000);
    write_save(dir.path() / "newer.psv", newer, 2000);
    write_bytes(dir.path() / "garbage.psv", {1, 2, 3});

    const auto result = game::continue_game(dir.path(), demo_session());
    REQUIRE(result.outcome == LoadOutcome::Ok);
    CHECK(result.path.filename() == "newer.psv");
    CHECK(result.simulation->world().compute_hash() == newer.world().compute_hash());

    const auto entries = game::scan_saves(dir.path());
    CHECK(game::pick_continue_save(entries) == dir.path() / "newer.psv");
}
