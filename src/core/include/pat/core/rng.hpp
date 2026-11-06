#pragma once

#include "pat/core/assert.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

// Deterministic random numbers for the simulation (ADR-0001, NFR-DET-1).
//
//   Pcg32       PCG-XSH-RR 64/32 generator (O'Neill), reference-compatible: Pcg32(42, 54) produces
//               0xa15c02b7, 0x7b47f409, 0xba1d3330, ... exactly like pcg32_srandom_r(42, 54).
//   RngStreams  named, independent generators ("events", "ai.dynasty" + index, ...). A stream's
//               sequence depends only on (master seed, stream name), never on creation order.
//
// Never use std::rand, std::random_device or std::mt19937 in simulation code.

namespace pat::core {

/// SplitMix64 step: advances `state` and returns a well-mixed 64-bit value. Used for seeding.
[[nodiscard]] constexpr std::uint64_t split_mix64_next(std::uint64_t& state) noexcept {
    state += 0x9E3779B97F4A7C15ULL;
    std::uint64_t z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

/// 64-bit FNV-1a hash of raw bytes; platform independent (unlike std::hash).
[[nodiscard]] constexpr std::uint64_t fnv1a64(std::string_view bytes) noexcept {
    std::uint64_t hash = 0xCBF29CE484222325ULL;
    for (const char c : bytes) {
        hash ^= static_cast<std::uint8_t>(c);
        hash *= 0x100000001B3ULL;
    }
    return hash;
}

class Pcg32 {
public:
    /// Reference seeding (pcg32_srandom_r): `stream` selects one of 2^63 independent sequences.
    constexpr Pcg32(std::uint64_t seed, std::uint64_t stream) noexcept {
        inc_ = (stream << 1U) | 1U;
        (void)next_u32();
        state_ += seed;
        (void)next_u32();
    }

    /// Restores a generator from values previously obtained with state() / inc().
    [[nodiscard]] static constexpr Pcg32 from_state(std::uint64_t state, std::uint64_t inc) noexcept {
        Pcg32 g(0, 0);
        g.state_ = state;
        g.inc_ = inc | 1U;
        return g;
    }

    [[nodiscard]] constexpr std::uint64_t state() const noexcept { return state_; }
    [[nodiscard]] constexpr std::uint64_t inc() const noexcept { return inc_; }

    [[nodiscard]] constexpr std::uint32_t next_u32() noexcept {
        const std::uint64_t old = state_;
        state_ = old * kMultiplier + inc_;
        const auto xorshifted = static_cast<std::uint32_t>(((old >> 18U) ^ old) >> 27U);
        const auto rot = static_cast<std::uint32_t>(old >> 59U);
        return (xorshifted >> rot) | (xorshifted << ((~rot + 1U) & 31U));
    }

    /// Two draws: high word first.
    [[nodiscard]] constexpr std::uint64_t next_u64() noexcept {
        const std::uint64_t hi = next_u32();
        const std::uint64_t lo = next_u32();
        return (hi << 32U) | lo;
    }

    /// Uniform in [0, bound), without modulo bias. bound must be > 0.
    [[nodiscard]] constexpr std::uint32_t bounded(std::uint32_t bound) noexcept {
        PAT_ASSERT(bound > 0, "Pcg32::bounded(0)");
        if (bound == 0) {
            return 0;
        }
        const std::uint32_t threshold = (0U - bound) % bound;
        for (;;) {
            const std::uint32_t r = next_u32();
            if (r >= threshold) {
                return r % bound;
            }
        }
    }

    /// Uniform integer in the closed interval [lo, hi].
    [[nodiscard]] constexpr std::int32_t range(std::int32_t lo, std::int32_t hi) noexcept {
        PAT_ASSERT(lo <= hi, "Pcg32::range lo > hi");
        const auto span = static_cast<std::uint32_t>(static_cast<std::int64_t>(hi) - lo + 1);
        // span == 0 means the full 2^32 range
        const std::uint32_t offset = span == 0 ? next_u32() : bounded(span);
        return static_cast<std::int32_t>(static_cast<std::int64_t>(lo) + offset);
    }

    /// True with probability permille / 1000 (0 never, >= 1000 always).
    [[nodiscard]] constexpr bool chance_permille(std::int32_t permille) noexcept {
        if (permille <= 0) {
            return false;
        }
        if (permille >= 1000) {
            return true;
        }
        return bounded(1000) < static_cast<std::uint32_t>(permille);
    }

    /// Fisher-Yates shuffle; the result depends only on the generator state.
    template <class T>
    constexpr void shuffle(std::span<T> items) noexcept {
        for (std::size_t i = items.size(); i > 1; --i) {
            const auto j = static_cast<std::size_t>(bounded(static_cast<std::uint32_t>(i)));
            std::swap(items[i - 1], items[j]);
        }
    }

    /// Jumps `delta` draws ahead in O(log delta). Pass (0 - n) as uint64 to step back n draws.
    constexpr void advance(std::uint64_t delta) noexcept {
        std::uint64_t cur_mult = kMultiplier;
        std::uint64_t cur_plus = inc_;
        std::uint64_t acc_mult = 1;
        std::uint64_t acc_plus = 0;
        while (delta > 0) {
            if ((delta & 1U) != 0) {
                acc_mult *= cur_mult;
                acc_plus = acc_plus * cur_mult + cur_plus;
            }
            cur_plus = (cur_mult + 1U) * cur_plus;
            cur_mult *= cur_mult;
            delta >>= 1U;
        }
        state_ = acc_mult * state_ + acc_plus;
    }

    [[nodiscard]] friend constexpr bool operator==(const Pcg32&, const Pcg32&) noexcept = default;

private:
    static constexpr std::uint64_t kMultiplier = 6364136223846793005ULL;

    std::uint64_t state_ = 0;
    std::uint64_t inc_ = 1;
};

struct RngStreamState {
    std::uint64_t key = 0;
    std::uint64_t state = 0;
    std::uint64_t inc = 0;

    [[nodiscard]] bool operator==(const RngStreamState&) const = default;
};

struct RngSnapshot {
    std::uint64_t master_seed = 0;
    std::vector<RngStreamState> streams;  // sorted by key

    [[nodiscard]] bool operator==(const RngSnapshot&) const = default;
};

/// Registry of named independent generators derived from one master seed.
class RngStreams {
public:
    explicit RngStreams(std::uint64_t master_seed) noexcept : master_seed_(master_seed) {}

    [[nodiscard]] std::uint64_t master_seed() const noexcept { return master_seed_; }

    /// Returns the stream called `name`, creating it on first use. References stay valid for the
    /// lifetime of the registry (map nodes are stable).
    [[nodiscard]] Pcg32& stream(std::string_view name);

    /// Stream for an indexed family, e.g. stream("ai.dynasty", dynasty_id). Same sequence as any
    /// other call with the same (prefix, index), independent of creation order.
    [[nodiscard]] Pcg32& stream(std::string_view prefix, std::uint64_t index);

    [[nodiscard]] RngSnapshot save() const;
    /// Replaces all streams with the ones in the snapshot (and the master seed).
    void restore(const RngSnapshot& snapshot);

    [[nodiscard]] std::size_t size() const noexcept { return streams_.size(); }

private:
    [[nodiscard]] Pcg32& stream_for_key(std::uint64_t key);

    std::uint64_t master_seed_;
    std::map<std::uint64_t, Pcg32> streams_;  // ordered: deterministic iteration
};

}  // namespace pat::core
