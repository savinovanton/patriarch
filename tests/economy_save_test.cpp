// Saving, loading, hashing and migrating the economy (E9-T03, docs/dev/economy-model.md section 13,
// ADR-0006 D6/D7). The fixtures are real files made by the code of earlier schemas; they must keep
// loading, with the same hash, and with an empty economy.

#include "pat/core/bytes.hpp"
#include "pat/sim/save.hpp"
#include "pat/sim/world.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

using namespace pat::sim;
using pat::core::ByteReader;
using pat::core::ByteWriter;

namespace {

std::filesystem::path fixture_path(const char* name) {
    return std::filesystem::path(PATRIARCH_TEST_DATA_DIR) / "saves" / name;
}

void require_valid(const World& world) {
    const auto problems = world.check_invariants();
    for (const auto& p : problems) {
        MESSAGE(p);
    }
    REQUIRE(problems.empty());
}

std::vector<std::uint8_t> state_bytes(const World& world) {
    ByteWriter out;
    world.save_state(out);
    return out.take();
}

pat::core::Result<World, pat::core::Error> load(std::span<const std::uint8_t> bytes) {
    ByteReader in(bytes);
    auto result = World::load_state(in);
    if (result.has_value() && !in.at_end()) {
        return pat::core::Result<World, pat::core::Error>::err(pat::core::Error{99, "trailing bytes"});
    }
    return result;
}

}  // namespace

// =================================================================================================
// fixtures of the earlier schemas
// =================================================================================================

namespace {

struct FixtureCase {
    const char* file;
    std::uint32_t schema;      // the schema the file was written with
    std::uint64_t total_hash;  // WorldHash::total as the code of that schema computed it
    std::size_t characters;    // living
    std::size_t provinces;
};

constexpr FixtureCase kFixtures[] = {
    // made by the code of E1 (state schema 1): simcli --seed 42 --days 1461 --save ...
    {"e1_demo_v1.psv", 1, 0xe9822290b7ce3cc3ULL, 116, 80},
    // made by the code of E7-T02 (schema 2), E7-T03 (schema 3), E7-T04 (schema 4): stressgen
    {"dynasty_v2.psv", 2, 0x3aa123d9ea55bb03ULL, 240, 24},
    {"dynasty_v3.psv", 3, 0xd11a46b2f4fc2641ULL, 240, 24},
    {"dynasty_v4_archive.psv", 4, 0x0ab1c575c79b7467ULL, 240, 24},
    // made by the code of main right before E9-T03 (schema 4, E9-T02 tables in the World but not saved):
    //   stressgen --profile year1 --seed 11 --characters 300 --dynasties 12 --provinces 60 --archived 60
    //             --armies 0 --schemes 0 --contracts 0 --events 0 --output dynasty_v4_provinces.psv
    {"dynasty_v4_provinces.psv", 4, 0xb10331b93ac1a93fULL, 300, 60},
    //   simcli --seed 42 --days 400 --save e1_demo_v4.psv   (a demo world in the middle of a year)
    {"e1_demo_v4.psv", 4, 0x11402051e903e28dULL, 79, 80},
};

}  // namespace

TEST_CASE("fixtures: every earlier schema loads, keeps its hash and has no economy") {
    for (const FixtureCase& fixture : kFixtures) {
        CAPTURE(fixture.file);
        const auto file = read_file(fixture_path(fixture.file));
        REQUIRE(file.has_value());
        auto loaded = deserialize_save(file.value());
        REQUIRE(loaded.has_value());
        World& world = loaded.value().world;
        require_valid(world);
        const WorldHash hash = world.compute_hash();
        CHECK(hash.total == fixture.total_hash);
        CHECK(world.living().size() == fixture.characters);
        CHECK(world.provinces().size() == fixture.provinces);
        CHECK_FALSE(world.economy_present());
        CHECK(world.economy() == economy::EconomyState{});
        // The state saves and loads again with the same hash, and a second save is identical.
        const std::vector<std::uint8_t> bytes = state_bytes(world);
        auto again = load(bytes);
        REQUIRE(again.has_value());
        CHECK(again.value().compute_hash() == hash);
        CHECK(state_bytes(again.value()) == bytes);
    }
}
