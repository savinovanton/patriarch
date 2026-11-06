#pragma once

#include "pat/core/assert.hpp"
#include "pat/core/detail/sigmoid_table.hpp"
#include "pat/core/detail/wide_math.hpp"

#include <cmath>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <string>
#include <type_traits>

// Deterministic fixed-point numbers for the simulation (ADR-0001, NFR-DET-1).
//
//   Fixed32 = Q16.16  (int32 raw)  range about +-32768, resolution 1/65536
//   Fixed64 = Q32.32  (int64 raw)  range about +-2.1e9,  resolution 1/4294967296
//
// Rules:
//   * Multiplication and division TRUNCATE TOWARD ZERO (sign-symmetric, identical on all compilers).
//   * Overflow is a programming error: PAT_ASSERT fires in debug builds, the result wraps in
//     release builds. Use the saturating_* functions where clamping is the intended behaviour.
//   * to_double()/from_double() exist for tools, tests and UI only. Never use floating point in
//     simulation state or in calculations that influence it.

namespace pat::core {

template <class Raw, int Frac>
class Fixed {
    static_assert(std::is_signed_v<Raw> && std::is_integral_v<Raw>, "Raw must be a signed integer");
    static_assert(Frac > 4 && Frac < static_cast<int>(sizeof(Raw) * 8) - 1, "unsupported fraction width");

public:
    using raw_type = Raw;
    using unsigned_raw = std::make_unsigned_t<Raw>;
    static constexpr int frac_bits = Frac;
    static constexpr Raw one_raw = static_cast<Raw>(Raw{1} << Frac);

    constexpr Fixed() noexcept = default;

    [[nodiscard]] static constexpr Fixed from_raw(Raw raw) noexcept {
        Fixed f;
        f.raw_ = raw;
        return f;
    }

    [[nodiscard]] static constexpr Fixed from_int(std::int64_t value) noexcept {
        const std::int64_t limit = std::int64_t{1} << (static_cast<int>(sizeof(Raw) * 8) - 1 - Frac);
        PAT_ASSERT(value >= -limit && value < limit, "Fixed::from_int out of range: {}", value);
        return from_raw(static_cast<Raw>(static_cast<unsigned_raw>(value) << Frac));
    }

    /// num / den, truncated toward zero. den must not be zero.
    [[nodiscard]] static constexpr Fixed from_ratio(std::int64_t num, std::int64_t den) noexcept {
        PAT_ASSERT(den != 0, "Fixed::from_ratio division by zero");
        PAT_ASSERT(static_cast<std::int64_t>(static_cast<Raw>(num)) == num &&
                       static_cast<std::int64_t>(static_cast<Raw>(den)) == den,
                   "Fixed::from_ratio arguments exceed the raw type");
        bool overflow = false;
        const Raw raw = muldiv_raw(static_cast<Raw>(num), one_raw, static_cast<Raw>(den), overflow);
        PAT_ASSERT(!overflow, "Fixed::from_ratio overflow");
        return from_raw(raw);
    }

    /// For tools, tests and UI only (see header comment).
    [[nodiscard]] static Fixed from_double(double value) noexcept {
        return from_raw(static_cast<Raw>(std::llround(value * static_cast<double>(one_raw))));
    }

    [[nodiscard]] static constexpr Fixed max() noexcept { return from_raw(std::numeric_limits<Raw>::max()); }
    [[nodiscard]] static constexpr Fixed min() noexcept {
        return from_raw(std::numeric_limits<Raw>::lowest());
    }
    [[nodiscard]] static constexpr Fixed zero() noexcept { return from_raw(0); }
    [[nodiscard]] static constexpr Fixed one() noexcept { return from_raw(one_raw); }

    [[nodiscard]] constexpr Raw raw() const noexcept { return raw_; }

    /// Integer part, truncated toward zero.
    [[nodiscard]] constexpr Raw to_int() const noexcept { return static_cast<Raw>(raw_ / one_raw); }
    [[nodiscard]] constexpr Raw floor_int() const noexcept { return static_cast<Raw>(raw_ >> Frac); }
    /// Round half away from zero.
    [[nodiscard]] constexpr Raw round_int() const noexcept {
        const Raw half = static_cast<Raw>(one_raw / 2);
        return static_cast<Raw>((raw_ >= 0 ? raw_ + half : raw_ - half) / one_raw);
    }
    [[nodiscard]] double to_double() const noexcept {
        return static_cast<double>(raw_) / static_cast<double>(one_raw);
    }

    [[nodiscard]] constexpr bool is_negative() const noexcept { return raw_ < 0; }

    // ---- comparison -------------------------------------------------------------------------
    [[nodiscard]] friend constexpr auto operator<=>(const Fixed&, const Fixed&) noexcept = default;
    [[nodiscard]] friend constexpr bool operator==(const Fixed&, const Fixed&) noexcept = default;

    // ---- arithmetic -------------------------------------------------------------------------
    [[nodiscard]] friend constexpr Fixed operator+(Fixed a, Fixed b) noexcept {
        const Raw r = wrap_add(a.raw_, b.raw_);
        PAT_ASSERT(((a.raw_ ^ r) & (b.raw_ ^ r)) >= 0, "Fixed add overflow");
        return from_raw(r);
    }
    [[nodiscard]] friend constexpr Fixed operator-(Fixed a, Fixed b) noexcept {
        const Raw r = wrap_sub(a.raw_, b.raw_);
        PAT_ASSERT(((a.raw_ ^ b.raw_) & (a.raw_ ^ r)) >= 0, "Fixed sub overflow");
        return from_raw(r);
    }
    [[nodiscard]] friend constexpr Fixed operator-(Fixed a) noexcept {
        PAT_ASSERT(a.raw_ != std::numeric_limits<Raw>::lowest(), "Fixed negate overflow");
        return from_raw(wrap_sub(0, a.raw_));
    }
    [[nodiscard]] friend constexpr Fixed operator*(Fixed a, Fixed b) noexcept {
        bool overflow = false;
        const Raw r = mul_raw(a.raw_, b.raw_, overflow);
        PAT_ASSERT(!overflow, "Fixed multiply overflow");
        return from_raw(r);
    }
    [[nodiscard]] friend constexpr Fixed operator/(Fixed a, Fixed b) noexcept {
        PAT_ASSERT(b.raw_ != 0, "Fixed division by zero");
        bool overflow = false;
        const Raw r = div_raw(a.raw_, b.raw_, overflow);
        PAT_ASSERT(!overflow, "Fixed divide overflow");
        return from_raw(r);
    }
    /// Multiplication by a plain integer.
    [[nodiscard]] friend constexpr Fixed operator*(Fixed a, Raw n) noexcept { return a.mul_div(n, 1); }
    [[nodiscard]] friend constexpr Fixed operator*(Raw n, Fixed a) noexcept { return a.mul_div(n, 1); }
    /// Division by a plain integer.
    [[nodiscard]] friend constexpr Fixed operator/(Fixed a, Raw n) noexcept {
        PAT_ASSERT(n != 0, "Fixed division by zero");
        return a.mul_div(1, n);
    }

    constexpr Fixed& operator+=(Fixed o) noexcept { return *this = *this + o; }
    constexpr Fixed& operator-=(Fixed o) noexcept { return *this = *this - o; }
    constexpr Fixed& operator*=(Fixed o) noexcept { return *this = *this * o; }
    constexpr Fixed& operator/=(Fixed o) noexcept { return *this = *this / o; }

    /// this * num / den computed with a wide intermediate (no precision loss), truncated toward zero.
    [[nodiscard]] constexpr Fixed mul_div(Raw num, Raw den) const noexcept {
        PAT_ASSERT(den != 0, "Fixed::mul_div division by zero");
        bool overflow = false;
        const Raw r = muldiv_raw(raw_, num, den, overflow);
        PAT_ASSERT(!overflow, "Fixed::mul_div overflow");
        return from_raw(r);
    }

    /// this * permille / 1000 (e.g. 250 -> 25%).
    [[nodiscard]] constexpr Fixed mul_permille(Raw permille) const noexcept {
        return mul_div(permille, 1000);
    }

    // ---- saturating variants ----------------------------------------------------------------
    [[nodiscard]] friend constexpr Fixed saturating_add(Fixed a, Fixed b) noexcept {
        const Raw r = wrap_add(a.raw_, b.raw_);
        if (((a.raw_ ^ r) & (b.raw_ ^ r)) < 0) {
            return a.raw_ < 0 ? min() : max();
        }
        return from_raw(r);
    }
    [[nodiscard]] friend constexpr Fixed saturating_sub(Fixed a, Fixed b) noexcept {
        const Raw r = wrap_sub(a.raw_, b.raw_);
        if (((a.raw_ ^ b.raw_) & (a.raw_ ^ r)) < 0) {
            return a.raw_ < 0 ? min() : max();
        }
        return from_raw(r);
    }
    [[nodiscard]] friend constexpr Fixed saturating_mul(Fixed a, Fixed b) noexcept {
        bool overflow = false;
        const Raw r = mul_raw(a.raw_, b.raw_, overflow);
        if (overflow) {
            return ((a.raw_ < 0) != (b.raw_ < 0)) ? min() : max();
        }
        return from_raw(r);
    }

private:
    Raw raw_ = 0;

    [[nodiscard]] static constexpr Raw wrap_add(Raw a, Raw b) noexcept {
        return static_cast<Raw>(static_cast<unsigned_raw>(a) + static_cast<unsigned_raw>(b));
    }
    [[nodiscard]] static constexpr Raw wrap_sub(Raw a, Raw b) noexcept {
        return static_cast<Raw>(static_cast<unsigned_raw>(a) - static_cast<unsigned_raw>(b));
    }
    [[nodiscard]] static constexpr std::uint64_t abs_u64(Raw v) noexcept {
        return v < 0 ? (0ULL - static_cast<std::uint64_t>(static_cast<std::int64_t>(v)))
                     : static_cast<std::uint64_t>(v);
    }
    /// Converts a magnitude plus sign into Raw; sets overflow if it does not fit.
    [[nodiscard]] static constexpr Raw
    from_magnitude(std::uint64_t mag, bool negative, bool& overflow) noexcept {
        constexpr std::uint64_t kMax = static_cast<std::uint64_t>(std::numeric_limits<Raw>::max());
        if (negative) {
            if (mag > kMax + 1) {
                overflow = true;
                return 0;
            }
            return static_cast<Raw>(0 - mag);
        }
        if (mag > kMax) {
            overflow = true;
            return 0;
        }
        return static_cast<Raw>(mag);
    }

    [[nodiscard]] static constexpr Raw mul_raw(Raw a, Raw b, bool& overflow) noexcept {
        const bool negative = (a < 0) != (b < 0);
        const detail::U128 product = detail::mul_u64(abs_u64(a), abs_u64(b));
        const detail::U128 shifted = detail::shr(product, Frac);
        if (shifted.hi != 0) {
            overflow = true;
            return 0;
        }
        return from_magnitude(shifted.lo, negative, overflow);
    }

    [[nodiscard]] static constexpr Raw div_raw(Raw a, Raw b, bool& overflow) noexcept {
        const bool negative = (a < 0) != (b < 0);
        const detail::U128 numerator = detail::shl(detail::U128{0, abs_u64(a)}, Frac);
        const std::uint64_t q = detail::div_u128_by_u64(numerator, abs_u64(b), overflow);
        if (overflow) {
            return 0;
        }
        return from_magnitude(q, negative, overflow);
    }

    [[nodiscard]] static constexpr Raw muldiv_raw(Raw a, Raw b, Raw c, bool& overflow) noexcept {
        const bool negative = ((a < 0) != (b < 0)) != (c < 0);
        const detail::U128 product = detail::mul_u64(abs_u64(a), abs_u64(b));
        const std::uint64_t q = detail::div_u128_by_u64(product, abs_u64(c), overflow);
        if (overflow) {
            return 0;
        }
        return from_magnitude(q, negative, overflow);
    }
};

using Fixed32 = Fixed<std::int32_t, 16>;  // Q16.16
using Fixed64 = Fixed<std::int64_t, 32>;  // Q32.32

// ---- free functions ---------------------------------------------------------------------------

template <class Raw, int Frac>
[[nodiscard]] constexpr Fixed<Raw, Frac> abs(Fixed<Raw, Frac> v) noexcept {
    return v.is_negative() ? -v : v;
}

template <class Raw, int Frac>
[[nodiscard]] constexpr Fixed<Raw, Frac>
clamp(Fixed<Raw, Frac> v, Fixed<Raw, Frac> lo, Fixed<Raw, Frac> hi) noexcept {
    PAT_ASSERT(!(hi < lo), "clamp: hi < lo");
    return v < lo ? lo : (hi < v ? hi : v);
}

/// a + (b - a) * t, with t usually in [0, 1].
template <class Raw, int Frac>
[[nodiscard]] constexpr Fixed<Raw, Frac>
lerp(Fixed<Raw, Frac> a, Fixed<Raw, Frac> b, Fixed<Raw, Frac> t) noexcept {
    return a + (b - a) * t;
}

/// floor(sqrt(v)) at the resolution of the type. v must be non-negative.
template <class Raw, int Frac>
[[nodiscard]] constexpr Fixed<Raw, Frac> sqrt(Fixed<Raw, Frac> v) noexcept {
    PAT_ASSERT(!v.is_negative(), "sqrt of a negative number");
    const detail::U128 n = detail::shl(detail::U128{0, static_cast<std::uint64_t>(v.raw())}, Frac);
    return Fixed<Raw, Frac>::from_raw(static_cast<Raw>(detail::isqrt_u128(n)));
}

/// Logistic function 1 / (1 + e^-x) from a lookup table with linear interpolation.
/// Input is clamped to [-8, 8]; output is in [0, 1].
template <class Raw, int Frac>
[[nodiscard]] constexpr Fixed<Raw, Frac> sigmoid(Fixed<Raw, Frac> x) noexcept {
    constexpr int kSteps = detail::kSigmoidTableSteps;  // points per unit
    constexpr int kRange = detail::kSigmoidTableRange;
    constexpr int kStepShift = Frac - 4;  // 1 / kSteps == 2^-4 in raw units
    static_assert(kSteps == 16, "table step is 1/16");

    const std::int64_t lo = -(static_cast<std::int64_t>(kRange) << Frac);
    const std::int64_t hi = static_cast<std::int64_t>(kRange) << Frac;
    std::int64_t raw = static_cast<std::int64_t>(x.raw());
    raw = raw < lo ? lo : (raw > hi ? hi : raw);

    const std::uint64_t shifted = static_cast<std::uint64_t>(raw - lo);  // 0 .. 16 << Frac
    const std::size_t index = static_cast<std::size_t>(shifted >> kStepShift);
    const std::int64_t rem = static_cast<std::int64_t>(shifted & ((std::uint64_t{1} << kStepShift) - 1));

    const std::int64_t a = detail::kSigmoidTable[index];
    const std::int64_t b =
        detail::kSigmoidTable[index < detail::kSigmoidTable.size() - 1 ? index + 1 : index];
    const std::int64_t value_q16 = a + (((b - a) * rem) >> kStepShift);  // Q16.16

    if constexpr (Frac >= 16) {
        return Fixed<Raw, Frac>::from_raw(static_cast<Raw>(value_q16 << (Frac - 16)));
    } else {
        return Fixed<Raw, Frac>::from_raw(static_cast<Raw>(value_q16 >> (16 - Frac)));
    }
}

/// Decimal text without floating point, e.g. "-12.3750" (truncated, not rounded).
template <class Raw, int Frac>
[[nodiscard]] std::string to_string(Fixed<Raw, Frac> v, int decimals = 4) {
    const bool negative = v.is_negative();
    const std::uint64_t mag = negative
                                  ? (0ULL - static_cast<std::uint64_t>(static_cast<std::int64_t>(v.raw())))
                                  : static_cast<std::uint64_t>(v.raw());
    const std::uint64_t mask = (std::uint64_t{1} << Frac) - 1;

    std::string out = negative ? "-" : "";
    out += std::to_string(mag >> Frac);
    if (decimals > 0) {
        out += '.';
        std::uint64_t frac = mag & mask;
        for (int i = 0; i < decimals; ++i) {
            frac *= 10;
            out += static_cast<char>('0' + static_cast<int>(frac >> Frac));
            frac &= mask;
        }
    }
    return out;
}

}  // namespace pat::core

template <class Raw, int Frac>
struct std::formatter<pat::core::Fixed<Raw, Frac>> : std::formatter<std::string> {
    auto format(pat::core::Fixed<Raw, Frac> v, auto& ctx) const {
        return std::formatter<std::string>::format(pat::core::to_string(v), ctx);
    }
};
