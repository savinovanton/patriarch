#include "pat/core/detail/wide_math.hpp"
#include "pat/core/fixed.hpp"
#include "pat/core/log.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <format>

using namespace pat::core;

namespace {

// Tiny deterministic generator for property tests (independent from the PRNG under test).
struct XorShift64 {
    std::uint64_t s = 0x9E3779B97F4A7C15ULL;
    std::uint64_t next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    }
    // value in [-limit, limit]
    std::int64_t symmetric(std::int64_t limit) {
        const std::uint64_t span = static_cast<std::uint64_t>(limit) * 2 + 1;
        return static_cast<std::int64_t>(next() % span) - limit;
    }
};

constexpr int kSamples = 20000;

}  // namespace

TEST_CASE("wide_math: 64x64 multiplication matches known products") {
    using namespace pat::core::detail;
    const U128 a = mul_u64(0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL);
    CHECK(a.hi == 0xFFFFFFFFFFFFFFFEULL);
    CHECK(a.lo == 1ULL);

    const U128 b = mul_u64(1ULL << 40, 1ULL << 40);
    CHECK(b.hi == 1ULL << 16);
    CHECK(b.lo == 0);

    const U128 c = mul_u64(123456789ULL, 987654321ULL);
    CHECK(c.hi == 0);
    CHECK(c.lo == 121932631112635269ULL);
}

TEST_CASE("wide_math: division, shifts and square root") {
    using namespace pat::core::detail;
    bool overflow = false;
    CHECK(div_u128_by_u64(U128{0, 100}, 7, overflow) == 14);
    CHECK_FALSE(overflow);

    const U128 big = mul_u64(0xFFFFFFFFFFFFFFFFULL, 12345ULL);
    CHECK(div_u128_by_u64(big, 12345ULL, overflow) == 0xFFFFFFFFFFFFFFFFULL);
    CHECK_FALSE(overflow);

    overflow = false;
    (void)div_u128_by_u64(U128{5, 0}, 3, overflow);  // quotient needs more than 64 bits
    CHECK(overflow);

    const U128 v{0x1234, 0x5678};
    CHECK(shr(shl(v, 70), 70).lo == v.lo);

    CHECK(isqrt_u128(U128{0, 0}) == 0);
    CHECK(isqrt_u128(U128{0, 16}) == 4);
    CHECK(isqrt_u128(U128{0, 17}) == 4);
    CHECK(isqrt_u128(mul_u64(0xFFFFFFFFULL, 0xFFFFFFFFULL)) == 0xFFFFFFFFULL);
    CHECK(isqrt_u128(U128{0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL}) == 0xFFFFFFFFFFFFFFFFULL);
}

TEST_CASE_TEMPLATE("fixed: construction and conversion", F, Fixed32, Fixed64) {
    CHECK(F().raw() == 0);
    CHECK(F::from_int(3).to_int() == 3);
    CHECK(F::from_int(-3).to_int() == -3);
    CHECK(F::from_raw(F::one_raw).to_int() == 1);
    CHECK(F::from_ratio(1, 2).raw() == F::one_raw / 2);
    CHECK(F::from_ratio(-1, 4).raw() == -(F::one_raw / 4));
    CHECK(F::from_ratio(7, 2).to_int() == 3);
    CHECK(F::from_ratio(-7, 2).to_int() == -3);     // truncation toward zero
    CHECK(F::from_ratio(-7, 2).floor_int() == -4);  // floor
    CHECK(F::from_ratio(5, 2).round_int() == 3);    // half away from zero
    CHECK(F::from_ratio(-5, 2).round_int() == -3);
    CHECK(F::from_ratio(7, 4).round_int() == 2);
    CHECK(F::from_double(1.5).raw() == F::one_raw + F::one_raw / 2);
    CHECK(F::from_int(2).to_double() == doctest::Approx(2.0));
}

TEST_CASE_TEMPLATE("fixed: comparison", F, Fixed32, Fixed64) {
    CHECK(F::from_int(1) < F::from_int(2));
    CHECK(F::from_int(-2) < F::from_int(-1));
    CHECK(F::from_int(2) == F::from_int(2));
    CHECK(F::from_int(2) != F::from_int(3));
    CHECK(F::max() > F::zero());
    CHECK(F::min() < F::zero());
}

TEST_CASE_TEMPLATE("fixed: multiplication and division truncate toward zero", F, Fixed32, Fixed64) {
    // 1 ulp * 1 ulp is far below resolution: 0 for both signs (not -1).
    CHECK((F::from_raw(1) * F::from_raw(1)).raw() == 0);
    CHECK((F::from_raw(-1) * F::from_raw(1)).raw() == 0);
    // 3 ulp * 0.5 = 1.5 ulp -> 1 (toward zero), and -1 for the negative counterpart.
    CHECK((F::from_raw(3) * F::from_ratio(1, 2)).raw() == 1);
    CHECK((F::from_raw(-3) * F::from_ratio(1, 2)).raw() == -1);
    // 1 / 3 truncated.
    CHECK((F::from_int(1) / F::from_int(3)).raw() == F::one_raw / 3);
    CHECK((F::from_int(-1) / F::from_int(3)).raw() == -(F::one_raw / 3));
    CHECK((F::from_int(6) / F::from_int(-3)) == F::from_int(-2));
}

TEST_CASE_TEMPLATE("fixed: algebraic properties on random inputs", F, Fixed32, Fixed64) {
    constexpr std::int64_t limit = sizeof(typename F::raw_type) == 4 ? (1LL << 28) : (1LL << 58);
    XorShift64 rng;
    for (int i = 0; i < kSamples; ++i) {
        const F a = F::from_raw(static_cast<typename F::raw_type>(rng.symmetric(limit)));
        const F b = F::from_raw(static_cast<typename F::raw_type>(rng.symmetric(limit)));

        CHECK(a + b == b + a);
        CHECK((a + b) - b == a);
        CHECK(a - b == -(b - a));
        CHECK(a * F::one() == a);
        CHECK(a * F::zero() == F::zero());
        CHECK(F::zero() + a == a);
        CHECK(-(-a) == a);
        CHECK(abs(a) >= F::zero());
        CHECK((a < b) == (a.raw() < b.raw()));
    }
}

TEST_CASE_TEMPLATE("fixed: products and quotients agree with double within resolution", F, Fixed32, Fixed64) {
    // Operands are kept small so that the double reference itself is accurate to well below one ulp
    // of F (a double has 53 mantissa bits; products must stay below ~2^20 for Q32.32).
    constexpr std::int64_t limit = sizeof(typename F::raw_type) == 4 ? (1LL << 22) : (1LL << 41);
    const double ulp = 1.0 / static_cast<double>(F::one_raw);
    XorShift64 rng;
    for (int i = 0; i < kSamples; ++i) {
        const F a = F::from_raw(static_cast<typename F::raw_type>(rng.symmetric(limit)));
        const F b = F::from_raw(static_cast<typename F::raw_type>(rng.symmetric(limit)));

        const double expected_mul = a.to_double() * b.to_double();
        CHECK(std::fabs((a * b).to_double() - expected_mul) <= ulp * 1.0001);
        CHECK(a * b == b * a);

        if (b.raw() != 0) {
            const double expected_div = a.to_double() / b.to_double();
            if (std::fabs(expected_div) < 1000.0) {
                CHECK(std::fabs((a / b).to_double() - expected_div) <=
                      ulp * 1.0001 + std::fabs(expected_div) * 1e-12);
            }
        }
    }
}

TEST_CASE_TEMPLATE("fixed: integer scaling helpers", F, Fixed32, Fixed64) {
    const F x = F::from_int(10);
    CHECK(x * 3 == F::from_int(30));
    CHECK(3 * x == F::from_int(30));
    CHECK(x / 4 == F::from_ratio(5, 2));
    CHECK(x.mul_div(3, 4) == F::from_ratio(15, 2));
    CHECK(x.mul_permille(250) == F::from_ratio(5, 2));
    CHECK(F::from_int(-10).mul_permille(250) == F::from_ratio(-5, 2));
    CHECK(x.mul_permille(1000) == x);
}

TEST_CASE_TEMPLATE("fixed: saturating arithmetic clamps instead of overflowing", F, Fixed32, Fixed64) {
    CHECK(saturating_add(F::max(), F::one()) == F::max());
    CHECK(saturating_add(F::min(), -F::one()) == F::min());
    CHECK(saturating_sub(F::min(), F::one()) == F::min());
    CHECK(saturating_sub(F::max(), -F::one()) == F::max());
    CHECK(saturating_mul(F::max(), F::from_int(2)) == F::max());
    CHECK(saturating_mul(F::min(), F::from_int(2)) == F::min());
    CHECK(saturating_mul(F::max(), F::from_int(-2)) == F::min());
    CHECK(saturating_add(F::from_int(1), F::from_int(2)) == F::from_int(3));
}

TEST_CASE_TEMPLATE("fixed: sqrt", F, Fixed32, Fixed64) {
    CHECK(sqrt(F::zero()) == F::zero());
    CHECK(sqrt(F::from_int(16)) == F::from_int(4));
    CHECK(sqrt(F::from_int(2)).to_double() == doctest::Approx(std::sqrt(2.0)).epsilon(1e-4));

    XorShift64 rng;
    constexpr std::int64_t limit = sizeof(typename F::raw_type) == 4 ? (1LL << 30) : (1LL << 60);
    for (int i = 0; i < 2000; ++i) {
        const F x = F::from_raw(static_cast<typename F::raw_type>(rng.symmetric(limit) / 2 + limit / 2));
        const F r = sqrt(x);
        // floor property: r*r <= x < (r+ulp)^2, checked in the integer domain with wide math.
        const auto rr = pat::core::detail::mul_u64(static_cast<std::uint64_t>(r.raw()),
                                                   static_cast<std::uint64_t>(r.raw()));
        const auto target = pat::core::detail::shl(
            pat::core::detail::U128{0, static_cast<std::uint64_t>(x.raw())}, F::frac_bits);
        CHECK_FALSE(pat::core::detail::less(target, rr));
        const auto r1 = static_cast<std::uint64_t>(r.raw()) + 1;
        const auto rr1 = pat::core::detail::mul_u64(r1, r1);
        CHECK(pat::core::detail::less(target, rr1));
    }
}

TEST_CASE_TEMPLATE("fixed: sigmoid", F, Fixed32, Fixed64) {
    CHECK(sigmoid(F::zero()) == F::from_ratio(1, 2));
    CHECK(sigmoid(F::from_int(100)).to_double() > 0.9995);
    CHECK(sigmoid(F::from_int(-100)).to_double() < 0.0005);
    CHECK(sigmoid(F::from_int(100)) <= F::one());
    CHECK(sigmoid(F::from_int(-100)) >= F::zero());

    F previous = sigmoid(F::from_int(-9));
    for (int i = -9 * 64; i <= 9 * 64; ++i) {
        const F x =
            F::from_raw(static_cast<typename F::raw_type>((static_cast<std::int64_t>(i) * F::one_raw) / 64));
        const F y = sigmoid(x);
        CHECK(y >= previous);  // monotonic
        previous = y;
        const double exact = 1.0 / (1.0 + std::exp(-x.to_double()));
        if (x.to_double() >= -8.0 && x.to_double() <= 8.0) {
            CHECK(std::fabs(y.to_double() - exact) < 1e-3);
        }
        const F mirrored = sigmoid(-x);
        CHECK(std::fabs((y + mirrored).to_double() - 1.0) < 2e-4);
    }
}

TEST_CASE_TEMPLATE("fixed: lerp and clamp", F, Fixed32, Fixed64) {
    CHECK(lerp(F::from_int(10), F::from_int(20), F::from_ratio(1, 4)) == F::from_ratio(25, 2));
    CHECK(clamp(F::from_int(5), F::from_int(0), F::from_int(3)) == F::from_int(3));
    CHECK(clamp(F::from_int(-5), F::from_int(0), F::from_int(3)) == F::from_int(0));
    CHECK(clamp(F::from_int(2), F::from_int(0), F::from_int(3)) == F::from_int(2));
}

TEST_CASE("fixed: text formatting is float-free") {
    CHECK(to_string(Fixed32::from_ratio(1, 4)) == "0.2500");
    CHECK(to_string(Fixed32::from_ratio(-25, 2), 1) == "-12.5");
    CHECK(to_string(Fixed64::from_int(7), 0) == "7");
    CHECK(to_string(Fixed64::from_ratio(1, 8), 3) == "0.125");
    CHECK(std::format("{}", Fixed32::from_ratio(3, 2)) == "1.5000");
}

#if PAT_ASSERTS_ENABLED
namespace {
int g_overflow_reports = 0;
void count_overflows(const AssertInfo&) {
    ++g_overflow_reports;
}
}  // namespace

TEST_CASE("fixed: overflow is reported in assert-enabled builds") {
    ScopedLogCapture capture;
    ScopedAssertHandler handler(&count_overflows);
    g_overflow_reports = 0;

    volatile std::int32_t big = Fixed32::max().raw();
    (void)(Fixed32::from_raw(big) + Fixed32::one());
    CHECK(g_overflow_reports == 1);
    (void)(Fixed32::from_raw(big) * Fixed32::from_int(2));
    CHECK(g_overflow_reports == 2);
    (void)(Fixed32::one() / Fixed32::from_raw(0));
    CHECK(g_overflow_reports >= 3);
}
#endif
