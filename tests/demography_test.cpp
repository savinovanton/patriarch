#include "pat/sim/demography.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <vector>

using namespace pat::sim;
using pat::core::Fixed32;
using pat::core::Fixed64;

namespace {

const DemographyRules& defaults() {
    return kDefaultDemography;
}

Fixed32 health(int value) {
    return Fixed32::from_int(value);
}

Fixed64 fixed(int value) {
    return Fixed64::from_int(value);
}

/// Whole per-mille result of a mortality query with no trait effect.
Fixed64 mortality(int age, int hp, bool noble) {
    return mortality_permille(defaults(), age, health(hp), noble).total;
}

ConceptionInput couple(int wife_age, int wife_health = 50) {
    ConceptionInput input;
    input.wife_age = wife_age;
    input.wife_health = health(wife_health);
    return input;
}

Fixed64 conception(const ConceptionInput& input) {
    return conception_permille(defaults(), input).total;
}

bool has_term(const ProbabilityBreakdown& b, std::string_view key) {
    for (const ProbabilityTerm& term : b.terms) {
        if (term.key == key) {
            return true;
        }
    }
    return false;
}

/// True when the breakdown is well formed: terms sum to the total, total in 0...1000, the first
/// term is the base value.
bool well_formed(const ProbabilityBreakdown& b) {
    return b.terms.size() >= 1 && b.sum_of_terms() == b.total && !b.total.is_negative() &&
           b.total <= Fixed64::from_int(1000) && b.terms[0].factor_permille == 0;
}

bool same(const ProbabilityBreakdown& a, const ProbabilityBreakdown& b) {
    if (a.total != b.total || a.terms.size() != b.terms.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.terms.size(); ++i) {
        if (!(a.terms[i] == b.terms[i])) {
            return false;
        }
    }
    return true;
}

DemographyRules broken(void (*mutate)(DemographyRules&)) {
    DemographyRules rules = kDefaultDemography;
    mutate(rules);
    return rules;
}

void require_error(const DemographyRules& rules, RulesErrorCode code) {
    const auto result = validate(rules);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == code);
    CHECK_FALSE(result.error().message.empty());
}

}  // namespace

TEST_CASE("demography: default rules pass validation") {
    CHECK(validate(kDefaultDemography).has_value());
}

TEST_CASE("demography: base mortality buckets match GDD 01 3.2 (average health, commoner)") {
    struct Row {
        int age;
        int permille;
    };
    const Row rows[] = {
        {0, 220},
        {1, 70},
        {4, 70},
        {5, 20},
        {14, 20},
        {15, 15},
        {29, 15},
        {30, 22},
        {44, 22},
        {45, 45},
        {59, 45},
        {60, 110},
        {69, 110},
        {70, 240},
        {79, 240},
        {80, 450},
        {95, 450},
        {120, 450},
    };
    for (const Row& row : rows) {
        CAPTURE(row.age);
        CHECK(mortality(row.age, 50, false) == fixed(row.permille));
    }
}

TEST_CASE("demography: infant year is the deadliest of childhood and negative ages count as zero") {
    CHECK(mortality(0, 50, false) == fixed(220));
    CHECK(mortality(0, 50, false) > mortality(1, 50, false));
    CHECK(mortality(1, 50, false) > mortality(5, 50, false));
    CHECK(mortality(-3, 50, false) == mortality(0, 50, false));
}

TEST_CASE("demography: adult mortality never falls from one bucket to the next") {
    Fixed64 previous = mortality(5, 50, false);  // the 5-14 bucket is the safest of life (20)
    CHECK(mortality(15, 50, false) < previous);  // 15
    previous = mortality(15, 50, false);
    for (int age = 15; age <= 110; ++age) {
        const Fixed64 now = mortality(age, 50, false);
        CAPTURE(age);
        CHECK(now >= previous);
        previous = now;
    }
}

TEST_CASE("demography: nobles get x0.8 on child buckets only") {
    CHECK(mortality(0, 50, true) == fixed(176));
    CHECK(mortality(1, 50, true) == fixed(56));
    CHECK(mortality(14, 50, true) == fixed(16));
    CHECK(mortality(15, 50, true) == fixed(15));
    CHECK(mortality(60, 50, true) == fixed(110));
    CHECK(has_term(mortality_permille(defaults(), 0, health(50), true), "demography.mortality.noble_child"));
    CHECK_FALSE(
        has_term(mortality_permille(defaults(), 0, health(50), false), "demography.mortality.noble_child"));
    CHECK_FALSE(
        has_term(mortality_permille(defaults(), 30, health(50), true), "demography.mortality.noble_child"));
}

TEST_CASE("demography: health below 30 doubles mortality, 80 and above cuts it by a quarter") {
    CHECK(mortality(45, 29, false) == fixed(90));
    CHECK(mortality(45, 0, false) == fixed(90));
    CHECK(mortality(45, 30, false) == fixed(45));  // boundary: 30 is not "below 30"
    CHECK(mortality(45, 79, false) == fixed(45));
    CHECK(mortality(60, 80, false) == Fixed64::from_ratio(825, 10));  // 110 * 0.75 = 82.5
    CHECK(mortality(60, 100, false) == Fixed64::from_ratio(825, 10));
    // A health value just under 30 (Fixed32 has sub-integer resolution) still counts as below.
    CHECK(mortality_permille(defaults(), 45, Fixed32::from_ratio(2999, 100), false).total == fixed(90));
    CHECK(has_term(mortality_permille(defaults(), 45, health(10), false), "demography.mortality.low_health"));
    CHECK(
        has_term(mortality_permille(defaults(), 45, health(90), false), "demography.mortality.high_health"));
}

TEST_CASE("demography: sick people die at least as often as healthy ones at every age") {
    bool ok = true;
    for (int age = 0; age <= 110; ++age) {
        for (int noble = 0; noble < 2; ++noble) {
            const Fixed64 sick = mortality(age, 10, noble != 0);
            const Fixed64 mid = mortality(age, 50, noble != 0);
            const Fixed64 fit = mortality(age, 95, noble != 0);
            ok = ok && sick >= mid && mid >= fit;
        }
    }
    CHECK(ok);
}

TEST_CASE("demography: trait multiplier scales mortality and is clamped") {
    const auto with_traits = [](int permille) {
        return mortality_permille(defaults(), 45, health(50), false, permille);
    };
    CHECK(with_traits(1000).total == fixed(45));
    CHECK(with_traits(2000).total == fixed(90));
    CHECK(with_traits(500).total == Fixed64::from_ratio(225, 10));
    CHECK(with_traits(0).total == fixed(0));
    CHECK(with_traits(-500).total == fixed(0));         // negative multiplier is treated as 0
    CHECK(with_traits(1'000'000).total == fixed(450));  // clamped to x10
    CHECK_FALSE(has_term(with_traits(1000), "demography.mortality.traits"));
    CHECK(has_term(with_traits(2000), "demography.mortality.traits"));
}

TEST_CASE("demography: mortality stays within 0...1000, the clamp is recorded as a term") {
    const auto extreme = mortality_permille(defaults(), 85, health(0), false, 10000);  // 450 * 2 * 10
    CHECK(extreme.total == fixed(1000));
    CHECK(has_term(extreme, "demography.mortality.clamp"));
    CHECK(extreme.sum_of_terms() == extreme.total);
    CHECK(extreme.terms[extreme.terms.size() - 1].value.is_negative());
}

TEST_CASE("demography: mortality breakdown sums to the result over a sweep") {
    const int traits[] = {0, 300, 750, 1000, 1500, 4000, 10000, 99999, -10};
    bool ok = true;
    int cases = 0;
    for (int age = -2; age <= 125; ++age) {
        for (int hp = 0; hp <= 100; hp += 5) {
            for (int noble = 0; noble < 2; ++noble) {
                for (const int trait : traits) {
                    const auto b = mortality_permille(defaults(), age, health(hp), noble != 0, trait);
                    ok = ok && well_formed(b) && b.terms[0].key == "demography.mortality.base";
                    ++cases;
                }
            }
        }
    }
    CHECK(ok);
    CHECK(cases > 10000);
}

TEST_CASE("demography: breakdown starts with the base bucket and orders terms noble, health, traits") {
    const auto b = mortality_permille(defaults(), 2, health(10), true, 1500);
    REQUIRE(b.terms.size() == 4);
    CHECK(b.terms[0].key == "demography.mortality.base");
    CHECK(b.terms[0].value == fixed(70));
    CHECK(b.terms[1].key == "demography.mortality.noble_child");
    CHECK(b.terms[1].factor_permille == 800);
    CHECK(b.terms[1].value == fixed(-14));  // 70 -> 56
    CHECK(b.terms[2].key == "demography.mortality.low_health");
    CHECK(b.terms[2].factor_permille == 2000);
    CHECK(b.terms[2].value == fixed(56));  // 56 -> 112
    CHECK(b.terms[3].key == "demography.mortality.traits");
    CHECK(b.terms[3].value == fixed(56));  // 112 -> 168
    CHECK(b.total == fixed(168));
    CHECK(b.rounded_permille() == 168);
}

TEST_CASE("demography: conception follows the GDD 01 3.3 age factors") {
    const Fixed64 base = fixed(12);
    CHECK(conception(couple(10)) == fixed(0));
    CHECK(conception(couple(14)) == fixed(0));
    CHECK(conception(couple(15)) == base.mul_permille(800));
    CHECK(conception(couple(19)) == base.mul_permille(800));
    CHECK(conception(couple(20)) == base);
    CHECK(conception(couple(29)) == base);
    CHECK(conception(couple(30)) == base.mul_permille(800));
    CHECK(conception(couple(34)) == base.mul_permille(800));
    CHECK(conception(couple(35)) == base.mul_permille(500));
    CHECK(conception(couple(39)) == base.mul_permille(500));
    CHECK(conception(couple(40)) == base.mul_permille(200));
    CHECK(conception(couple(44)) == base.mul_permille(200));
    CHECK(conception(couple(45)) == fixed(0));
    CHECK(conception(couple(70)) == fixed(0));
    CHECK(conception(couple(-4)) == fixed(0));
}

TEST_CASE("demography: fertility falls monotonically after the peak") {
    Fixed64 previous = conception(couple(20));
    for (int age = 21; age <= 60; ++age) {
        const Fixed64 now = conception(couple(age));
        CAPTURE(age);
        CHECK(now <= previous);
        previous = now;
    }
}

TEST_CASE("demography: conception health, separation and child cap") {
    CHECK(conception(couple(25, 29)) == fixed(6));  // x0.5
    CHECK(conception(couple(25, 30)) == fixed(12));
    CHECK(conception(couple(25, 79)) == fixed(12));
    CHECK(conception(couple(25, 80)) == Fixed64::from_ratio(132, 10));  // x1.1

    ConceptionInput apart = couple(25);
    apart.together = false;
    CHECK(conception(apart) == fixed(0));
    CHECK(has_term(conception_permille(defaults(), apart), "demography.conception.apart"));

    ConceptionInput many = couple(25);
    many.children_born = 13;
    CHECK(conception(many) == fixed(12));
    many.children_born = 14;
    CHECK(conception(many) == fixed(0));
    CHECK(has_term(conception_permille(defaults(), many), "demography.conception.child_cap"));
    many.children_born = 40;
    CHECK(conception(many) == fixed(0));

    ConceptionInput traits = couple(25);
    traits.trait_permille = 1500;
    CHECK(conception(traits) == fixed(18));
    traits.trait_permille = 0;
    CHECK(conception(traits) == fixed(0));
}

TEST_CASE("demography: conception breakdown sums to the result over a sweep") {
    bool ok = true;
    int cases = 0;
    for (int age = -2; age <= 70; ++age) {
        for (int hp = 0; hp <= 100; hp += 10) {
            for (int together = 0; together < 2; ++together) {
                for (const int children : {0, 5, 13, 14, 20}) {
                    for (const int trait : {0, 500, 1000, 3000, 99999}) {
                        ConceptionInput input = couple(age, hp);
                        input.together = together != 0;
                        input.children_born = children;
                        input.trait_permille = trait;
                        const auto b = conception_permille(defaults(), input);
                        ok = ok && well_formed(b) && b.terms[0].key == "demography.conception.base";
                        ++cases;
                    }
                }
            }
        }
    }
    CHECK(ok);
    CHECK(cases > 10000);
}

TEST_CASE("demography: conception breakdown lists each applied factor") {
    ConceptionInput input = couple(37, 90);
    input.trait_permille = 1200;
    const auto b = conception_permille(defaults(), input);
    REQUIRE(b.terms.size() == 4);
    CHECK(b.terms[0].key == "demography.conception.base");
    CHECK(b.terms[1].key == "demography.conception.age");
    CHECK(b.terms[1].factor_permille == 500);
    CHECK(b.terms[2].key == "demography.conception.high_health");
    CHECK(b.terms[3].key == "demography.conception.traits");
    CHECK(b.sum_of_terms() == b.total);
    CHECK(b.total > fixed(0));
    CHECK(b.total < fixed(12));
}

TEST_CASE("demography: childbirth risk stays between 3% and 6%") {
    const auto risk = [](int hp, bool physician) {
        return childbirth_risk_permille(defaults(), health(hp), physician).total;
    };
    CHECK(risk(50, false) == fixed(50));
    CHECK(risk(90, false) == fixed(40));
    CHECK(risk(10, false) == fixed(60));
    CHECK(risk(50, true) == fixed(40));
    CHECK(risk(90, true) == fixed(30));
    CHECK(risk(10, true) == fixed(50));

    bool ok = true;
    for (int hp = 0; hp <= 100; ++hp) {
        for (int physician = 0; physician < 2; ++physician) {
            const auto b = childbirth_risk_permille(defaults(), health(hp), physician != 0);
            ok = ok && well_formed(b) && b.total >= fixed(30) && b.total <= fixed(60);
        }
    }
    CHECK(ok);
    // More health never raises the risk; a physician never raises it.
    CHECK(risk(95, false) <= risk(50, false));
    CHECK(risk(50, false) <= risk(5, false));
    CHECK(risk(5, true) <= risk(5, false));
}

TEST_CASE("demography: sex ratio and life stages") {
    CHECK(male_birth_permille(defaults()) == 512);  // 105 / 205

    const auto stage = [](int age) { return life_stage(defaults(), age); };
    CHECK(stage(0) == LifeStage::Child);
    CHECK(stage(6) == LifeStage::Child);
    CHECK(stage(7) == LifeStage::Adolescent);
    CHECK(stage(15) == LifeStage::Adolescent);
    CHECK(stage(16) == LifeStage::Prime);
    CHECK(stage(50) == LifeStage::Prime);
    CHECK(stage(51) == LifeStage::Mature);
    CHECK(stage(65) == LifeStage::Mature);
    CHECK(stage(66) == LifeStage::Elder);
    CHECK(stage(110) == LifeStage::Elder);
    CHECK(stage(-1) == LifeStage::Child);

    CHECK(defaults().marriage_age_male == 16);
    CHECK(defaults().marriage_age_female == 15);
    CHECK(defaults().max_children_per_woman == 14);
    CHECK(defaults().pregnancy_months == 9);
    CHECK(defaults().base_conception_permille == 12);
}

TEST_CASE("demography: results are deterministic") {
    std::uint64_t checksum_a = 0;
    std::uint64_t checksum_b = 0;
    for (std::uint64_t* sum : {&checksum_a, &checksum_b}) {
        for (int age = 0; age <= 100; ++age) {
            for (int hp = 0; hp <= 100; hp += 7) {
                const auto m =
                    mortality_permille(defaults(), age, health(hp), (age + hp) % 2 == 0, 1000 + age * 3);
                const auto c = conception_permille(defaults(), couple(age, hp));
                *sum = *sum * 1099511628211ULL + static_cast<std::uint64_t>(m.total.raw());
                *sum = *sum * 1099511628211ULL + static_cast<std::uint64_t>(c.total.raw());
                *sum = *sum * 1099511628211ULL + m.terms.size() * 31 + c.terms.size();
            }
        }
    }
    CHECK(checksum_a == checksum_b);
    CHECK(checksum_a != 0);
    CHECK(same(mortality_permille(defaults(), 33, health(41), true),
               mortality_permille(defaults(), 33, health(41), true)));
    CHECK(same(conception_permille(defaults(), couple(27, 66)),
               conception_permille(defaults(), couple(27, 66))));
}

TEST_CASE("demography: validation rejects broken age tables") {
    require_error(broken([](DemographyRules& r) { r.mortality.count = 0; }), RulesErrorCode::MortalityTable);
    require_error(broken([](DemographyRules& r) { r.mortality.count = 17; }), RulesErrorCode::MortalityTable);
    require_error(broken([](DemographyRules& r) { r.mortality.buckets[0].from_age = 1; }),
                  RulesErrorCode::MortalityTable);  // hole at age 0
    require_error(broken([](DemographyRules& r) { r.mortality.buckets[3].from_age = 5; }),
                  RulesErrorCode::MortalityTable);  // equal to the previous start
    require_error(broken([](DemographyRules& r) { r.mortality.buckets[3].from_age = 2; }),
                  RulesErrorCode::MortalityTable);  // goes backwards
    require_error(broken([](DemographyRules& r) { r.mortality.buckets[2].value = 1001; }),
                  RulesErrorCode::MortalityTable);
    require_error(broken([](DemographyRules& r) { r.mortality.buckets[2].value = -1; }),
                  RulesErrorCode::MortalityTable);
    require_error(broken([](DemographyRules& r) { r.age_factor.count = 0; }), RulesErrorCode::AgeFactorTable);
    require_error(broken([](DemographyRules& r) { r.age_factor.buckets[0].from_age = 3; }),
                  RulesErrorCode::AgeFactorTable);
    require_error(broken([](DemographyRules& r) { r.age_factor.buckets[2].from_age = 15; }),
                  RulesErrorCode::AgeFactorTable);
    require_error(broken([](DemographyRules& r) { r.age_factor.buckets[1].value = 10001; }),
                  RulesErrorCode::AgeFactorTable);
}

TEST_CASE("demography: validation rejects out-of-range parameters") {
    require_error(broken([](DemographyRules& r) { r.noble_child_multiplier_permille = -1; }),
                  RulesErrorCode::Multiplier);
    require_error(broken([](DemographyRules& r) { r.low_health_mortality_permille = 10001; }),
                  RulesErrorCode::Multiplier);
    require_error(broken([](DemographyRules& r) { r.high_health_fertility_permille = 20000; }),
                  RulesErrorCode::Multiplier);
    require_error(broken([](DemographyRules& r) { r.noble_child_age_limit = -1; }),
                  RulesErrorCode::Multiplier);

    require_error(broken([](DemographyRules& r) { r.low_health_below = 80; }),
                  RulesErrorCode::HealthThreshold);
    require_error(broken([](DemographyRules& r) { r.low_health_below = 90; }),
                  RulesErrorCode::HealthThreshold);
    require_error(broken([](DemographyRules& r) { r.high_health_from = 101; }),
                  RulesErrorCode::HealthThreshold);
    require_error(broken([](DemographyRules& r) { r.low_health_below = -1; }),
                  RulesErrorCode::HealthThreshold);

    require_error(broken([](DemographyRules& r) { r.base_conception_permille = 1001; }),
                  RulesErrorCode::Conception);
    require_error(broken([](DemographyRules& r) { r.base_conception_permille = -1; }),
                  RulesErrorCode::Conception);
    require_error(broken([](DemographyRules& r) { r.max_children_per_woman = 0; }),
                  RulesErrorCode::Conception);
    require_error(broken([](DemographyRules& r) { r.pregnancy_months = 0; }), RulesErrorCode::Conception);
    require_error(broken([](DemographyRules& r) { r.pregnancy_months = 25; }), RulesErrorCode::Conception);

    require_error(broken([](DemographyRules& r) { r.boys_per_100_girls = 0; }), RulesErrorCode::Birth);
    require_error(broken([](DemographyRules& r) { r.childbirth_risk_base_permille = 1001; }),
                  RulesErrorCode::Birth);
    require_error(broken([](DemographyRules& r) { r.childbirth_risk_high_health_delta_permille = -60; }),
                  RulesErrorCode::Birth);  // base 50 - 60 - 10 < 0
    require_error(broken([](DemographyRules& r) { r.childbirth_risk_low_health_delta_permille = 951; }),
                  RulesErrorCode::Birth);  // base 50 + 951 > 1000
}

TEST_CASE("demography: validation rejects inconsistent life stages") {
    require_error(broken([](DemographyRules& r) { r.adolescent_from = 0; }), RulesErrorCode::LifeStages);
    require_error(broken([](DemographyRules& r) { r.prime_from = 7; }), RulesErrorCode::LifeStages);
    require_error(broken([](DemographyRules& r) { r.mature_from = 10; }), RulesErrorCode::LifeStages);
    require_error(broken([](DemographyRules& r) { r.elder_from = 51; }), RulesErrorCode::LifeStages);
    require_error(broken([](DemographyRules& r) { r.marriage_age_male = 5; }), RulesErrorCode::LifeStages);
    require_error(broken([](DemographyRules& r) { r.marriage_age_female = 51; }), RulesErrorCode::LifeStages);
}

TEST_CASE("demography: validation accepts a tuned but consistent copy") {
    DemographyRules rules = kDefaultDemography;
    rules.mortality.buckets[0].value = 1000;
    rules.mortality.buckets[8].value = 0;
    rules.base_conception_permille = 1000;
    rules.max_children_per_woman = 1;
    rules.boys_per_100_girls = 100;
    CHECK(validate(rules).has_value());
    CHECK(male_birth_permille(rules) == 500);
    // The functions stay in bounds for any validated rules.
    const auto b = conception_permille(rules, couple(25));
    CHECK(well_formed(b));
}

TEST_CASE("demography: AgeTable lookup handles bucket edges") {
    const AgeTable& table = kDefaultDemography.mortality;
    CHECK(table.bucket_index(0) == 0);
    CHECK(table.bucket_index(1) == 1);
    CHECK(table.bucket_index(4) == 1);
    CHECK(table.bucket_index(5) == 2);
    CHECK(table.bucket_index(80) == 8);
    CHECK(table.bucket_index(1000) == 8);
    CHECK(table.lookup(-10) == 220);
}
