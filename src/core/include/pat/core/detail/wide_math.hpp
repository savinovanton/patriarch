#pragma once

#include <cstdint>

// Portable 128-bit helpers used by fixed-point arithmetic. No compiler intrinsics and no
// __int128 so results are identical on every toolchain (determinism, ADR-0001).

namespace pat::core::detail {

struct U128 {
    std::uint64_t hi = 0;
    std::uint64_t lo = 0;
};

[[nodiscard]] constexpr bool is_zero(U128 v) noexcept {
    return v.hi == 0 && v.lo == 0;
}

[[nodiscard]] constexpr bool less(U128 a, U128 b) noexcept {
    return a.hi < b.hi || (a.hi == b.hi && a.lo < b.lo);
}

[[nodiscard]] constexpr U128 add(U128 a, U128 b) noexcept {
    U128 r;
    r.lo = a.lo + b.lo;
    r.hi = a.hi + b.hi + (r.lo < a.lo ? 1U : 0U);
    return r;
}

/// a - b, requires a >= b.
[[nodiscard]] constexpr U128 sub(U128 a, U128 b) noexcept {
    U128 r;
    r.lo = a.lo - b.lo;
    r.hi = a.hi - b.hi - (a.lo < b.lo ? 1U : 0U);
    return r;
}

/// Logical shift right, shift in [0, 127].
[[nodiscard]] constexpr U128 shr(U128 v, int shift) noexcept {
    if (shift == 0) {
        return v;
    }
    if (shift < 64) {
        return U128{v.hi >> shift, (v.lo >> shift) | (v.hi << (64 - shift))};
    }
    return U128{0, v.hi >> (shift - 64)};
}

/// Logical shift left, shift in [0, 127]; bits shifted out are lost.
[[nodiscard]] constexpr U128 shl(U128 v, int shift) noexcept {
    if (shift == 0) {
        return v;
    }
    if (shift < 64) {
        return U128{(v.hi << shift) | (v.lo >> (64 - shift)), v.lo << shift};
    }
    return U128{v.lo << (shift - 64), 0};
}

/// Full 64x64 -> 128-bit unsigned product.
[[nodiscard]] constexpr U128 mul_u64(std::uint64_t a, std::uint64_t b) noexcept {
    constexpr std::uint64_t kMask = 0xFFFFFFFFULL;
    const std::uint64_t a_lo = a & kMask;
    const std::uint64_t a_hi = a >> 32;
    const std::uint64_t b_lo = b & kMask;
    const std::uint64_t b_hi = b >> 32;

    const std::uint64_t p0 = a_lo * b_lo;
    const std::uint64_t p1 = a_lo * b_hi;
    const std::uint64_t p2 = a_hi * b_lo;
    const std::uint64_t p3 = a_hi * b_hi;

    const std::uint64_t mid = (p0 >> 32) + (p1 & kMask) + (p2 & kMask);
    U128 r;
    r.lo = (p0 & kMask) | (mid << 32);
    r.hi = p3 + (p1 >> 32) + (p2 >> 32) + (mid >> 32);
    return r;
}

/// floor(n / d). Sets overflow (and returns 0) when the quotient does not fit in 64 bits
/// (n.hi >= d) or d == 0.
[[nodiscard]] constexpr std::uint64_t div_u128_by_u64(U128 n, std::uint64_t d, bool& overflow) noexcept {
    if (d == 0 || n.hi >= d) {
        overflow = true;
        return 0;
    }
    std::uint64_t remainder = n.hi;
    std::uint64_t quotient = 0;
    for (int i = 63; i >= 0; --i) {
        const bool carry = (remainder >> 63) != 0;
        remainder = (remainder << 1) | ((n.lo >> i) & 1U);
        quotient <<= 1;
        if (carry || remainder >= d) {
            remainder -= d;
            quotient |= 1U;
        }
    }
    return quotient;
}

/// floor(sqrt(n)) for any 128-bit n (result always fits in 64 bits).
[[nodiscard]] constexpr std::uint64_t isqrt_u128(U128 n) noexcept {
    U128 result{0, 0};
    U128 bit{1ULL << 62, 0};  // 2^126
    while (!is_zero(bit) && less(n, bit)) {
        bit = shr(bit, 2);
    }
    while (!is_zero(bit)) {
        const U128 trial = add(result, bit);
        if (!less(n, trial)) {
            n = sub(n, trial);
            result = add(shr(result, 1), bit);
        } else {
            result = shr(result, 1);
        }
        bit = shr(bit, 2);
    }
    return result.lo;
}

}  // namespace pat::core::detail
