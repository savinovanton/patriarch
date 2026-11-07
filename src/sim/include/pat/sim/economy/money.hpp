#pragma once

#include "pat/core/assert.hpp"
#include "pat/core/detail/wide_math.hpp"
#include "pat/core/fixed.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>

// Money of the economy (E9-T04, docs/dev/economy-model.md sections 3 and 5, GDD 03 section 7.3).
//
// Money is a plain `int64` count of KOPECKS (1 rouble = 100 kopecks). There is no floating point
// and no Fixed type for money. Everything here is a pure constexpr function:
//
//   * checked_*      - exact result, or std::nullopt when it does not fit in int64;
//   * saturating_*   - the result clamps to the int64 range instead;
//   * mul_permille   - amount x permille / 1000, truncated toward zero, saturating;
//   * split_*        - "split by subtraction": the parts always add up to the whole, so no kopeck
//                      is lost or created by rounding (economy-model section 3);
//   * apply_posting  - how a balance reacts to a posting; World::post_money (E9-T02) uses it so the
//                      account and the ledger line always change by the same applied amount.
//
// Formatting for display (roubles, kopecks, thousands separators) is NOT here: it belongs to the
// presentation layer (src/game).

namespace pat::sim::economy {

using Kopecks = std::int64_t;

inline constexpr Kopecks kKopecksPerRouble = 100;
/// Largest absolute account balance (10^15 kopecks = 10^13 roubles).
inline constexpr Kopecks kMaxTreasuryKop = 1'000'000'000'000'000;
/// Largest absolute amount of a single posting (10^13 kopecks).
inline constexpr Kopecks kMaxPostingKop = 10'000'000'000'000;
/// Per-mille value of "x1.0".
inline constexpr std::int32_t kPermilleOne = 1000;

namespace detail {

[[nodiscard]] constexpr std::uint64_t magnitude(Kopecks v) noexcept {
    return v < 0 ? (0ULL - static_cast<std::uint64_t>(v)) : static_cast<std::uint64_t>(v);
}

/// Sign + magnitude -> int64; nullopt when it does not fit.
[[nodiscard]] constexpr std::optional<Kopecks> from_magnitude(std::uint64_t mag, bool negative) noexcept {
    constexpr auto kMax = static_cast<std::uint64_t>(std::numeric_limits<Kopecks>::max());
    if (negative) {
        if (mag > kMax + 1) {
            return std::nullopt;
        }
        return static_cast<Kopecks>(0ULL - mag);
    }
    if (mag > kMax) {
        return std::nullopt;
    }
    return static_cast<Kopecks>(mag);
}

/// floor(a * b / d) for unsigned operands; nullopt when the quotient exceeds 64 bits or d == 0.
[[nodiscard]] constexpr std::optional<std::uint64_t>
mul_div_u64(std::uint64_t a, std::uint64_t b, std::uint64_t d) noexcept {
    bool overflow = false;
    const std::uint64_t q = core::detail::div_u128_by_u64(core::detail::mul_u64(a, b), d, overflow);
    if (overflow) {
        return std::nullopt;
    }
    return q;
}

}  // namespace detail

// ---- checked and saturating arithmetic ---------------------------------------------------------

[[nodiscard]] constexpr std::optional<Kopecks> checked_add(Kopecks a, Kopecks b) noexcept {
    const auto r = static_cast<Kopecks>(static_cast<std::uint64_t>(a) + static_cast<std::uint64_t>(b));
    if (((a ^ r) & (b ^ r)) < 0) {
        return std::nullopt;
    }
    return r;
}

[[nodiscard]] constexpr std::optional<Kopecks> checked_sub(Kopecks a, Kopecks b) noexcept {
    const auto r = static_cast<Kopecks>(static_cast<std::uint64_t>(a) - static_cast<std::uint64_t>(b));
    if (((a ^ b) & (a ^ r)) < 0) {
        return std::nullopt;
    }
    return r;
}

[[nodiscard]] constexpr std::optional<Kopecks> checked_neg(Kopecks a) noexcept {
    return checked_sub(0, a);
}

[[nodiscard]] constexpr std::optional<Kopecks> checked_mul(Kopecks a, Kopecks b) noexcept {
    const core::detail::U128 product = core::detail::mul_u64(detail::magnitude(a), detail::magnitude(b));
    if (product.hi != 0) {
        return std::nullopt;
    }
    return detail::from_magnitude(product.lo, (a < 0) != (b < 0));
}

[[nodiscard]] constexpr Kopecks saturating_add(Kopecks a, Kopecks b) noexcept {
    if (const auto r = checked_add(a, b)) {
        return *r;
    }
    return a < 0 ? std::numeric_limits<Kopecks>::min() : std::numeric_limits<Kopecks>::max();
}

[[nodiscard]] constexpr Kopecks saturating_sub(Kopecks a, Kopecks b) noexcept {
    if (const auto r = checked_sub(a, b)) {
        return *r;
    }
    return a < 0 ? std::numeric_limits<Kopecks>::min() : std::numeric_limits<Kopecks>::max();
}

[[nodiscard]] constexpr Kopecks saturating_mul(Kopecks a, Kopecks b) noexcept {
    if (const auto r = checked_mul(a, b)) {
        return *r;
    }
    return ((a < 0) != (b < 0)) ? std::numeric_limits<Kopecks>::min() : std::numeric_limits<Kopecks>::max();
}

/// amount x num / den with a 128-bit intermediate, truncated toward zero. `den` must be positive;
/// nullopt when `den <= 0` or the result does not fit in int64.
[[nodiscard]] constexpr std::optional<Kopecks>
checked_mul_div(Kopecks amount, Kopecks num, Kopecks den) noexcept {
    if (den <= 0) {
        return std::nullopt;
    }
    const auto q = detail::mul_div_u64(
        detail::magnitude(amount), detail::magnitude(num), static_cast<std::uint64_t>(den));
    if (!q) {
        return std::nullopt;
    }
    return detail::from_magnitude(*q, (amount < 0) != (num < 0));
}

/// amount x permille / 1000 truncated toward zero (so -1 x 500 / 1000 == 0, not -1). Exact for every
/// int32 multiplier, positive or negative, equal to the textbook form
/// `(a / 1000) * p + ((a % 1000) * p) / 1000` but without its intermediate overflow. nullopt when
/// the result does not fit in int64.
[[nodiscard]] constexpr std::optional<Kopecks> checked_mul_permille(Kopecks amount,
                                                                    std::int32_t permille) noexcept {
    return checked_mul_div(amount, permille, kPermilleOne);
}

/// As checked_mul_permille, but an overflowing result saturates to the int64 limits; `saturated`
/// (optional) is set to true in that case so a caller can count the error.
[[nodiscard]] constexpr Kopecks
mul_permille(Kopecks amount, std::int32_t permille, bool* saturated = nullptr) noexcept {
    if (const auto r = checked_mul_permille(amount, permille)) {
        return *r;
    }
    if (saturated != nullptr) {
        *saturated = true;
    }
    return ((amount < 0) != (permille < 0)) ? std::numeric_limits<Kopecks>::min()
                                            : std::numeric_limits<Kopecks>::max();
}

/// A money value held in a Fixed64 (Q32.32, in kopecks) -> whole kopecks, truncated toward zero.
[[nodiscard]] constexpr Kopecks to_kopecks(core::Fixed64 value) noexcept {
    return value.to_int();
}

/// Clamps a balance to +-kMaxTreasuryKop.
[[nodiscard]] constexpr Kopecks clamp_treasury(Kopecks balance) noexcept {
    return balance < -kMaxTreasuryKop ? -kMaxTreasuryKop
                                      : (balance > kMaxTreasuryKop ? kMaxTreasuryKop : balance);
}

// ---- postings ----------------------------------------------------------------------------------

/// Outcome of applying a posting to a balance.
struct PostingResult {
    Kopecks new_balance = 0;
    /// The amount that was really applied: new_balance - old balance. Equals the requested amount
    /// unless the balance hit +-kMaxTreasuryKop. The ledger line must change by `applied`, never
    /// by the requested amount, so that `balance == opening + sum(lines)` always holds.
    Kopecks applied = 0;
    bool saturated = false;

    [[nodiscard]] bool operator==(const PostingResult&) const = default;
};

/// Applies `amount` to `balance` (|balance| <= kMaxTreasuryKop), saturating at +-kMaxTreasuryKop.
[[nodiscard]] constexpr PostingResult apply_posting(Kopecks balance, Kopecks amount) noexcept {
    PAT_ASSERT(balance >= -kMaxTreasuryKop && balance <= kMaxTreasuryKop,
               "balance outside the treasury range");
    const Kopecks target = clamp_treasury(saturating_add(balance, amount));
    return PostingResult{target, target - balance, target - balance != amount};
}

// ---- splitting without losing a kopeck ---------------------------------------------------------

/// A whole split in two: `part` and `rest`, with part + rest == the original amount.
struct Split {
    Kopecks part = 0;
    Kopecks rest = 0;

    [[nodiscard]] bool operator==(const Split&) const = default;
};

/// "Split by subtraction": part = amount x permille / 1000 (truncated toward zero), rest = amount -
/// part. The remainder always goes to `rest`, so the two add up to `amount` exactly. This is how a
/// tax is split into leak and net, or into the crown's share and the owner's. Saturates only for
/// |amount| beyond what mul_permille can express, in which case part is clamped and rest follows.
[[nodiscard]] constexpr Split split_permille(Kopecks amount, std::int32_t permille) noexcept {
    const Kopecks part = mul_permille(amount, permille);
    return Split{part, saturating_sub(amount, part)};
}

/// Distributes `amount` over `weights` into `out` (same length) so that sum(out) == amount exactly.
/// Part i is the share of what is left: remaining x w[i] / remaining_weight (truncated toward zero),
/// then the remaining amount and weight shrink by that part and w[i]. The remainder therefore
/// always moves on to later entries; the last non-zero weight takes whatever is left. Entries with
/// weight 0 receive 0. A negative amount distributes its magnitude and negates every part.
///
/// Returns false (and writes zeros) when `weights` is empty or all weights are zero - there is
/// nobody to give the money to, and the caller decides what to do.
[[nodiscard]] constexpr bool
split_by_weights(Kopecks amount, std::span<const std::uint32_t> weights, std::span<Kopecks> out) noexcept {
    PAT_ASSERT(weights.size() == out.size(), "split_by_weights: size mismatch");
    std::uint64_t remaining_weight = 0;
    for (const std::uint32_t w : weights) {
        remaining_weight += w;
    }
    for (Kopecks& slot : out) {
        slot = 0;
    }
    if (remaining_weight == 0) {
        return false;
    }
    const bool negative = amount < 0;
    std::uint64_t remaining = detail::magnitude(amount);
    for (std::size_t i = 0; i < weights.size(); ++i) {
        const std::uint64_t w = weights[i];
        if (w == 0) {
            continue;
        }
        // remaining * w / remaining_weight <= remaining (w <= remaining_weight), so it fits.
        const std::uint64_t part = detail::mul_div_u64(remaining, w, remaining_weight).value_or(remaining);
        out[i] = detail::from_magnitude(part, negative).value_or(0);
        remaining -= part;
        remaining_weight -= w;
    }
    return true;
}

}  // namespace pat::sim::economy
