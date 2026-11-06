#include "pat/core/rng.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <numeric>
#include <vector>

using namespace pat::core;

TEST_CASE("rng: PCG32 matches the reference implementation") {
    // pcg32_srandom_r(&rng, 42u, 54u) from the official pcg-c demo.
    Pcg32 rng(42, 54);
    const std::array<std::uint32_t, 6> expected = {
        0xa15c02b7U, 0x7b47f409U, 0xba1d3330U, 0x83d2f293U, 0xbfa4784bU, 0xcbed606eU};
    for (const std::uint32_t value : expected) {
        CHECK(rng.next_u32() == value);
    }
}

TEST_CASE("rng: SplitMix64 and FNV-1a known answers") {
    std::uint64_t s = 0;
    CHECK(split_mix64_next(s) == 0xe220a8397b1dcdafULL);
    CHECK(split_mix64_next(s) == 0x6e789e6aa1b965f4ULL);
    CHECK(split_mix64_next(s) == 0x06c45d188009454fULL);

    CHECK(fnv1a64("") == 0xcbf29ce484222325ULL);
    CHECK(fnv1a64("events") == 0xdd63d0cec6bb7d04ULL);
    CHECK(fnv1a64("ai.dynasty.17") == 0x2464419d2c15c2e1ULL);
}

TEST_CASE("rng: same seed gives the same sequence, different streams differ") {
    Pcg32 a(123, 1);
    Pcg32 b(123, 1);
    Pcg32 c(123, 2);
    Pcg32 d(124, 1);
    bool differs_stream = false;
    bool differs_seed = false;
    for (int i = 0; i < 100; ++i) {
        const std::uint32_t va = a.next_u32();
        CHECK(va == b.next_u32());
        differs_stream = differs_stream || va != c.next_u32();
        differs_seed = differs_seed || va != d.next_u32();
    }
    CHECK(differs_stream);
    CHECK(differs_seed);
}

TEST_CASE("rng: bounded is unbiased across buckets") {
    Pcg32 rng(2024, 7);
    constexpr std::uint32_t kBuckets = 10;
    constexpr int kDraws = 200000;
    std::array<int, kBuckets> counts{};
    for (int i = 0; i < kDraws; ++i) {
        const std::uint32_t v = rng.bounded(kBuckets);
        REQUIRE(v < kBuckets);
        ++counts[v];
    }
    for (const int c : counts) {
        CHECK(c > kDraws / static_cast<int>(kBuckets) * 97 / 100);
        CHECK(c < kDraws / static_cast<int>(kBuckets) * 103 / 100);
    }
    CHECK(rng.bounded(1) == 0);
}

TEST_CASE("rng: range is inclusive and handles negatives and the full interval") {
    Pcg32 rng(5, 5);
    bool saw_lo = false;
    bool saw_hi = false;
    for (int i = 0; i < 2000; ++i) {
        const std::int32_t v = rng.range(-3, 3);
        CHECK(v >= -3);
        CHECK(v <= 3);
        saw_lo = saw_lo || v == -3;
        saw_hi = saw_hi || v == 3;
    }
    CHECK(saw_lo);
    CHECK(saw_hi);
    CHECK(rng.range(7, 7) == 7);
    (void)rng.range(INT32_MIN, INT32_MAX);  // span wraps to 0: must not hang or crash
}

TEST_CASE("rng: chance_permille edge cases and frequency") {
    Pcg32 rng(99, 3);
    for (int i = 0; i < 100; ++i) {
        CHECK_FALSE(rng.chance_permille(0));
        CHECK_FALSE(rng.chance_permille(-5));
        CHECK(rng.chance_permille(1000));
        CHECK(rng.chance_permille(5000));
    }
    int hits = 0;
    constexpr int kDraws = 100000;
    for (int i = 0; i < kDraws; ++i) {
        hits += rng.chance_permille(250) ? 1 : 0;
    }
    CHECK(hits > kDraws * 24 / 100);
    CHECK(hits < kDraws * 26 / 100);
}

TEST_CASE("rng: shuffle is a deterministic permutation") {
    std::vector<int> base(50);
    std::iota(base.begin(), base.end(), 0);

    auto shuffled = [&](std::uint64_t seed) {
        std::vector<int> v = base;
        Pcg32 rng(seed, 1);
        rng.shuffle(std::span<int>(v));
        return v;
    };

    const auto a = shuffled(1);
    const auto b = shuffled(1);
    const auto c = shuffled(2);
    CHECK(a == b);
    CHECK(a != c);
    CHECK(a != base);

    auto sorted = a;
    std::sort(sorted.begin(), sorted.end());
    CHECK(sorted == base);

    // The first position should be roughly uniform over many seeds.
    std::array<int, 5> first{};
    for (std::uint64_t seed = 0; seed < 5000; ++seed) {
        std::array<int, 5> items = {0, 1, 2, 3, 4};
        Pcg32 rng(seed, 9);
        rng.shuffle(std::span<int>(items));
        ++first[static_cast<std::size_t>(items[0])];
    }
    for (const int count : first) {
        CHECK(count > 850);
        CHECK(count < 1150);
    }
}

TEST_CASE("rng: advance equals repeated draws and can step backwards") {
    Pcg32 stepped(77, 4);
    Pcg32 jumped(77, 4);
    for (int i = 0; i < 1000; ++i) {
        (void)stepped.next_u32();
    }
    jumped.advance(1000);
    CHECK(stepped == jumped);
    CHECK(stepped.next_u32() == jumped.next_u32());

    // Back 5 draws and replay them.
    Pcg32 rng(8, 8);
    std::array<std::uint32_t, 5> first{};
    for (auto& v : first) {
        v = rng.next_u32();
    }
    rng.advance(0 - std::uint64_t{5});
    for (const auto v : first) {
        CHECK(rng.next_u32() == v);
    }
}

TEST_CASE("rng: state can be saved and restored") {
    Pcg32 rng(31337, 6);
    for (int i = 0; i < 10; ++i) {
        (void)rng.next_u32();
    }
    Pcg32 copy = Pcg32::from_state(rng.state(), rng.inc());
    for (int i = 0; i < 50; ++i) {
        CHECK(rng.next_u32() == copy.next_u32());
    }
}

TEST_CASE("rng streams: sequence depends only on master seed and name") {
    RngStreams one(1000);
    RngStreams two(1000);

    // Different creation order must not change any stream.
    const std::uint32_t a1 = one.stream("events").next_u32();
    const std::uint32_t b1 = one.stream("combat").next_u32();
    const std::uint32_t b2 = two.stream("combat").next_u32();
    const std::uint32_t a2 = two.stream("events").next_u32();
    CHECK(a1 == a2);
    CHECK(b1 == b2);
    CHECK(a1 != b1);

    RngStreams other_seed(1001);
    CHECK(other_seed.stream("events").next_u32() != a1);
}

TEST_CASE("rng streams: streams are independent and references are stable") {
    RngStreams streams(7);
    Pcg32& events = streams.stream("events");
    const std::uint32_t first = events.next_u32();

    RngStreams reference(7);
    Pcg32& reference_events = reference.stream("events");
    CHECK(reference_events.next_u32() == first);

    // Drawing from another stream (and creating many more) does not disturb "events".
    for (int i = 0; i < 100; ++i) {
        (void)streams.stream("noise", static_cast<std::uint64_t>(i)).next_u32();
    }
    CHECK(events.next_u32() == reference_events.next_u32());
    CHECK(&events == &streams.stream("events"));
}

TEST_CASE("rng streams: indexed families give distinct, reproducible streams") {
    RngStreams a(55);
    RngStreams b(55);
    const std::uint32_t d17 = a.stream("ai.dynasty", 17).next_u32();
    const std::uint32_t d18 = a.stream("ai.dynasty", 18).next_u32();
    CHECK(d17 != d18);
    CHECK(b.stream("ai.dynasty", 18).next_u32() == d18);
    CHECK(b.stream("ai.dynasty", 17).next_u32() == d17);
}

TEST_CASE("rng streams: snapshot round trip continues every sequence") {
    RngStreams original(4242);
    for (int i = 0; i < 20; ++i) {
        (void)original.stream("events").next_u32();
        (void)original.stream("ai.dynasty", 3).next_u32();
    }
    const RngSnapshot snapshot = original.save();
    REQUIRE(snapshot.streams.size() == 2);
    CHECK(snapshot.streams[0].key < snapshot.streams[1].key);  // sorted

    RngStreams restored(0);
    restored.restore(snapshot);
    CHECK(restored.master_seed() == 4242);
    for (int i = 0; i < 20; ++i) {
        CHECK(restored.stream("events").next_u32() == original.stream("events").next_u32());
        CHECK(restored.stream("ai.dynasty", 3).next_u32() == original.stream("ai.dynasty", 3).next_u32());
    }
    // A stream that did not exist at snapshot time is derived from the restored master seed.
    CHECK(restored.stream("new").next_u32() == original.stream("new").next_u32());
}
