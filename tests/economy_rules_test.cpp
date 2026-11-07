#include "pat/core/rng.hpp"
#include "pat/sim/economy/breakdown.hpp"
#include "pat/sim/economy/money.hpp"
#include "pat/sim/economy/rules.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <fstream>
#include <limits>
#include <span>
#include <sstream>
#include <string>
#include <vector>

using namespace pat::sim::economy;
using pat::core::Fixed64;
using pat::core::Pcg32;

namespace {

constexpr Kopecks kMax = std::numeric_limits<Kopecks>::max();
constexpr Kopecks kMin = std::numeric_limits<Kopecks>::min();

pat::core::Fixed32 Fixed32Points(int whole) {
    return pat::core::Fixed32::from_int(whole);
}

/// Random kopeck amount of every magnitude, both signs, with the extremes mixed in.
Kopecks random_amount(Pcg32& rng) {
    switch (rng.bounded(8)) {
        case 0:
            return kMax;
        case 1:
            return kMin;
        case 2:
            return static_cast<Kopecks>(rng.range(-1000, 1000));
        case 3:
            return static_cast<Kopecks>(rng.next_u64() % static_cast<std::uint64_t>(kMaxTreasuryKop)) *
                   (rng.chance_permille(500) ? 1 : -1);
        default:
            return static_cast<Kopecks>(rng.next_u32()) * (rng.chance_permille(500) ? 1 : -1);
    }
}

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

bool contains_word(const std::string& text, const std::string& word) {
    const auto is_ident = [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    };
    for (std::size_t pos = text.find(word); pos != std::string::npos; pos = text.find(word, pos + 1)) {
        const bool left_ok = pos == 0 || !is_ident(text[pos - 1]);
        const std::size_t end = pos + word.size();
        const bool right_ok = end >= text.size() || !is_ident(text[end]);
        if (left_ok && right_ok) {
            return true;
        }
    }
    return false;
}

}  // namespace

// ---- money: checked and saturating arithmetic --------------------------------------------------

TEST_CASE("economy money: constants are consistent") {
    static_assert(kKopecksPerRouble == 100);
    static_assert(kMaxPostingKop < kMaxTreasuryKop);
    // a treasury and one posting can be added without overflowing int64
    static_assert(kMaxTreasuryKop + kMaxPostingKop > 0);
    CHECK(kMaxTreasuryKop == 1'000'000'000'000'000);
    CHECK(kMaxPostingKop == 10'000'000'000'000);
}

TEST_CASE("economy money: checked_add / checked_sub detect overflow at the edges") {
    CHECK(checked_add(1, 2) == 3);
    CHECK(checked_add(-5, 3) == -2);
    CHECK(checked_add(kMax, 0) == kMax);
    CHECK(checked_add(kMax, -1) == kMax - 1);
    CHECK(checked_add(kMin, kMax) == -1);
    CHECK_FALSE(checked_add(kMax, 1).has_value());
    CHECK_FALSE(checked_add(kMin, -1).has_value());
    CHECK_FALSE(checked_add(kMax, kMax).has_value());
    CHECK_FALSE(checked_add(kMin, kMin).has_value());

    CHECK(checked_sub(5, 3) == 2);
    CHECK(checked_sub(3, 5) == -2);
    CHECK(checked_sub(kMin, 0) == kMin);
    CHECK(checked_sub(kMax, kMax) == 0);
    CHECK(checked_sub(-1, kMax) == kMin);
    CHECK_FALSE(checked_sub(kMin, 1).has_value());
    CHECK_FALSE(checked_sub(kMax, -1).has_value());
    CHECK_FALSE(checked_sub(0, kMin).has_value());
    CHECK_FALSE(checked_sub(-2, kMax).has_value());
}

TEST_CASE("economy money: checked_neg and checked_mul") {
    CHECK(checked_neg(5) == -5);
    CHECK(checked_neg(-5) == 5);
    CHECK(checked_neg(kMax) == -kMax);
    CHECK_FALSE(checked_neg(kMin).has_value());

    CHECK(checked_mul(0, kMax) == 0);
    CHECK(checked_mul(-3, 4) == -12);
    CHECK(checked_mul(-3, -4) == 12);
    CHECK(checked_mul(kMax, 1) == kMax);
    CHECK(checked_mul(kMin, 1) == kMin);
    CHECK(checked_mul(kMax, -1) == -kMax);
    CHECK(checked_mul(kMin, 0) == 0);
    CHECK_FALSE(checked_mul(kMin, -1).has_value());
    CHECK_FALSE(checked_mul(kMax, 2).has_value());
    CHECK_FALSE(checked_mul(kMin, 2).has_value());
    CHECK_FALSE(checked_mul(3'037'000'500, 3'037'000'500).has_value());
    CHECK(checked_mul(3'037'000'499, 3'037'000'499).has_value());
}

TEST_CASE("economy money: saturating operations clamp to the int64 range") {
    CHECK(saturating_add(1, 2) == 3);
    CHECK(saturating_add(kMax, 1) == kMax);
    CHECK(saturating_add(kMin, -1) == kMin);
    CHECK(saturating_sub(kMin, 1) == kMin);
    CHECK(saturating_sub(kMax, -1) == kMax);
    CHECK(saturating_sub(0, kMin) == kMax);
    CHECK(saturating_mul(kMax, 2) == kMax);
    CHECK(saturating_mul(kMax, -2) == kMin);
    CHECK(saturating_mul(kMin, -1) == kMax);
    CHECK(saturating_mul(-7, 6) == -42);
}

TEST_CASE("economy money: checked_mul_div needs a positive divisor and a result that fits") {
    CHECK(checked_mul_div(1000, 3, 7) == 428);
    CHECK(checked_mul_div(-1000, 3, 7) == -428);
    CHECK(checked_mul_div(1000, -3, 7) == -428);
    CHECK(checked_mul_div(kMax, kMax, kMax) == kMax);
    CHECK(checked_mul_div(kMin, 3, 3) == kMin);
    CHECK_FALSE(checked_mul_div(10, 10, 0).has_value());
    CHECK_FALSE(checked_mul_div(10, 10, -1).has_value());
    CHECK_FALSE(checked_mul_div(kMax, 2, 1).has_value());
    CHECK_FALSE(checked_mul_div(kMax, kMax, 1).has_value());
}

// ---- money: mul_permille -----------------------------------------------------------------------

TEST_CASE("economy money: mul_permille truncates toward zero for both signs") {
    CHECK(mul_permille(0, 1000) == 0);
    CHECK(mul_permille(12345, 1000) == 12345);
    CHECK(mul_permille(12345, 0) == 0);
    CHECK(mul_permille(1000, 1500) == 1500);
    CHECK(mul_permille(1, 999) == 0);
    CHECK(mul_permille(-1, 999) == 0);  // toward zero, not floor
    CHECK(mul_permille(3, 500) == 1);   // 1.5 -> 1
    CHECK(mul_permille(-3, 500) == -1);
    CHECK(mul_permille(3, -500) == -1);
    CHECK(mul_permille(-3, -500) == 1);
    CHECK(mul_permille(999, 1) == 0);
    CHECK(mul_permille(1999, 1000) == 1999);
    CHECK(mul_permille(1999, 1001) == 2000);  // 2000.999 -> 2000
    CHECK(mul_permille(-1999, 1001) == -2000);
}

TEST_CASE("economy money: mul_permille equals the textbook split form wherever that does not overflow") {
    Pcg32 rng(2024, 11);
    for (int i = 0; i < 5000; ++i) {
        const Kopecks a = static_cast<Kopecks>(rng.next_u64() % 4'000'000'000'000'000ULL) *
                          (rng.chance_permille(500) ? 1 : -1);
        const std::int32_t p = rng.range(-10000, 10000);
        const Kopecks reference = (a / 1000) * p + ((a % 1000) * p) / 1000;
        CAPTURE(a);
        CAPTURE(p);
        REQUIRE(mul_permille(a, p) == reference);
    }
}

TEST_CASE("economy money: mul_permille saturates and reports the overflow") {
    bool saturated = false;
    CHECK(mul_permille(kMax, 1000, &saturated) == kMax);
    CHECK_FALSE(saturated);
    CHECK(mul_permille(kMin, 1000, &saturated) == kMin);
    CHECK_FALSE(saturated);

    CHECK(mul_permille(kMax, 1001, &saturated) == kMax);
    CHECK(saturated);

    saturated = false;
    CHECK(mul_permille(kMin, 1001, &saturated) == kMin);
    CHECK(saturated);

    saturated = false;
    CHECK(mul_permille(kMax, -1001, &saturated) == kMin);
    CHECK(saturated);

    CHECK(mul_permille(kMax, 2000) == kMax);  // the pointer is optional
    CHECK_FALSE(checked_mul_permille(kMax, 2000).has_value());
    CHECK(checked_mul_permille(kMax / 2, 2000).has_value());
}

TEST_CASE("economy money: to_kopecks truncates a Fixed64 toward zero") {
    CHECK(to_kopecks(Fixed64::zero()) == 0);
    CHECK(to_kopecks(Fixed64::from_int(5)) == 5);
    CHECK(to_kopecks(Fixed64::from_int(-5)) == -5);
    const Fixed64 five_and_a_half = Fixed64::from_raw((std::int64_t{5} << 32) + (std::int64_t{1} << 31));
    CHECK(to_kopecks(five_and_a_half) == 5);
    CHECK(to_kopecks(Fixed64::from_raw(-five_and_a_half.raw())) == -5);  // not -6
    CHECK(to_kopecks(Fixed64::from_raw(1)) == 0);
    CHECK(to_kopecks(Fixed64::from_raw(-1)) == 0);
    CHECK(to_kopecks(Fixed64::max()) == (kMax >> 32));
}

// ---- money: postings ---------------------------------------------------------------------------

TEST_CASE("economy money: apply_posting changes a balance by exactly the applied amount") {
    PostingResult r = apply_posting(1000, 250);
    CHECK(r.new_balance == 1250);
    CHECK(r.applied == 250);
    CHECK_FALSE(r.saturated);

    r = apply_posting(1000, -5000);  // a negative treasury is allowed
    CHECK(r.new_balance == -4000);
    CHECK(r.applied == -5000);
    CHECK_FALSE(r.saturated);

    r = apply_posting(kMaxTreasuryKop - 10, 100);
    CHECK(r.new_balance == kMaxTreasuryKop);
    CHECK(r.applied == 10);
    CHECK(r.saturated);

    r = apply_posting(-kMaxTreasuryKop + 10, -100);
    CHECK(r.new_balance == -kMaxTreasuryKop);
    CHECK(r.applied == -10);
    CHECK(r.saturated);

    r = apply_posting(kMaxTreasuryKop, kMax);  // even an absurd amount cannot overflow
    CHECK(r.new_balance == kMaxTreasuryKop);
    CHECK(r.applied == 0);
    CHECK(r.saturated);

    r = apply_posting(-kMaxTreasuryKop, kMin);
    CHECK(r.new_balance == -kMaxTreasuryKop);
    CHECK(r.applied == 0);
    CHECK(r.saturated);

    CHECK(clamp_treasury(kMax) == kMaxTreasuryKop);
    CHECK(clamp_treasury(kMin) == -kMaxTreasuryKop);
    CHECK(clamp_treasury(-7) == -7);
}

TEST_CASE("economy money: balance == opening + sum of applied postings under random load") {
    Pcg32 rng(77, 3);
    Kopecks balance = 0;
    Kopecks lines = 0;
    for (int i = 0; i < 20000; ++i) {
        const Kopecks amount = rng.chance_permille(20)
                                   ? random_amount(rng)
                                   : static_cast<Kopecks>(rng.range(-5'000'000, 5'000'000)) * 1'000'000;
        const PostingResult r = apply_posting(balance, amount);
        balance = r.new_balance;
        lines += r.applied;
        REQUIRE(balance >= -kMaxTreasuryKop);
        REQUIRE(balance <= kMaxTreasuryKop);
        REQUIRE(balance == lines);  // opening is 0
    }
}

// ---- money: splitting without losing a kopeck --------------------------------------------------

TEST_CASE("economy money: split_permille keeps every kopeck") {
    Split s = split_permille(1000, 150);
    CHECK(s.part == 150);
    CHECK(s.rest == 850);

    s = split_permille(1001, 500);  // the odd kopeck stays in `rest`
    CHECK(s.part == 500);
    CHECK(s.rest == 501);

    s = split_permille(-1001, 500);
    CHECK(s.part == -500);
    CHECK(s.rest == -501);

    s = split_permille(7, 0);
    CHECK(s.part == 0);
    CHECK(s.rest == 7);

    s = split_permille(7, 1000);
    CHECK(s.part == 7);
    CHECK(s.rest == 0);

    Pcg32 rng(5, 5);
    for (int i = 0; i < 3000; ++i) {
        const Kopecks a = static_cast<Kopecks>(rng.next_u64() % 1'000'000'000'000'000ULL) *
                          (rng.chance_permille(500) ? 1 : -1);
        const std::int32_t p = rng.range(0, 1000);
        const Split split = split_permille(a, p);
        REQUIRE(split.part + split.rest == a);
    }
}

TEST_CASE("economy money: split_by_weights known results") {
    std::array<Kopecks, 3> out{};
    const std::array<std::uint32_t, 3> equal{1, 1, 1};
    REQUIRE(split_by_weights(100, equal, out));
    CHECK(out == std::array<Kopecks, 3>{33, 33, 34});

    REQUIRE(split_by_weights(-100, equal, out));
    CHECK(out == std::array<Kopecks, 3>{-33, -33, -34});

    REQUIRE(split_by_weights(0, equal, out));
    CHECK(out == std::array<Kopecks, 3>{0, 0, 0});

    const std::array<std::uint32_t, 3> uneven{1, 2, 7};
    REQUIRE(split_by_weights(1000, uneven, out));
    CHECK(out == std::array<Kopecks, 3>{100, 200, 700});

    const std::array<std::uint32_t, 3> with_gap{0, 5, 0};
    REQUIRE(split_by_weights(77, with_gap, out));
    CHECK(out == std::array<Kopecks, 3>{0, 77, 0});

    const std::array<std::uint32_t, 3> none{0, 0, 0};
    out = {9, 9, 9};
    CHECK_FALSE(split_by_weights(100, none, out));
    CHECK(out == std::array<Kopecks, 3>{0, 0, 0});

    std::array<Kopecks, 1> single{};
    const std::array<std::uint32_t, 1> one{4'000'000'000U};
    REQUIRE(split_by_weights(kMin, one, single));
    CHECK(single[0] == kMin);

    std::array<Kopecks, 0> nothing{};
    CHECK_FALSE(split_by_weights(5, std::span<const std::uint32_t>{}, nothing));
}

TEST_CASE("economy money: split_by_weights never loses or creates a kopeck (random property)") {
    Pcg32 rng(31337, 1);
    for (int round = 0; round < 4000; ++round) {
        const auto count = static_cast<std::size_t>(rng.range(1, 12));
        std::vector<std::uint32_t> weights(count);
        bool any = false;
        for (std::uint32_t& w : weights) {
            switch (rng.bounded(4)) {
                case 0:
                    w = 0;
                    break;
                case 1:
                    w = rng.next_u32();  // up to 4e9: populations and shares never get that large, but must
                                         // still work
                    break;
                default:
                    w = static_cast<std::uint32_t>(rng.range(1, 100000));
                    break;
            }
            any = any || w != 0;
        }
        const Kopecks amount = random_amount(rng);
        std::vector<Kopecks> parts(count, 12345);

        const bool ok = split_by_weights(amount, weights, parts);
        CAPTURE(round);
        CAPTURE(amount);
        if (!any) {
            REQUIRE_FALSE(ok);
            continue;
        }
        REQUIRE(ok);

        Kopecks sum = 0;
        for (std::size_t i = 0; i < count; ++i) {
            // each part has the sign of the amount, never exceeds it, and zero weight gets nothing
            if (amount >= 0) {
                REQUIRE(parts[i] >= 0);
                REQUIRE(parts[i] <= amount);
            } else {
                REQUIRE(parts[i] <= 0);
                REQUIRE(parts[i] >= amount);
            }
            if (weights[i] == 0) {
                REQUIRE(parts[i] == 0);
            }
            sum += parts[i];  // cannot overflow: parts share a sign and add up to `amount`
        }
        REQUIRE(sum == amount);
    }
}

TEST_CASE("economy money: split_by_weights stays within one kopeck per part of the exact share") {
    Pcg32 rng(4, 4);
    for (int round = 0; round < 1000; ++round) {
        const auto count = static_cast<std::size_t>(rng.range(2, 8));
        std::vector<std::uint32_t> weights(count);
        std::uint64_t total = 0;
        for (std::uint32_t& w : weights) {
            w = static_cast<std::uint32_t>(rng.range(1, 5000));
            total += w;
        }
        const Kopecks amount = rng.range(0, 2'000'000);
        std::vector<Kopecks> parts(count);
        REQUIRE(split_by_weights(amount, weights, parts));
        for (std::size_t i = 0; i < count; ++i) {
            const Kopecks exact_floor =
                amount * static_cast<Kopecks>(weights[i]) / static_cast<Kopecks>(total);
            const Kopecks diff = parts[i] > exact_floor ? parts[i] - exact_floor : exact_floor - parts[i];
            CHECK(diff <= static_cast<Kopecks>(count));
        }
    }
}

// ---- breakdown ---------------------------------------------------------------------------------

TEST_CASE("economy breakdown: a tax tree adds up and keeps insertion order") {
    Breakdown b;
    CHECK(b.empty());
    b.begin(term::kOutputValue, Unit::Kopecks, 1000);
    CHECK(b.size() == 1);
    CHECK(b.total() == 1000);
    CHECK(b.root().parent == kNoNode);

    const NodeIndex base = b.add_term(kRootNode, term::kTaxBase, 1200);
    const NodeIndex leak = b.add_term(kRootNode, term::kTaxLeak, -200);
    REQUIRE(base == 1);
    REQUIRE(leak == 2);
    CHECK(b.node(base).code == term::kTaxBase);
    CHECK(b.node(leak).value == -200);
    CHECK(b.node(base).parent == kRootNode);
    CHECK(b.child_count(kRootNode) == 2);
    CHECK(b.children_sum(kRootNode) == 1000);
    CHECK(b.is_consistent());
    CHECK_FALSE(b.truncated());

    // a contribution under a term: the term must then equal the sum of its contributions
    CHECK_FALSE(b.add_term(base, term::kTaxPolicy, 1000) == kNoNode);
    CHECK_FALSE(b.is_consistent());  // 1000 != 1200
    CHECK(b.add_term(base, term::kTaxWealth, 200) == 4);
    CHECK(b.is_consistent());
    CHECK(b.depth(kRootNode) == 0);
    CHECK(b.depth(base) == 1);
    CHECK(b.depth(4) == 2);
    CHECK(b.find(term::kTaxWealth) == 4);
    CHECK(b.find(term::kTaxLaw) == kNoNode);

    // the order of nodes is the order of insertion
    CHECK(b.node(0).code == term::kOutputValue);
    CHECK(b.node(1).code == term::kTaxBase);
    CHECK(b.node(2).code == term::kTaxLeak);
    CHECK(b.node(3).code == term::kTaxPolicy);
    CHECK(b.node(4).code == term::kTaxWealth);
}

TEST_CASE("economy breakdown: a wrong total is detected") {
    Breakdown b;
    b.begin(term::kOutputValue, Unit::Kopecks, 999);
    (void)b.add_term(kRootNode, term::kTaxBase, 1000);
    CHECK_FALSE(b.is_consistent());
    b.set_total(1000);
    CHECK(b.is_consistent());
    b.set_total(0);
    CHECK(b.set_total_from_children());
    CHECK(b.total() == 1000);
    CHECK(b.is_consistent());
}

TEST_CASE("economy breakdown: a chain of multipliers becomes additive deltas") {
    // 10000 kop x1.20 x0.90 x1.05, every step truncated like the real formulas will
    Kopecks running = 10000;
    Breakdown b;
    b.begin(term::kTaxBase, Unit::Kopecks, 0);
    (void)b.add_term(kRootNode, term::kTaxBase, running);
    for (const std::int32_t factor : {1200, 900, 1050}) {
        const Kopecks next = mul_permille(running, factor);
        REQUIRE(b.add_step(kRootNode, term::kTaxPolicy, running, next, factor) != kNoNode);
        running = next;
    }
    REQUIRE(b.set_total_from_children());
    CHECK(b.total() == running);
    CHECK(b.total() == 11340);
    CHECK(b.is_consistent());
    CHECK(b.node(2).value == 2000);
    CHECK(b.node(2).factor_permille == 1200);
    CHECK(b.node(3).value == -1200);
    CHECK(b.node(4).value == 540);
}

TEST_CASE("economy breakdown: add_remainder closes a parent by subtraction") {
    Breakdown b;
    b.begin(term::kOutputValue, Unit::Kopecks, 1001);
    (void)b.add_term(kRootNode, term::kTaxBase, 400);
    (void)b.add_term(kRootNode, term::kTaxPolicy, 300);
    const NodeIndex rest = b.add_remainder(kRootNode, term::kTaxLaw);
    REQUIRE(rest != kNoNode);
    CHECK(b.node(rest).value == 301);
    CHECK(b.is_consistent());
    // nothing left to add
    CHECK(b.add_remainder(kRootNode, term::kTaxLeak) == kNoNode);
    CHECK_FALSE(b.truncated());
    CHECK(b.size() == 4);
}

TEST_CASE("economy breakdown: depth is limited to total -> term -> contribution") {
    Breakdown b;
    b.begin(term::kOutputValue, Unit::Kopecks, 0);
    const NodeIndex term_node = b.add_term(kRootNode, term::kTaxBase, 0);
    const NodeIndex contribution = b.add_term(term_node, term::kTaxPolicy, 0);
    REQUIRE(contribution != kNoNode);
    CHECK_FALSE(b.truncated());
    CHECK(b.add_term(contribution, term::kTaxWealth, 0) == kNoNode);
    CHECK(b.truncated());
    CHECK_FALSE(b.is_consistent());
    CHECK(b.size() == 3);
}

TEST_CASE("economy breakdown: at most 32 nodes, no allocation, truncation is flagged") {
    static_assert(kBreakdownCapacity == 32);
    Breakdown b;
    b.begin(term::kOutputValue, Unit::Kopecks, 0);
    for (std::size_t i = 1; i < kBreakdownCapacity; ++i) {
        REQUIRE(b.add_term(kRootNode, term::kTaxBase, 1) != kNoNode);
    }
    CHECK(b.size() == kBreakdownCapacity);
    CHECK_FALSE(b.truncated());
    CHECK(b.add_term(kRootNode, term::kTaxBase, 1) == kNoNode);
    CHECK(b.truncated());
    CHECK(b.size() == kBreakdownCapacity);

    // begin() starts over
    b.begin(term::kOutputValue, Unit::Kopecks, 5);
    CHECK(b.size() == 1);
    CHECK_FALSE(b.truncated());
    CHECK(b.total() == 5);
    CHECK(sizeof(Breakdown) <= 32 * sizeof(BreakdownNode) + 32);
}

TEST_CASE("economy breakdown: invalid parents and overflow are refused, not undefined") {
    Breakdown unstarted;
    CHECK(unstarted.add_term(kRootNode, term::kTaxBase, 1) == kNoNode);
    CHECK(unstarted.truncated());

    Breakdown b;
    b.begin(term::kOutputValue, Unit::Kopecks, 0);
    CHECK(b.add_term(7, term::kTaxBase, 1) == kNoNode);  // no such parent
    CHECK(b.truncated());

    b.begin(term::kOutputValue, Unit::Kopecks, 0);
    CHECK(b.add_step(kRootNode, term::kTaxBase, kMin, kMax) == kNoNode);  // delta does not fit
    CHECK(b.truncated());

    b.begin(term::kOutputValue, Unit::Kopecks, 0);
    (void)b.add_term(kRootNode, term::kTaxBase, kMax);
    (void)b.add_term(kRootNode, term::kTaxPolicy, 1);
    CHECK_FALSE(b.children_sum(kRootNode).has_value());
    CHECK_FALSE(b.set_total_from_children());
    CHECK_FALSE(b.is_consistent());
}

TEST_CASE("economy breakdown: the unit is inherited and building twice gives equal trees") {
    const auto build = [] {
        Breakdown b;
        b.begin(term::kMoodStep, Unit::Points, -3);
        (void)b.add_term(kRootNode, term::kMoodBase, 5 * 65536);
        (void)b.add_term(kRootNode, term::kMoodTaxPressure, -8 * 65536, 0);
        return b;
    };
    const Breakdown a = build();
    const Breakdown b = build();
    CHECK(a == b);
    CHECK(a.node(1).unit == Unit::Points);
    Breakdown c = build();
    (void)c.add_term(kRootNode, term::kMoodEvents, 1);
    CHECK_FALSE(a == c);
}

TEST_CASE("economy breakdown: term codes have unique, ascending, non-empty localisation keys") {
    const std::size_t count = term_code_count();
    REQUIRE(count > 50);
    std::vector<std::string> keys;
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint16_t code = term_code_at(i);
        if (i > 0) {
            CHECK(code > term_code_at(i - 1));
        }
        const std::string key(term_key(code));
        CHECK_FALSE(key.empty());
        CHECK(key.rfind("economy.", 0) == 0);
        for (const std::string& seen : keys) {
            CHECK(seen != key);
        }
        keys.push_back(key);
    }
    CHECK(term_key(term::kTaxBase) == "economy.tax.base");
    CHECK(term_key(term::kUnrestGroupFirst + 3) == "economy.unrest.peasants");
    CHECK(term_key(0).empty());
    CHECK(term_key(1000).empty());
    CHECK(term_key(65535).empty());
}

// ---- rules -------------------------------------------------------------------------------------

TEST_CASE("economy rules: the defaults are valid and stable") {
    const EconomyRules rules = EconomyRules::defaults();
    const auto result = validate(rules);
    const std::string problem = result.has_value() ? std::string() : result.error().message;
    CHECK_MESSAGE(result.has_value(), problem);
    CHECK(rules == EconomyRules::defaults());
}

TEST_CASE("economy rules: defaults carry the numbers the GDD and the model give") {
    const EconomyRules r = EconomyRules::defaults();
    // GDD 03 s.3.2
    CHECK(r.control.low_threshold == 40);
    CHECK(r.control.anarchy_threshold == 15);
    // GDD 03 s.4.3
    CHECK(r.unrest.calm_below == 25);
    CHECK(r.unrest.riot_from == 50);
    CHECK(r.unrest.uprising_from == 75);
    // economy-model s.11.1 example
    CHECK(r.production.base_price_kop[0] == 1200);
    CHECK(r.consumption.need_per_head_milli[index_of(Group::Peasants)][0] == 35);
    CHECK(r.tax.levels[3].tax_permille == 1600);
    REQUIRE(r.buildings.count >= 1);
    CHECK(r.buildings.defs[0].cost_kop == 400'000);
    CHECK(r.buildings.defs[0].build_days == 120);
    // interim decisions of the open questions (economy-model s.15.2)
    CHECK(r.crown_account_mode == CrownAccountMode::Separate);
    CHECK_FALSE(r.build_foreign_allowed);
    // nobility and clergy pay no money tax in M1 (economy-model s.6.1)
    CHECK(r.tax.head_tax_kop_year[index_of(Group::Nobility)] == 0);
    CHECK(r.tax.head_tax_kop_year[index_of(Group::Clergy)] == 0);
}

TEST_CASE("economy rules: default growth sits in the 0.3...1.0 percent a year band") {
    const EconomyRules r = EconomyRules::defaults();
    for (std::size_t g = 0; g < kGroupCount; ++g) {
        const std::int32_t net = r.population.birth_ppm_year[g] - r.population.death_ppm_year[g];
        CAPTURE(g);
        CHECK(net >= 3000);
        CHECK(net <= 10000);
    }
}

TEST_CASE("economy rules: tests can substitute any value and validation follows") {
    EconomyRules r = EconomyRules::defaults();
    r.tax.head_tax_kop_year[index_of(Group::Peasants)] = 999;
    r.control.low_threshold = 50;
    r.unrest.riot_from = 60;
    CHECK(validate(r).has_value());
    CHECK_FALSE(r == EconomyRules::defaults());
}

TEST_CASE("economy rules: validation rejects broken rules with a code and a message") {
    struct Case {
        const char* name;
        void (*mutate)(EconomyRules&);
        EconomyRulesErrorCode code;
    };
    const std::vector<Case> cases{
        {"birth rate above 100%",
         [](EconomyRules& r) { r.population.birth_ppm_year[3] = 2'000'000; },
         EconomyRulesErrorCode::Population},
        {"negative death rate",
         [](EconomyRules& r) { r.population.death_ppm_year[0] = -1; },
         EconomyRulesErrorCode::Population},
        {"zero capacity",
         [](EconomyRules& r) { r.population.capacity_per_development = 0; },
         EconomyRulesErrorCode::Population},
        {"labour min above max",
         [](EconomyRules& r) { r.production.labour_min_permille = 2000; },
         EconomyRulesErrorCode::Production},
        {"losses above 100%",
         [](EconomyRules& r) { r.production.losses_permille = 1001; },
         EconomyRulesErrorCode::Production},
        {"negative price",
         [](EconomyRules& r) { r.production.base_price_kop[2] = -1; },
         EconomyRulesErrorCode::Production},
        {"owner share above 100%",
         [](EconomyRules& r) { r.production.owner_share_permille[1] = 1500; },
         EconomyRulesErrorCode::Production},
        {"negative need",
         [](EconomyRules& r) { r.consumption.need_per_head_milli[2][4] = -1; },
         EconomyRulesErrorCode::Consumption},
        {"zero stock cap",
         [](EconomyRules& r) { r.consumption.stock_cap_months = 0; },
         EconomyRulesErrorCode::Consumption},
        {"wealth base above 300",
         [](EconomyRules& r) { r.consumption.wealth_base[3] = 301; },
         EconomyRulesErrorCode::Consumption},
        {"negative head tax",
         [](EconomyRules& r) { r.tax.head_tax_kop_year[3] = -1; },
         EconomyRulesErrorCode::Tax},
        {"city share above 100%",
         [](EconomyRules& r) { r.tax.city_crown_share_permille = 1001; },
         EconomyRulesErrorCode::Tax},
        {"no tax levels", [](EconomyRules& r) { r.tax.level_count = 0; }, EconomyRulesErrorCode::Tax},
        {"too many tax levels", [](EconomyRules& r) { r.tax.level_count = 9; }, EconomyRulesErrorCode::Tax},
        {"default tax level out of range",
         [](EconomyRules& r) { r.tax.default_level = 4; },
         EconomyRulesErrorCode::Tax},
        {"tax multiplier too large",
         [](EconomyRules& r) { r.tax.levels[1].tax_permille = 20000; },
         EconomyRulesErrorCode::Tax},
        {"mood pressure out of range",
         [](EconomyRules& r) { r.tax.levels[2].mood_pressure[3] = Fixed32Points(150); },
         EconomyRulesErrorCode::Tax},
        {"anarchy above low control",
         [](EconomyRules& r) { r.control.anarchy_threshold = 40; },
         EconomyRulesErrorCode::Control},
        {"low control above 100",
         [](EconomyRules& r) { r.control.low_threshold = 101; },
         EconomyRulesErrorCode::Control},
        {"corruption base above 100%",
         [](EconomyRules& r) { r.control.corruption_base_permille = 1001; },
         EconomyRulesErrorCode::Control},
        {"inertia above 100%",
         [](EconomyRules& r) { r.mood.inertia_permille = 1001; },
         EconomyRulesErrorCode::Mood},
        {"base mood out of range",
         [](EconomyRules& r) { r.mood.base_mood[0] = Fixed32Points(-101); },
         EconomyRulesErrorCode::Mood},
        {"unrest thresholds out of order",
         [](EconomyRules& r) { r.unrest.riot_from = 20; },
         EconomyRulesErrorCode::Unrest},
        {"uprising above 100",
         [](EconomyRules& r) { r.unrest.uprising_from = 101; },
         EconomyRulesErrorCode::Unrest},
        {"riot chance min above max",
         [](EconomyRules& r) { r.unrest.riot_chance_min_permille = 900; },
         EconomyRulesErrorCode::Unrest},
        {"no influence at all",
         [](EconomyRules& r) {
             for (auto& w : r.unrest.influence_permille) {
                 w = 0;
             }
         },
         EconomyRulesErrorCode::Unrest},
        {"recruit share above 100%",
         [](EconomyRules& r) { r.recruit.share_permille[3] = 1001; },
         EconomyRulesErrorCode::Recruit},
        {"no recruit levels",
         [](EconomyRules& r) { r.recruit.level_count = 0; },
         EconomyRulesErrorCode::Recruit},
        {"default recruit level out of range",
         [](EconomyRules& r) { r.recruit.default_level = 3; },
         EconomyRulesErrorCode::Recruit},
        {"negative loan rate",
         [](EconomyRules& r) { r.loans.rate_ppm_year = -1; },
         EconomyRulesErrorCode::Loans},
        {"zero loan term", [](EconomyRules& r) { r.loans.term_months = 0; }, EconomyRulesErrorCode::Loans},
        {"bankruptcy exit above threshold",
         [](EconomyRules& r) { r.loans.bankruptcy_exit_kop = r.loans.bankruptcy_threshold_kop; },
         EconomyRulesErrorCode::Loans},
        {"building slot 0",
         [](EconomyRules& r) { r.buildings.defs[0].slot = 0; },
         EconomyRulesErrorCode::Buildings},
        {"duplicate building slot",
         [](EconomyRules& r) {
             r.buildings.defs[1] = r.buildings.defs[0];
             r.buildings.count = 2;
         },
         EconomyRulesErrorCode::Buildings},
        {"building without a key",
         [](EconomyRules& r) { r.buildings.defs[0].key = {}; },
         EconomyRulesErrorCode::Buildings},
        {"building takes no time",
         [](EconomyRules& r) { r.buildings.defs[0].build_days = 0; },
         EconomyRulesErrorCode::Buildings},
        {"building cost negative",
         [](EconomyRules& r) { r.buildings.defs[0].cost_kop = -5; },
         EconomyRulesErrorCode::Buildings},
        {"too many buildings",
         [](EconomyRules& r) { r.buildings.count = 17; },
         EconomyRulesErrorCode::Buildings},
        {"refund above 100%",
         [](EconomyRules& r) { r.buildings.refund_permille = 1200; },
         EconomyRulesErrorCode::Buildings},
        {"unknown crown mode",
         [](EconomyRules& r) { r.crown_account_mode = static_cast<CrownAccountMode>(9); },
         EconomyRulesErrorCode::Flags},
    };
    for (const Case& c : cases) {
        CAPTURE(c.name);
        EconomyRules rules = EconomyRules::defaults();
        c.mutate(rules);
        const auto result = validate(rules);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == c.code);
        CHECK_FALSE(result.error().message.empty());
    }
}

TEST_CASE("economy rules: a second building with a distinct slot is accepted") {
    EconomyRules r = EconomyRules::defaults();
    r.buildings.defs[1] = r.buildings.defs[0];
    r.buildings.defs[1].slot = 2;
    r.buildings.defs[1].key = "building.manufactory";
    r.buildings.count = 2;
    CHECK(validate(r).has_value());
}

// ---- determinism and floating point ------------------------------------------------------------

TEST_CASE("economy: money and breakdown results are reproducible from a seed") {
    const auto run = [] {
        Pcg32 rng(9001, 2);
        Breakdown b;
        b.begin(term::kTaxBase, Unit::Kopecks, 0);
        Kopecks running = 100000;
        (void)b.add_term(kRootNode, term::kTaxBase, running);
        for (int i = 0; i < 8; ++i) {
            const std::int32_t factor = rng.range(500, 1500);
            const Kopecks next = mul_permille(running, factor);
            (void)b.add_step(kRootNode, term::kTaxPolicy, running, next, factor);
            running = next;
        }
        (void)b.set_total_from_children();
        return b;
    };
    const Breakdown first = run();
    const Breakdown second = run();
    CHECK(first == second);
    CHECK(first.is_consistent());
}

TEST_CASE("economy: the module contains no floating-point types") {
    const std::string root = std::string(PATRIARCH_SOURCE_DIR) + "/src/sim/";
    for (const char* file : {"include/pat/sim/economy/types.hpp",
                             "include/pat/sim/economy/money.hpp",
                             "include/pat/sim/economy/breakdown.hpp",
                             "include/pat/sim/economy/rules.hpp",
                             "economy/breakdown.cpp",
                             "economy/rules.cpp"}) {
        const std::string text = read_file(root + file);
        CAPTURE(file);
        REQUIRE_FALSE(text.empty());
        for (const char* word :
             {"float", "double", "to_double", "from_double", "std::pow", "std::sqrt", "std::lround"}) {
            CAPTURE(word);
            CHECK_FALSE(contains_word(text, word));
        }
    }
}
