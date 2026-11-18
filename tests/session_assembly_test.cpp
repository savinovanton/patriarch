#include "pat/content/m1_scenario.hpp"
#include "pat/content/session_assembly.hpp"
#include "pat/mapdata/map_gen.hpp"
#include "pat/sim/demo_scenario.hpp"
#include "pat/sim/save.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <utility>

using namespace pat;

namespace {

content::M1Scenario m1_scenario() {
    const auto map = mapdata::generate_map(content::m1_map_params());
    auto result = content::load_m1_scenario(PATRIARCH_DATA_DIR, map, 7);
    if (!result) {
        INFO(result.error().message);
        REQUIRE(result.has_value());
    }
    return std::move(result).value();
}

}  // namespace

TEST_CASE("assemble_session: a save in the middle continues exactly like an uninterrupted run (M1)") {
    const content::M1Scenario scenario = m1_scenario();
    const auto session = content::SessionContent::from_scenario(scenario);

    sim::Simulation straight = content::assemble_session(session, scenario.world);
    straight.run_days(60);

    sim::Simulation first = content::assemble_session(session, scenario.world);
    first.run_days(25);
    const auto bytes = sim::serialize_save(first.world(), first.pending_internal());
    auto loaded = sim::deserialize_save(bytes);
    REQUIRE(loaded.has_value());
    CHECK(session.save_error(loaded.value().world).empty());
    sim::Simulation resumed = content::assemble_session(
        session, std::move(loaded.value().world), std::move(loaded.value().pending_internal));
    resumed.run_days(35);

    CHECK(resumed.world().compute_hash() == straight.world().compute_hash());
}

TEST_CASE("assemble_session: the demo scenario resumes identically too") {
    sim::demo::Config config;
    config.seed = 5;
    const auto session = content::SessionContent::demo();

    sim::Simulation straight = sim::demo::make_simulation(config);
    straight.run_days(90);

    sim::Simulation first = sim::demo::make_simulation(config);
    first.run_days(40);
    auto loaded = sim::deserialize_save(sim::serialize_save(first.world(), first.pending_internal()));
    REQUIRE(loaded.has_value());
    sim::Simulation resumed = content::assemble_session(
        session, std::move(loaded.value().world), std::move(loaded.value().pending_internal));
    resumed.run_days(50);

    CHECK(resumed.world().compute_hash() == straight.world().compute_hash());
}

TEST_CASE("SessionContent::save_error: refuses a world that belongs to other content") {
    const content::M1Scenario scenario = m1_scenario();
    const auto m1 = content::SessionContent::from_scenario(scenario);
    const auto demo = content::SessionContent::demo();

    CHECK(m1.save_error(scenario.world).empty());
    CHECK(demo.save_error(sim::demo::make_world({})).empty());
    CHECK_FALSE(demo.save_error(scenario.world).empty());           // an economy the demo systems cannot run
    CHECK_FALSE(m1.save_error(sim::demo::make_world({})).empty());  // no economy

    auto other = m1;
    other.content_hash ^= 1;
    CHECK_FALSE(other.save_error(scenario.world).empty());
    CHECK(other.save_error(scenario.world, /*ignore_hash=*/true).empty());
}
