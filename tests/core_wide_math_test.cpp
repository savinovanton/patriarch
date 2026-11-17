#include "pat/core/detail/wide_math.hpp"
#include "pat/core/fixed.hpp"
#include "pat/sim/economy/money.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <limits>
#include <optional>

// Table-driven tests of the shared signed-arithmetic primitives (docs/dev/code-duplication-audit.md D9):
// the boundaries of int64 and int32, +-1 and 0, compared with an independent formulation
// (range comparison instead of sign-bit tricks).

namespace {

namespace wm = pat::core::detail;
namespace econ = pat::sim::economy;

constexpr std::int64_t kMin = std::numeric_limits<std::int64_t>::min();
constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();

constexpr std::array<std::int64_t, 17> kSamples{kMin,
                                                kMin + 1,
                                                kMin / 2 - 1,
                                                kMin / 2,
                                                -4'294'967'296LL,
                                                -1'000'000'007LL,
                                                -2,
                                                -1,
                                                0,
                                                1,
                                                2,
                                                1'000'000'007LL,
                                                4'294'967'296LL,
                                                kMax / 2,
                                                kMax / 2 + 1,
                                                kMax - 1,
                                                kMax};

// a + b fits in int64?  (a > 0: b <= kMax - a;  a < 0: b >= kMin - a)
bool add_fits(std::int64_t a, std::int64_t b) {
    if (a > 0) {
        return b <= kMax - a;
    }
    if (a < 0) {
        return b >= kMin - a;
    }
    return true;
}

// a - b fits in int64?
bool sub_fits(std::int64_t a, std::int64_t b) {
    if (b > 0) {
        return a >= kMin + b;
    }
    if (b < 0) {
        return a <= kMax + b;
    }
    return true;
}

}  // namespace

TEST_CASE("wide_math: abs_u64 at the boundaries") {
    CHECK(wm::abs_u64(0) == 0);
    CHECK(wm::abs_u64(1) == 1);
    CHECK(wm::abs_u64(-1) == 1);
    CHECK(wm::abs_u64(kMax) == static_cast<std::uint64_t>(kMax));
    CHECK(wm::abs_u64(kMin) == 9'223'372'036'854'775'808ULL);
    CHECK(wm::abs_u64(kMin + 1) == static_cast<std::uint64_t>(kMax));
    CHECK(wm::abs_u64(std::numeric_limits<std::int32_t>::min()) == 2'147'483'648ULL);
}

TEST_CASE("wide_math: wrap and overflow flags agree with the range formulation") {
    for (const std::int64_t a : kSamples) {
        for (const std::int64_t b : kSamples) {
            INFO("a=" << a << " b=" << b);
            const std::int64_t sum = wm::wrap_add(a, b);
            CHECK(wm::add_overflowed(a, b, sum) == !add_fits(a, b));
            if (add_fits(a, b)) {
                CHECK(sum - b == a);
            }
            const std::int64_t diff = wm::wrap_sub(a, b);
            CHECK(wm::sub_overflowed(a, b, diff) == !sub_fits(a, b));
            if (sub_fits(a, b)) {
                CHECK(diff + b == a);
            }
        }
    }
}

TEST_CASE("wide_math: wrap_add and wrap_sub wrap at the narrow width too") {
    constexpr std::int32_t kMax32 = std::numeric_limits<std::int32_t>::max();
    constexpr std::int32_t kMin32 = std::numeric_limits<std::int32_t>::min();
    CHECK(wm::wrap_add<std::int32_t>(kMax32, 1) == kMin32);
    CHECK(wm::wrap_sub<std::int32_t>(kMin32, 1) == kMax32);
    CHECK(wm::wrap_sub<std::int32_t>(0, kMin32) == kMin32);  // negating the lowest value wraps
    CHECK(wm::add_overflowed<std::int32_t>(kMax32, 1, wm::wrap_add<std::int32_t>(kMax32, 1)));
    CHECK_FALSE(wm::add_overflowed<std::int32_t>(kMax32, -1, wm::wrap_add<std::int32_t>(kMax32, -1)));
    CHECK(wm::sub_overflowed<std::int32_t>(kMin32, 1, wm::wrap_sub<std::int32_t>(kMin32, 1)));
}

TEST_CASE("wide_math: from_magnitude boundaries for int64 and int32") {
    CHECK(wm::from_magnitude<std::int64_t>(0, false) == 0);
    CHECK(wm::from_magnitude<std::int64_t>(0, true) == 0);
    CHECK(wm::from_magnitude<std::int64_t>(5, true) == -5);
    CHECK(wm::from_magnitude<std::int64_t>(static_cast<std::uint64_t>(kMax), false) == kMax);
    CHECK_FALSE(wm::from_magnitude<std::int64_t>(static_cast<std::uint64_t>(kMax) + 1, false).has_value());
    CHECK(wm::from_magnitude<std::int64_t>(static_cast<std::uint64_t>(kMax) + 1, true) == kMin);
    CHECK_FALSE(wm::from_magnitude<std::int64_t>(static_cast<std::uint64_t>(kMax) + 2, true).has_value());
    CHECK_FALSE(
        wm::from_magnitude<std::int64_t>(std::numeric_limits<std::uint64_t>::max(), true).has_value());

    CHECK(wm::from_magnitude<std::int32_t>(2'147'483'647ULL, false) == 2'147'483'647);
    CHECK_FALSE(wm::from_magnitude<std::int32_t>(2'147'483'648ULL, false).has_value());
    CHECK(wm::from_magnitude<std::int32_t>(2'147'483'648ULL, true) ==
          std::numeric_limits<std::int32_t>::min());
    CHECK_FALSE(wm::from_magnitude<std::int32_t>(2'147'483'649ULL, true).has_value());
}

TEST_CASE("money: checked and saturating add sub mul at the boundaries") {
    for (const std::int64_t a : kSamples) {
        for (const std::int64_t b : kSamples) {
            INFO("a=" << a << " b=" << b);
            const auto sum = econ::checked_add(a, b);
            CHECK(sum.has_value() == add_fits(a, b));
            const std::int64_t expected_sum = add_fits(a, b) ? wm::wrap_add(a, b) : (a < 0 ? kMin : kMax);
            CHECK(econ::saturating_add(a, b) == expected_sum);

            const auto diff = econ::checked_sub(a, b);
            CHECK(diff.has_value() == sub_fits(a, b));
            const std::int64_t expected_diff = sub_fits(a, b) ? wm::wrap_sub(a, b) : (a < 0 ? kMin : kMax);
            CHECK(econ::saturating_sub(a, b) == expected_diff);
        }
    }

    CHECK(econ::checked_mul(0, kMin) == 0);
    CHECK(econ::checked_mul(1, kMin) == kMin);
    CHECK(econ::checked_mul(-1, kMax) == -kMax);
    CHECK_FALSE(econ::checked_mul(-1, kMin).has_value());  // 2^63 does not fit
    CHECK_FALSE(econ::checked_mul(kMax, 2).has_value());
    CHECK_FALSE(econ::checked_mul(kMin, kMin).has_value());
    CHECK(econ::checked_mul(3'037'000'499LL, 3'037'000'499LL) == 9'223'372'030'926'249'001LL);
    CHECK_FALSE(econ::checked_mul(3'037'000'500LL, 3'037'000'500LL).has_value());
    CHECK(econ::saturating_mul(kMax, 2) == kMax);
    CHECK(econ::saturating_mul(kMax, -2) == kMin);
    CHECK(econ::saturating_mul(kMin, kMin) == kMax);
    CHECK(econ::saturating_mul(-1, kMin) == kMax);
    CHECK(econ::checked_neg(kMin) == std::nullopt);
    CHECK(econ::checked_neg(kMax) == -kMax);
    CHECK(econ::checked_neg(0) == 0);
}

TEST_CASE("money: checked_mul_div and permille at the boundaries") {
    CHECK(econ::checked_mul_div(kMax, kMax, kMax) == kMax);
    CHECK(econ::checked_mul_div(kMin, 1, 1) == kMin);
    CHECK(econ::checked_mul_div(kMin, -1, 1) == std::nullopt);  // 2^63
    CHECK(econ::checked_mul_div(kMin, 1, 2) == kMin / 2);
    CHECK(econ::checked_mul_div(-1, 500, 1000) == 0);  // truncated toward zero
    CHECK(econ::checked_mul_div(10, 10, 0) == std::nullopt);
    CHECK(econ::checked_mul_div(10, 10, -3) == std::nullopt);
    CHECK(econ::checked_mul_permille(kMax, 1000) == kMax);
    CHECK(econ::checked_mul_permille(kMax, 1001) == std::nullopt);
    CHECK(econ::checked_mul_permille(kMin, 1000) == kMin);
    CHECK(econ::checked_mul_permille(kMin, -1000) == std::nullopt);
    bool saturated = false;
    CHECK(econ::mul_permille(kMax, 2000, &saturated) == kMax);
    CHECK(saturated);
    saturated = false;
    CHECK(econ::mul_permille(kMax, -2000, &saturated) == kMin);
    CHECK(saturated);
    saturated = false;
    CHECK(econ::mul_permille(1'000, 250, &saturated) == 250);
    CHECK_FALSE(saturated);
}

TEST_CASE_TEMPLATE("fixed: saturating operations at the limits", F, pat::core::Fixed32, pat::core::Fixed64) {
    const F hi = F::max();
    const F lo = F::min();
    const F one = F::from_int(1);
    const F zero = F::zero();
    CHECK(saturating_add(hi, one) == hi);
    CHECK(saturating_add(lo, -one) == lo);
    CHECK(saturating_add(hi, lo) == F::from_raw(-1));
    CHECK(saturating_sub(lo, one) == lo);
    CHECK(saturating_sub(hi, -one) == hi);
    CHECK(saturating_sub(zero, lo) == hi);  // -lowest saturates
    CHECK(saturating_sub(hi, hi) == zero);
    CHECK(saturating_mul(hi, hi) == hi);
    CHECK(saturating_mul(lo, lo) == hi);
    CHECK(saturating_mul(hi, lo) == lo);
    CHECK(saturating_mul(lo, zero) == zero);
    CHECK(saturating_mul(F::from_int(2), F::from_int(3)) == F::from_int(6));
}
