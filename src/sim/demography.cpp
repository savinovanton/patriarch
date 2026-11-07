#include "pat/sim/demography.hpp"

#include <algorithm>
#include <format>
#include <utility>

namespace pat::sim {

using core::Fixed32;
using core::Fixed64;

namespace {

constexpr std::int32_t kPermilleOne = 1000;
constexpr std::int32_t kMaxHealth = 100;

/// Builds a breakdown step by step; every change to the running value becomes one term, so the
/// terms always add up to the total.
class BreakdownBuilder {
public:
    void base(std::string_view key, Fixed64 value) {
        running_ = value;
        out_.terms.push_back({key, value, 0});
    }

    /// Multiplies the running value by `permille / 1000` and records the change. A neutral
    /// multiplier records nothing.
    void factor(std::string_view key, std::int32_t permille) {
        if (permille == kPermilleOne) {
            return;
        }
        const Fixed64 next = running_.mul_permille(permille);
        out_.terms.push_back({key, next - running_, permille});
        running_ = next;
    }

    /// Adds `points` per-mille points; a zero change records nothing.
    void add(std::string_view key, std::int32_t points) {
        if (points == 0) {
            return;
        }
        const Fixed64 delta = Fixed64::from_int(points);
        out_.terms.push_back({key, delta, 0});
        running_ += delta;
    }

    /// Clamps to 0...1000, recording the correction (if any) as a term, and returns the result.
    ProbabilityBreakdown finish(std::string_view clamp_key) {
        const Fixed64 lo = Fixed64::zero();
        const Fixed64 hi = Fixed64::from_int(kPermilleOne);
        const Fixed64 clamped = core::clamp(running_, lo, hi);
        if (clamped != running_) {
            out_.terms.push_back({clamp_key, clamped - running_, 0});
            running_ = clamped;
        }
        out_.total = running_;
        return std::move(out_);
    }

private:
    ProbabilityBreakdown out_;
    Fixed64 running_;
};

std::int32_t clamp_multiplier(std::int32_t permille) noexcept {
    return std::clamp<std::int32_t>(permille, 0, DemographyRules::kMaxMultiplierPermille);
}

bool health_below(Fixed32 health, std::int32_t threshold) noexcept {
    return health < Fixed32::from_int(threshold);
}

bool health_at_least(Fixed32 health, std::int32_t threshold) noexcept {
    return !(health < Fixed32::from_int(threshold));
}

RulesError make_error(RulesErrorCode code, std::string message) {
    return RulesError{code, std::move(message)};
}

using Check = core::Result<void, RulesError>;

Check check_age_table(const AgeTable& table,
                      std::int32_t max_value,
                      RulesErrorCode code,
                      std::string_view name) {
    if (table.count < 1 || table.count > static_cast<std::int32_t>(AgeTable::kMaxBuckets)) {
        return Check::err(make_error(
            code,
            std::format("{}: bucket count {} outside 1...{}", name, table.count, AgeTable::kMaxBuckets)));
    }
    if (table.buckets[0].from_age != 0) {
        return Check::err(make_error(
            code,
            std::format("{}: first bucket starts at age {}, not 0 (gap)", name, table.buckets[0].from_age)));
    }
    for (std::int32_t i = 0; i < table.count; ++i) {
        const AgeBucket& bucket = table.buckets[static_cast<std::size_t>(i)];
        if (i > 0 && bucket.from_age <= table.buckets[static_cast<std::size_t>(i - 1)].from_age) {
            return Check::err(make_error(
                code,
                std::format("{}: bucket {} (from age {}) does not increase", name, i, bucket.from_age)));
        }
        if (bucket.value < 0 || bucket.value > max_value) {
            return Check::err(make_error(
                code,
                std::format("{}: bucket {} value {} outside 0...{}", name, i, bucket.value, max_value)));
        }
    }
    return Check::ok();
}

Check check_multiplier(std::int32_t permille, std::string_view name) {
    if (permille < 0 || permille > DemographyRules::kMaxMultiplierPermille) {
        return Check::err(make_error(
            RulesErrorCode::Multiplier,
            std::format("{}: {} outside 0...{}", name, permille, DemographyRules::kMaxMultiplierPermille)));
    }
    return Check::ok();
}

}  // namespace

// ---- ProbabilityBreakdown / AgeTable -----------------------------------------------------------

Fixed64 ProbabilityBreakdown::sum_of_terms() const noexcept {
    Fixed64 sum = Fixed64::zero();
    for (const ProbabilityTerm& term : terms) {
        sum += term.value;
    }
    return sum;
}

std::int32_t AgeTable::bucket_index(std::int32_t age) const noexcept {
    std::int32_t index = 0;
    for (std::int32_t i = 1; i < count; ++i) {
        if (buckets[static_cast<std::size_t>(i)].from_age > age) {
            break;
        }
        index = i;
    }
    return index;
}

std::int32_t AgeTable::lookup(std::int32_t age) const noexcept {
    return buckets[static_cast<std::size_t>(bucket_index(age))].value;
}

// ---- validation --------------------------------------------------------------------------------

core::Result<void, RulesError> validate(const DemographyRules& r) {
    if (auto res = check_age_table(r.mortality, kPermilleOne, RulesErrorCode::MortalityTable, "mortality");
        !res) {
        return res;
    }
    if (auto res = check_age_table(r.age_factor,
                                   DemographyRules::kMaxMultiplierPermille,
                                   RulesErrorCode::AgeFactorTable,
                                   "age_factor");
        !res) {
        return res;
    }

    for (const auto& [value, name] : {
             std::pair{r.noble_child_multiplier_permille, "noble_child_multiplier"},
             std::pair{r.low_health_mortality_permille, "low_health_mortality"},
             std::pair{r.high_health_mortality_permille, "high_health_mortality"},
             std::pair{r.low_health_fertility_permille, "low_health_fertility"},
             std::pair{r.high_health_fertility_permille, "high_health_fertility"},
         }) {
        if (auto res = check_multiplier(value, name); !res) {
            return res;
        }
    }

    if (r.noble_child_age_limit < 0 || r.noble_child_age_limit > 200) {
        return Check::err(
            make_error(RulesErrorCode::Multiplier,
                       std::format("noble_child_age_limit {} outside 0...200", r.noble_child_age_limit)));
    }

    if (r.low_health_below < 0 || r.high_health_from > kMaxHealth ||
        r.low_health_below >= r.high_health_from) {
        return Check::err(
            make_error(RulesErrorCode::HealthThreshold,
                       std::format("health thresholds low<{} high>={} must satisfy 0 <= low < high <= {}",
                                   r.low_health_below,
                                   r.high_health_from,
                                   kMaxHealth)));
    }

    if (r.base_conception_permille < 0 || r.base_conception_permille > kPermilleOne) {
        return Check::err(make_error(RulesErrorCode::Conception,
                                     std::format("base_conception_permille {} outside 0...{}",
                                                 r.base_conception_permille,
                                                 kPermilleOne)));
    }
    if (r.max_children_per_woman < 1) {
        return Check::err(make_error(
            RulesErrorCode::Conception,
            std::format("max_children_per_woman {} must be at least 1", r.max_children_per_woman)));
    }
    if (r.pregnancy_months < 1 || r.pregnancy_months > 24) {
        return Check::err(make_error(RulesErrorCode::Conception,
                                     std::format("pregnancy_months {} outside 1...24", r.pregnancy_months)));
    }

    if (r.boys_per_100_girls < 1 || r.boys_per_100_girls > 10000) {
        return Check::err(
            make_error(RulesErrorCode::Birth,
                       std::format("boys_per_100_girls {} outside 1...10000", r.boys_per_100_girls)));
    }
    const std::int32_t risk_high = r.childbirth_risk_base_permille +
                                   std::min(0, r.childbirth_risk_high_health_delta_permille) +
                                   std::min(0, r.childbirth_risk_physician_delta_permille);
    const std::int32_t risk_low =
        r.childbirth_risk_base_permille + std::max(0, r.childbirth_risk_low_health_delta_permille);
    if (r.childbirth_risk_base_permille < 0 || r.childbirth_risk_base_permille > kPermilleOne ||
        risk_high < 0 || risk_low > kPermilleOne) {
        return Check::err(
            make_error(RulesErrorCode::Birth, "childbirth risk (base plus deltas) leaves 0...1000"));
    }

    if (r.adolescent_from <= 0 || r.prime_from <= r.adolescent_from || r.mature_from <= r.prime_from ||
        r.elder_from <= r.mature_from) {
        return Check::err(make_error(
            RulesErrorCode::LifeStages,
            std::format("stage ages {}/{}/{}/{} must satisfy 0 < adolescent < prime < mature < elder",
                        r.adolescent_from,
                        r.prime_from,
                        r.mature_from,
                        r.elder_from)));
    }
    for (const auto& [age, name] : {
             std::pair{r.marriage_age_male, "marriage_age_male"},
             std::pair{r.marriage_age_female, "marriage_age_female"},
         }) {
        if (age < r.adolescent_from || age >= r.mature_from) {
            return Check::err(make_error(
                RulesErrorCode::LifeStages,
                std::format("{} {} must lie in [{}, {})", name, age, r.adolescent_from, r.mature_from)));
        }
    }
    return Check::ok();
}

// ---- pure functions ----------------------------------------------------------------------------

ProbabilityBreakdown mortality_permille(
    const DemographyRules& rules, std::int32_t age, Fixed32 health, bool noble, std::int32_t trait_permille) {
    const std::int32_t years = std::max<std::int32_t>(age, 0);

    BreakdownBuilder builder;
    builder.base("demography.mortality.base", Fixed64::from_int(rules.mortality.lookup(years)));
    if (noble && years < rules.noble_child_age_limit) {
        builder.factor("demography.mortality.noble_child", rules.noble_child_multiplier_permille);
    }
    if (health_below(health, rules.low_health_below)) {
        builder.factor("demography.mortality.low_health", rules.low_health_mortality_permille);
    } else if (health_at_least(health, rules.high_health_from)) {
        builder.factor("demography.mortality.high_health", rules.high_health_mortality_permille);
    }
    builder.factor("demography.mortality.traits", clamp_multiplier(trait_permille));
    return builder.finish("demography.mortality.clamp");
}

ProbabilityBreakdown conception_permille(const DemographyRules& rules, const ConceptionInput& input) {
    const std::int32_t years = std::max<std::int32_t>(input.wife_age, 0);

    BreakdownBuilder builder;
    builder.base("demography.conception.base", Fixed64::from_int(rules.base_conception_permille));
    builder.factor("demography.conception.age", rules.age_factor.lookup(years));
    if (health_below(input.wife_health, rules.low_health_below)) {
        builder.factor("demography.conception.low_health", rules.low_health_fertility_permille);
    } else if (health_at_least(input.wife_health, rules.high_health_from)) {
        builder.factor("demography.conception.high_health", rules.high_health_fertility_permille);
    }
    builder.factor("demography.conception.traits", clamp_multiplier(input.trait_permille));
    if (!input.together) {
        builder.factor("demography.conception.apart", 0);
    }
    if (input.children_born >= rules.max_children_per_woman) {
        builder.factor("demography.conception.child_cap", 0);
    }
    return builder.finish("demography.conception.clamp");
}

ProbabilityBreakdown
childbirth_risk_permille(const DemographyRules& rules, Fixed32 health, bool physician_at_court) {
    BreakdownBuilder builder;
    builder.base("demography.childbirth.base", Fixed64::from_int(rules.childbirth_risk_base_permille));
    if (health_below(health, rules.low_health_below)) {
        builder.add("demography.childbirth.low_health", rules.childbirth_risk_low_health_delta_permille);
    } else if (health_at_least(health, rules.high_health_from)) {
        builder.add("demography.childbirth.high_health", rules.childbirth_risk_high_health_delta_permille);
    }
    if (physician_at_court) {
        builder.add("demography.childbirth.physician", rules.childbirth_risk_physician_delta_permille);
    }
    return builder.finish("demography.childbirth.clamp");
}

std::int32_t male_birth_permille(const DemographyRules& rules) noexcept {
    const std::int64_t boys = rules.boys_per_100_girls;
    return static_cast<std::int32_t>(boys * kPermilleOne / (boys + 100));
}

LifeStage life_stage(const DemographyRules& rules, std::int32_t age) noexcept {
    if (age < rules.adolescent_from) {
        return LifeStage::Child;
    }
    if (age < rules.prime_from) {
        return LifeStage::Adolescent;
    }
    if (age < rules.mature_from) {
        return LifeStage::Prime;
    }
    if (age < rules.elder_from) {
        return LifeStage::Mature;
    }
    return LifeStage::Elder;
}

}  // namespace pat::sim
