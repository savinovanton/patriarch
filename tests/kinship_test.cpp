// Kinship (E7-T03, ADR-0006 D2/D6): parents and child lists, spouses and former spouses, the
// godparent reservation, the invariants of GDD 01 section 13.3, the "kin" section of the save
// (schema 3) and the schema-2 fixture that must keep loading.

#include "pat/core/bytes.hpp"
#include "pat/sim/save.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <vector>

using namespace pat::sim;

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

}  // namespace

// =================================================================================================
// the schema-2 fixture
// =================================================================================================

TEST_CASE("migration: the schema-2 dynasty-mode fixture loads and keeps its hash") {
    // Made by the code of E7-T02 (state schema 2), before any kinship change:
    //   stressgen --profile year1 --seed 7 --characters 240 --dynasties 8 --provinces 24
    //             --armies 0 --schemes 0 --contracts 0 --events 0 --output dynasty_v2.psv
    const auto file = read_file(fixture_path("dynasty_v2.psv"));
    REQUIRE(file.has_value());
    auto loaded = deserialize_save(file.value());
    REQUIRE(loaded.has_value());
    const World& world = loaded.value().world;
    require_valid(world);

    const WorldHash hash = world.compute_hash();
    CHECK(hash.total == 0x3aa123d9ea55bb03ULL);
    CHECK(hash.calendar == 0x8c7f2e1322674e31ULL);
    CHECK(hash.rng == 0xc6df27438252f6c7ULL);
    CHECK(hash.dynasties == 0x60d711459d77ae89ULL);
    CHECK(hash.provinces == 0x26b46309da939932ULL);
    CHECK(hash.characters == 0x76bb6b7db86975feULL);
    CHECK(world.dynasty_mode());
    CHECK(world.living().size() == 240);
}
