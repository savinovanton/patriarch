// Golden determinism tests (E1-T09).
//
// These constants pin the exact outcome of the built-in demo scenario. They change ONLY when the
// simulation rules, the demo scenario, the hashing scheme or a file format changes on purpose; an
// accidental change (a different compiler, a hidden nondeterminism, a refactoring slip) fails here.
//
// To update after an intentional change:
//   1. run  simcli --seed 42 --days 1461           and copy the printed hash parts,
//   2. run  the "golden: replay file bytes" and "golden: save file bytes" tests, copy the reported
//      hashes (the save hash skips the leading schema word of the payload),
//   3. explain the reason in the commit message.
//
// The six hash values and the replay hash must stay the same when only new, optional state is added
// (extended hash parts are 0 when unused and then do not take part in `total`, ADR-0006 D7).

#include "pat/core/bytes.hpp"
#include "pat/core/hash_stream.hpp"
#include "pat/sim/demo_scenario.hpp"
#include "pat/sim/replay.hpp"
#include "pat/sim/save.hpp"

#include <doctest/doctest.h>

#include <format>
#include <span>
#include <vector>

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
    /* ext */ {},
};
constexpr std::uint64_t kExpectedReplayBytesHash = 0xa4ac03f173dd6a0aULL;
// E7-T02 (schema 2): the payload gained the empty extension block (u32 flags = 0, u16 sections = 0) and
// the hash now skips the schema word; this is the only golden value that changed (ADR-0006 D7).
constexpr std::uint64_t kExpectedSaveBytesHash = 0x4b47132c32ec4b8eULL;

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
    // The demo uses none of the extended features: every extended part is empty (0) and therefore
    // does not take part in `total`.
    for (const std::uint64_t part : actual.ext) {
        CHECK(part == 0);
    }
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
    // The leading schema word is left out of the hash: later schema bumps that do not touch a
    // world like this one (new sections are not written when empty) keep the value, and the word
    // itself is checked separately.
    pat::core::ByteWriter payload;
    sim.world().save_state(payload);
    const std::vector<std::uint8_t>& bytes = payload.data();
    REQUIRE(bytes.size() > 4);
    pat::core::ByteReader schema_word(std::span<const std::uint8_t>(bytes.data(), 4));
    CHECK(schema_word.u32() == World::kStateSchema);
    const std::uint64_t actual = hash_bytes(std::vector<std::uint8_t>(bytes.begin() + 4, bytes.end()));
    MESSAGE("actual save payload hash: " << std::format("{:#018x}", actual));
    CHECK(actual == kExpectedSaveBytesHash);
}
