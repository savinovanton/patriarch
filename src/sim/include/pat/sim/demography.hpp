#pragma once

#include "pat/core/fixed.hpp"
#include "pat/core/result.hpp"
#include "pat/core/static_vec.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// Demography rules (E7-T06, GDD 01 section 3): the numbers that turn a character's age, health and
// traits into yearly mortality and monthly conception probabilities.
//
// This header is pure data plus pure functions: nothing here reads or writes the World, so the same
// rules can be built from code (kDefaultDemography), from `.pd` files (E7-T07) and from tests.
// There is no floating point anywhere: probabilities are per-mille values (0...1000) held in Fixed64,
// multipliers are integers in per-mille of one (1000 = x1.0, 800 = x0.8).
//
// Every probability function returns a ProbabilityBreakdown: the base value followed by signed terms
// whose sum is exactly the result. The UI turns it into an explanation tree (FR-UI-3) without any
// hand-written text per case.
//
// ALL numbers in kDefaultDemography are [predl.] placeholders from the GDD; they are meant to be
// tuned (E7-T19) and are replaced from data files by E7-T07.

namespace pat::sim {

// ---- result with explanation -------------------------------------------------------------------

/// One line of a breakdown. The first term of a breakdown is the absolute base value; every later
/// term is a signed change in per-mille points. `key` is a stable localisation/tooltip key.
struct ProbabilityTerm {
    std::string_view key;
    core::Fixed64 value;
    /// Multiplier (per-mille of one) that produced this term, or 0 for the base, additive and
    /// clamp terms; lets a tooltip show "x0.8" next to the delta.
    std::int32_t factor_permille = 0;

    [[nodiscard]] bool operator==(const ProbabilityTerm&) const = default;
};

/// A probability in per-mille (0...1000) with the terms that add up to it. Invariant:
/// sum of `terms[i].value` == `total`, and 0 <= total <= 1000.
struct ProbabilityBreakdown {
    static constexpr std::size_t kMaxTerms = 10;

    core::StaticVec<ProbabilityTerm, kMaxTerms> terms;
    core::Fixed64 total;

    [[nodiscard]] core::Fixed64 sum_of_terms() const noexcept;
    /// total rounded half away from zero to whole per-mille (for display).
    [[nodiscard]] std::int32_t rounded_permille() const noexcept {
        return static_cast<std::int32_t>(total.round_int());
    }
};

// ---- rules -------------------------------------------------------------------------------------

/// A step table over age: `value` applies from `from_age` (inclusive) up to the next entry's
/// `from_age` (exclusive); the last entry is open-ended. The first entry starts at age 0, so
/// there are no holes by construction (validate() checks the entries are strictly increasing).
struct AgeBucket {
    std::int32_t from_age = 0;
    std::int32_t value = 0;
};

struct AgeTable {
    static constexpr std::size_t kMaxBuckets = 16;

    std::array<AgeBucket, kMaxBuckets> buckets{};
    std::int32_t count = 0;

    /// Value of the bucket containing `age` (negative ages count as 0). Requires count >= 1.
    [[nodiscard]] std::int32_t lookup(std::int32_t age) const noexcept;
    /// Index of the bucket containing `age`. Requires count >= 1.
    [[nodiscard]] std::int32_t bucket_index(std::int32_t age) const noexcept;
};

/// Life stages (GDD 01 section 3.1).
enum class LifeStage : std::uint8_t {
    Child,       ///< infancy and early childhood
    Adolescent,  ///< education years
    Prime,       ///< adult, peak fertility and health
    Mature,      ///< rising illness risk, falling fertility
    Elder,       ///< fast health decline, high mortality
};

struct DemographyRules {
    // Mortality (GDD 01 section 3.2): base yearly death chance by age bucket, per mille.
    AgeTable mortality;
    /// Noble (full-detail) characters younger than this get the child multiplier below.
    std::int32_t noble_child_age_limit = 0;
    std::int32_t noble_child_multiplier_permille = 1000;
    /// Health below `low_health_below` multiplies mortality by `low_health_mortality_permille`;
    /// health at or above `high_health_from` multiplies it by `high_health_mortality_permille`.
    std::int32_t low_health_below = 0;
    std::int32_t high_health_from = 100;
    std::int32_t low_health_mortality_permille = 1000;
    std::int32_t high_health_mortality_permille = 1000;

    // Conception (GDD 01 section 3.3): P_month = base x age x health x trait x together.
    std::int32_t base_conception_permille = 0;  ///< per month
    AgeTable age_factor;                        ///< wife's age -> multiplier in per-mille of one
    std::int32_t low_health_fertility_permille = 1000;
    std::int32_t high_health_fertility_permille = 1000;
    std::int32_t max_children_per_woman = 1;
    std::int32_t pregnancy_months = 9;

    // Birth (GDD 01 section 3.3).
    std::int32_t boys_per_100_girls = 100;
    std::int32_t childbirth_risk_base_permille = 0;  ///< mother's death risk per birth
    std::int32_t childbirth_risk_high_health_delta_permille = 0;
    std::int32_t childbirth_risk_low_health_delta_permille = 0;
    std::int32_t childbirth_risk_physician_delta_permille = 0;

    // Life stages (GDD 01 section 3.1): first age of each stage.
    std::int32_t adolescent_from = 0;
    std::int32_t prime_from = 0;
    std::int32_t mature_from = 0;
    std::int32_t elder_from = 0;
    std::int32_t marriage_age_male = 0;
    std::int32_t marriage_age_female = 0;

    /// Largest accepted multiplier (per-mille of one); keeps Fixed32 arithmetic far from overflow.
    static constexpr std::int32_t kMaxMultiplierPermille = 10000;
};

/// The one table of default values. Every number is a [predl.] placeholder from GDD 01 section 3;
/// E7-T07 reproduces exactly these values in `data/balance/demography.pd`.
inline constexpr DemographyRules kDefaultDemography{
    // GDD 01 3.2: base yearly mortality, per mille [predl.]
    .mortality = {.buckets = {{
                      {0, 220},   // first year of life (includes miscarriage/stillbirth share)
                      {1, 70},    // 1-4
                      {5, 20},    // 5-14
                      {15, 15},   // 15-29
                      {30, 22},   // 30-44
                      {45, 45},   // 45-59
                      {60, 110},  // 60-69
                      {70, 240},  // 70-79
                      {80, 450},  // 80+
                  }},
                  .count = 9},
    // GDD 01 3.2: nobles x0.8 on the child buckets (ages 0-14) [predl.]
    .noble_child_age_limit = 15,
    .noble_child_multiplier_permille = 800,
    // GDD 01 3.2: health < 30 doubles the chance, health >= 80 lowers it by 25% [predl.]
    .low_health_below = 30,
    .high_health_from = 80,
    .low_health_mortality_permille = 2000,
    .high_health_mortality_permille = 750,

    // GDD 01 3.3: base_conception ~ 12 per mille per month [predl.]
    .base_conception_permille = 12,
    // GDD 01 3.3: age_factor 15-19: 0.8; 20-29: 1.0; 30-34: 0.8; 35-39: 0.5; 40-44: 0.2; 45+: 0.0
    // (below 15 the wife cannot conceive: GDD 01 3.1 minimum marriage age 15) [predl.]
    .age_factor = {.buckets = {{
                       {0, 0},
                       {15, 800},
                       {20, 1000},
                       {30, 800},
                       {35, 500},
                       {40, 200},
                       {45, 0},
                   }},
                   .count = 7},
    // GDD 01 3.3 names a health_factor but gives no numbers; these two are this task's own
    // placeholders: a sickly wife halves the chance, a robust one adds 10% [predl.]
    .low_health_fertility_permille = 500,
    .high_health_fertility_permille = 1100,
    // GDD 01 3.3: hard cap ~14 children per woman [predl.]; "births by a 9-month timer"
    .max_children_per_woman = 14,
    .pregnancy_months = 9,

    // GDD 01 3.3: 105 boys per 100 girls [predl.]
    .boys_per_100_girls = 105,
    // GDD 01 3.3: mother's death risk 3-6% per birth, lower with high health and a physician at
    // court. 50 +10 (low health) = 60 (6%); 50 -10 (high health) -10 (physician) = 30 (3%) [predl.]
    .childbirth_risk_base_permille = 50,
    .childbirth_risk_high_health_delta_permille = -10,
    .childbirth_risk_low_health_delta_permille = 10,
    .childbirth_risk_physician_delta_permille = -10,

    // GDD 01 3.1: 0-6 child, 7-15 adolescent, 16-50 prime, 51-65 mature, 66+ elder [predl.]
    .adolescent_from = 7,
    .prime_from = 16,
    .mature_from = 51,
    .elder_from = 66,
    // GDD 01 3.1: may marry from 16 (men) / 15 (women) [predl.]
    .marriage_age_male = 16,
    .marriage_age_female = 15,
};

// ---- validation --------------------------------------------------------------------------------

enum class RulesErrorCode : std::uint8_t {
    MortalityTable,   ///< empty, not starting at age 0, not strictly increasing, or value outside 0...1000
    AgeFactorTable,   ///< as above, factor outside 0...kMaxMultiplierPermille
    Multiplier,       ///< a multiplier outside 0...kMaxMultiplierPermille
    HealthThreshold,  ///< thresholds outside 0...100 or low >= high
    Conception,       ///< base probability outside 0...1000, bad child cap or pregnancy length
    Birth,            ///< sex ratio or childbirth risk out of range
    LifeStages,       ///< stage ages not strictly increasing, or marriage ages outside the stages
};

struct RulesError {
    RulesErrorCode code = RulesErrorCode::MortalityTable;
    std::string message;

    [[nodiscard]] bool operator==(const RulesError&) const = default;
};

/// Checks a rules instance for consistency: age tables start at 0, strictly increase and stay in
/// range (no holes), per-mille values stay within 0...1000, stage boundaries increase, marriage
/// ages fall inside the stages. Returns the first problem found.
[[nodiscard]] core::Result<void, RulesError> validate(const DemographyRules& rules);

// ---- pure functions ----------------------------------------------------------------------------

/// Yearly chance (per mille) that a character of `age` whole years dies of natural causes.
/// base bucket, then x noble multiplier (children only), x health multiplier, x `trait_permille`
/// (1000 = no trait effect; clamped to 0...kMaxMultiplierPermille), finally clamped to 0...1000.
/// Terms (stable keys): demography.mortality.base / .noble_child / .low_health / .high_health /
/// .traits / .clamp. Terms with a neutral multiplier are omitted.
[[nodiscard]] ProbabilityBreakdown mortality_permille(const DemographyRules& rules,
                                                      std::int32_t age,
                                                      core::Fixed32 health,
                                                      bool noble,
                                                      std::int32_t trait_permille = 1000);

/// Inputs of the monthly conception chance of one couple.
struct ConceptionInput {
    std::int32_t wife_age = 0;
    core::Fixed32 wife_health = core::Fixed32::from_int(50);
    std::int32_t trait_permille = 1000;  ///< combined trait multiplier of the couple (1000 = none)
    bool together = true;                ///< spouses live in the same place
    std::int32_t children_born = 0;      ///< children the wife has borne so far
};

/// Monthly conception chance (per mille): base x age factor x health factor x traits x together,
/// zero once `children_born` reaches the cap. Terms: demography.conception.base / .age / .low_health
/// / .high_health / .traits / .apart / .child_cap / .clamp.
[[nodiscard]] ProbabilityBreakdown conception_permille(const DemographyRules& rules,
                                                       const ConceptionInput& input);

/// Chance (per mille) that the mother dies in childbirth: base plus health and physician deltas,
/// clamped to 0...1000. Terms: demography.childbirth.base / .low_health / .high_health /
/// .physician / .clamp.
[[nodiscard]] ProbabilityBreakdown
childbirth_risk_permille(const DemographyRules& rules, core::Fixed32 health, bool physician_at_court);

/// Chance (per mille, integer) that a newborn is a boy: boys / (boys + 100) from the sex ratio.
[[nodiscard]] std::int32_t male_birth_permille(const DemographyRules& rules) noexcept;

[[nodiscard]] LifeStage life_stage(const DemographyRules& rules, std::int32_t age) noexcept;

}  // namespace pat::sim
