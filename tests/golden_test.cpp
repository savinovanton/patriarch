// Golden determinism tests (E1-T09).
//
// These constants pin the exact outcome of the built-in demo scenario. They change ONLY when the
// simulation rules, the demo scenario, the hashing scheme or a file format changes on purpose; an
// accidental change (a different compiler, a hidden nondeterminism, a refactoring slip) fails here.
//
// To update after an intentional change:
//   1. run  simcli --seed 42 --days 1461           and copy the printed hash parts,
//   2. run  the "golden: replay file bytes" test, copy the reported hash,
//   3. explain the reason in the commit message.

#include "pat/core/hash_stream.hpp"
#include "pat/sim/demo_scenario.hpp"
#include "pat/sim/replay.hpp"
#include "pat/sim/save.hpp"

#include <doctest/doctest.h>

#include <format>

using namespace pat::sim;

namespace {

constexpr std::uint64_t kSeed = 42;
constexpr int kDays = 1461;  // 1 January 1703 -> 1 January 1707 (four Julian years)

// Expected state after kDays with seed 42 (default demo::Config otherwise).
constexpr WorldHash kExpectedHash = {
    /* total */ 0xe9822290b7ce3cc3ULL,
    /* calendar */ 0x4807d5e946718425ULL,
    /* rng */ 0xf263ca6972dff83cULL,
    /* dynasties */ 0xaf4a37c18471ae17ULL,
    /* provinces */ 0x5bb5f6e0fb36fc10ULL,
    /* characters */ 0xc2f43c23a0a02683ULL,
};
constexpr std::uint64_t kExpectedReplayBytesHash = 0xa4ac03f173dd6a0aULL;
constexpr std::uint64_t kExpectedSaveBytesHash = 0x58ac47e1b14d57d3ULL;

std::string describe(const WorldHash& h) {
    return std::format("total={:#018x} calendar={:#018x} rng={:#018x} dynasties={:#018x} provinces={:#018x} "
                       "characters={:#018x}",
                       h.total,
                       h.calendar,
                       h.rng,
                       h.dynasties,
                       h.provinces,
                       h.characters);
}

std::uint64_t hash_bytes(const std::vector<std::uint8_t>& bytes) {
    pat::core::HashStream h;
    h.bytes(bytes);
    return h.value();
}

demo::Config golden_config() {
    demo::Config c;
    c.seed = kSeed;
    return c;
}

}  // namespace

TEST_CASE("golden: four years of the demo scenario end in the pinned state") {
    Simulation sim = demo::make_simulation(golden_config());
    sim.run_days(kDays);

    const WorldHash actual = sim.world().compute_hash();
    MESSAGE("actual: " << describe(actual));
    CHECK(sim.world().today() == Date::from_ymd(1707, 1, 1));
    CHECK(actual.calendar == kExpectedHash.calendar);
    CHECK(actual.rng == kExpectedHash.rng);
    CHECK(actual.dynasties == kExpectedHash.dynasties);
    CHECK(actual.provinces == kExpectedHash.provinces);
    CHECK(actual.characters == kExpectedHash.characters);
    CHECK(actual.total == kExpectedHash.total);
}

TEST_CASE("golden: replay file bytes are pinned") {
    Simulation sim = demo::make_simulation(golden_config());
    ReplayRecorder recorder(kSeed, demo::kScenarioId, sim.world().today(), 30);
    for (int i = 0; i < kDays; ++i) {
        (void)sim.tick();
        recorder.after_tick(sim);
    }
    const std::uint64_t actual = hash_bytes(serialize_replay(recorder.finish(sim)));
    MESSAGE("actual replay bytes hash: " << std::format("{:#018x}", actual));
    CHECK(actual == kExpectedReplayBytesHash);
}

TEST_CASE("golden: save file bytes are pinned") {
    Simulation sim = demo::make_simulation(golden_config());
    sim.run_days(kDays);
    // The compressed container depends on the zstd version, so pin the uncompressed payload instead.
    pat::core::ByteWriter payload;
    sim.world().save_state(payload);
    const std::uint64_t actual = hash_bytes(payload.data());
    MESSAGE("actual save payload hash: " << std::format("{:#018x}", actual));
    CHECK(actual == kExpectedSaveBytesHash);
}
