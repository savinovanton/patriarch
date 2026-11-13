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
inline constexpr std::size_t kLifeStageCount = 5;

/// How strained a character is (E7-T11): the thresholds of the stress scale. Ordered, so that a
/// system can tell a rise by comparing the levels before and after a step.
enum class StressLevel : std::uint8_t {
    Calm,      ///< below `stress_high_from`
    Strained,  ///< from `stress_high_from`: health suffers; a hook for events of E11
    Breaking,  ///< from `stress_critical_from`: breakdowns and illness; a hook for events of E11
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
    /// Crowding (E7-T12): the monthly chance is multiplied by a factor that is 1.0 while the number of
    /// living characters is at most `crowding_from_living` and falls linearly to
    /// `crowding_min_permille` at `crowding_to_living` (and stays there). A soft brake, not a hard
    /// cap: the population levels off instead of growing exponentially. 1000 turns it off.
    std::int32_t crowding_from_living = 0;
    std::int32_t crowding_to_living = 1;
    std::int32_t crowding_min_permille = 1000;
    /// Share of the pregnancies that end early in a miscarriage (somewhere in months 1 ... N-1).
    std::int32_t miscarriage_permille = 0;

    // Birth (GDD 01 section 3.3).
    std::int32_t boys_per_100_girls = 100;
    std::int32_t childbirth_risk_base_permille = 0;  ///< mother's death risk per birth
    std::int32_t childbirth_risk_high_health_delta_permille = 0;
    std::int32_t childbirth_risk_low_health_delta_permille = 0;
    std::int32_t childbirth_risk_physician_delta_permille = 0;
    /// Share of the births at term that are stillbirths.
    std::int32_t stillbirth_permille = 0;
    /// Health of a newborn (0...100) before traits.
    std::int32_t newborn_health = 0;
    /// Skills of a child at birth: this share (per mille) of the average of the living parents'
    /// skills, plus a spread of up to +-`skill_spread` skill points (GDD 01 section 3.4).
    std::int32_t skill_inherit_permille = 0;
    std::int32_t skill_spread = 0;

    // Life stages (GDD 01 section 3.1): first age of each stage.
    std::int32_t adolescent_from = 0;
    std::int32_t prime_from = 0;
    std::int32_t mature_from = 0;
    std::int32_t elder_from = 0;
    std::int32_t marriage_age_male = 0;
    std::int32_t marriage_age_female = 0;

    // Health and stress (E7-T11, GDD 01 sections 2.1 and 3). Every amount below is in HUNDREDTHS of a
    // point per month (100 = one point of the 0...100 scale), signed where a loss is meant.
    /// Monthly change of the stored health by life stage (indexed by LifeStage): growth in childhood,
    /// nothing in the prime, loss from maturity on, fast loss in old age.
    std::array<std::int32_t, kLifeStageCount> health_drift{};
    /// Random monthly swing of the health: uniform in [-noise, +noise] (always one draw per person).
    std::int32_t health_noise = 0;
    /// Extra monthly change while the effective health is below `low_health_below` (a sickly body
    /// recovers badly).
    std::int32_t low_health_drift = 0;
    /// Effective stress from which a character is strained / breaking (0...100, strictly increasing).
    std::int32_t stress_high_from = 99;
    std::int32_t stress_critical_from = 100;
    /// Extra monthly change of the health while strained / breaking (the worse level replaces the
    /// milder one).
    std::int32_t stress_high_health_drift = 0;
    std::int32_t stress_critical_health_drift = 0;
    /// Monthly stress gained while the effective health is below `low_health_below` (illness is a
    /// worry), and by the head of a dynasty (the burden of the house).
    std::int32_t stress_gain_low_health = 0;
    std::int32_t stress_gain_head = 0;
    /// Share of the stress that fades every month, per mille (rest).
    std::int32_t stress_decay_permille = 0;

    // Education (E7-T13, GDD 01 section 5): the monthly progress of a child who has a focus, and what it
    // brings on the 16th birthday.
    /// Points of the education level (0...100) gained a month from the start of adolescence.
    std::int32_t education_gain_per_month = 1;
    /// A tutor adds `skill / education_tutor_skill_step` (whole skill points, rounded down) to the monthly
    /// gain.
    std::int32_t education_tutor_skill_step = 5;
    /// The education trait of the focus is earned from this level.
    std::int32_t education_trait_min_level = 50;
    /// Skill points of the focus skill at full level, without a tutor.
    std::int32_t education_bonus_max = 0;
    /// What a tutor adds to that bonus: this share (per mille) of the tutor's skill in the focus skill.
    std::int32_t education_tutor_bonus_permille = 0;

    /// Largest accepted multiplier (per-mille of one); keeps Fixed32 arithmetic far from overflow.
    static constexpr std::int32_t kMaxMultiplierPermille = 10000;
    /// Largest accepted number of living characters in the crowding rule, and of skill spread points.
    static constexpr std::int32_t kMaxCrowdingLiving = 1048576;
    static constexpr std::int32_t kMaxSkillSpread = 20;
    /// Largest accepted monthly amount of the health and stress rules, in hundredths of a point.
    static constexpr std::int32_t kMaxVitalsStep = 10000;
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
    // E7-T12: the factor falls from 1.0 at 3000 living to 0.05 at 5000 (NFR-PERF-5), so the number of
    // living characters levels off near the budget instead of growing exponentially [predl.]
    .crowding_from_living = 3000,
    .crowding_to_living = 5000,
    .crowding_min_permille = 50,
    // E7-T12: 3 % of the pregnancies end in a miscarriage [predl.]. GDD 01 3.3 counts miscarriages and
    // stillbirths inside the first-year mortality (220 per mille); E7-T19 settles the overlap.
    .miscarriage_permille = 30,

    // GDD 01 3.3: 105 boys per 100 girls [predl.]
    .boys_per_100_girls = 105,
    // GDD 01 3.3: mother's death risk 3-6% per birth, lower with high health and a physician at
    // court. 50 +10 (low health) = 60 (6%); 50 -10 (high health) -10 (physician) = 30 (3%) [predl.]
    .childbirth_risk_base_permille = 50,
    .childbirth_risk_high_health_delta_permille = -10,
    .childbirth_risk_low_health_delta_permille = 10,
    .childbirth_risk_physician_delta_permille = -10,
    // E7-T12: 2 % of the births at term are stillbirths; a newborn starts at health 70 (neither "weak"
    // nor "robust"); GDD 01 3.4: 0.3 x the average of the parents' skills, +-3 [predl.]
    .stillbirth_permille = 20,
    .newborn_health = 70,
    .skill_inherit_permille = 300,
    .skill_spread = 3,

    // GDD 01 3.1: 0-6 child, 7-15 adolescent, 16-50 prime, 51-65 mature, 66+ elder [predl.]
    .adolescent_from = 7,
    .prime_from = 16,
    .mature_from = 51,
    .elder_from = 66,
    // GDD 01 3.1: may marry from 16 (men) / 15 (women) [predl.]
    .marriage_age_male = 16,
    .marriage_age_female = 15,

    // E7-T11, in hundredths of a point per month [predl.]. GDD 01 3.1 gives the stages in words: childhood
    // and adolescence build the body (+0.15 / +0.10 a month), the prime holds it, maturity "raises the
    // risk of illness" (-0.10, about -1.2 a year), old age "falls fast" (-0.40, about -4.8 a year).
    .health_drift = {15, 10, 0, -10, -40},
    // a swing of +-0.2 a month; an unwell body (< 30) loses 0.1 more; strain costs 0.2, a breakdown 0.6
    .health_noise = 20,
    .low_health_drift = -10,
    .stress_high_from = 60,
    .stress_critical_from = 90,
    .stress_high_health_drift = -20,
    .stress_critical_health_drift = -60,
    // illness adds a point of stress a month, leading a house 0.3; a tenth of the stress fades monthly
    .stress_gain_low_health = 100,
    .stress_gain_head = 30,
    .stress_decay_permille = 100,

    // E7-T13 [predl.]: a point of level a month from age 7 fills the scale by 16 (108 months); a tutor with
    // skill 10 adds two points a month; the education trait from level 50; +2 skill points at full level
    // and 15 % of the tutor's skill on top (GDD 01 section 5: "bonuses of skills at 16")
    .education_gain_per_month = 1,
    .education_tutor_skill_step = 5,
    .education_trait_min_level = 50,
    .education_bonus_max = 2,
    .education_tutor_bonus_permille = 150,
};

// ---- validation --------------------------------------------------------------------------------

enum class RulesErrorCode : std::uint8_t {
    MortalityTable,   ///< empty, not starting at age 0, not strictly increasing, or value outside 0...1000
    AgeFactorTable,   ///< as above, factor outside 0...kMaxMultiplierPermille
    Multiplier,       ///< a multiplier outside 0...kMaxMultiplierPermille
    HealthThreshold,  ///< thresholds outside 0...100 or low >= high
    Conception,       ///< base probability, child cap, pregnancy length, crowding or miscarriage chance
    Birth,            ///< sex ratio, childbirth risk, stillbirth, newborn health or skills out of range
    LifeStages,       ///< stage ages not strictly increasing, or marriage ages outside the stages
    HealthDrift,      ///< a monthly health amount or the swing outside its corridor
    Stress,           ///< stress thresholds not increasing within 0...100, or an amount outside its corridor
    Education,        ///< education gain, tutor step, trait level or bonus outside its corridor
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
    std::int32_t living_count = 0;       ///< living characters in the world (for the crowding factor)
};

/// Monthly conception chance (per mille): base x age factor x health factor x traits x together,
/// x crowding factor, zero once `children_born` reaches the cap. Terms: demography.conception.base /
/// .age / .low_health / .high_health / .traits / .crowding / .apart / .child_cap / .clamp.
[[nodiscard]] ProbabilityBreakdown conception_permille(const DemographyRules& rules,
                                                       const ConceptionInput& input);

/// Chance (per mille) that the mother dies in childbirth: base plus health and physician deltas,
/// clamped to 0...1000. Terms: demography.childbirth.base / .low_health / .high_health /
/// .physician / .clamp.
[[nodiscard]] ProbabilityBreakdown
childbirth_risk_permille(const DemographyRules& rules, core::Fixed32 health, bool physician_at_court);

/// The crowding factor (per mille of one, `crowding_min_permille` ... 1000) for `living` living
/// characters; see DemographyRules::crowding_from_living.
[[nodiscard]] std::int32_t crowding_permille(const DemographyRules& rules, std::int32_t living) noexcept;

/// Chance (per mille, integer) that a newborn is a boy: boys / (boys + 100) from the sex ratio.
[[nodiscard]] std::int32_t male_birth_permille(const DemographyRules& rules) noexcept;

[[nodiscard]] LifeStage life_stage(const DemographyRules& rules, std::int32_t age) noexcept;

// ---- health, stress and fertility of a month (E7-T11) --------------------------------------------

/// One line of an explanation of a monthly change; like ProbabilityTerm, `key` is a stable tooltip key.
struct VitalsTerm {
    std::string_view key;
    core::Fixed32 value;

    [[nodiscard]] bool operator==(const VitalsTerm&) const = default;
};

/// A signed monthly change of a 0...100 scale with the terms that add up to it exactly:
/// sum of `terms[i].value` == `total`.
struct VitalsChange {
    static constexpr std::size_t kMaxTerms = 6;

    core::StaticVec<VitalsTerm, kMaxTerms> terms;
    core::Fixed32 total;

    [[nodiscard]] core::Fixed32 sum_of_terms() const noexcept;
};

/// The tail of a swing roll that means "no swing at all" (the middle of the range).
inline constexpr std::uint32_t kNeutralHealthRoll = 0x80000000U;

/// The monthly change of the STORED health of a character of `age` whose effective health and stress
/// are given (traits included): the drift of the life stage, plus `low_health_drift` below
/// `low_health_below`, plus the strain penalty at `stress_high_from` (the breakdown penalty from
/// `stress_critical_from` replaces it), plus a swing in [-health_noise, +health_noise] hundredths chosen
/// by `noise_roll` (a raw 32-bit draw, mapped by multiply-high). Terms (stable keys):
/// demography.health.age / .low_health / .stress / .noise; a zero amount is omitted. The result is not
/// clamped: the caller keeps the stored value within 0...100.
[[nodiscard]] VitalsChange monthly_health_change(const DemographyRules& rules,
                                                 std::int32_t age,
                                                 core::Fixed32 effective_health,
                                                 core::Fixed32 effective_stress,
                                                 std::uint32_t noise_roll = kNeutralHealthRoll);

/// The monthly change of the STORED stress: `stress_gain_low_health` while the effective health is below
/// `low_health_below`, `stress_gain_head` for the head of a dynasty, then `stress_decay_permille` of the
/// sum fades; the result stays within 0...100. Terms: demography.stress.low_health / .head / .decay /
/// .clamp; the total is `new - stored`, so that stored + total is the new stored stress.
[[nodiscard]] VitalsChange monthly_stress_change(const DemographyRules& rules,
                                                 core::Fixed32 stored_stress,
                                                 core::Fixed32 effective_health,
                                                 bool dynasty_head);

/// Education of a month and of the 16th birthday (E7-T13). `tutor_skill` is the effective skill of the
/// tutor in the focus skill of the child (0 without a tutor).
[[nodiscard]] std::int32_t education_monthly_gain(const DemographyRules& rules,
                                                  core::Fixed32 tutor_skill) noexcept;

/// What a child brings home at 16 with `level` (0...100): the skill bonus split into the part of the
/// focus itself and the part of the tutor, each scaled by the level; the sum is cut to the skill scale.
struct EducationOutcome {
    core::Fixed32 base_bonus;   ///< education_bonus_max x level / 100
    core::Fixed32 tutor_bonus;  ///< tutor skill x education_tutor_bonus_permille / 1000 x level / 100
    core::Fixed32 bonus;        ///< base + tutor, at most the top of the skill scale
    bool earns_trait = false;   ///< level >= education_trait_min_level

    [[nodiscard]] bool operator==(const EducationOutcome&) const = default;
};
[[nodiscard]] EducationOutcome
education_outcome(const DemographyRules& rules, std::int32_t level, core::Fixed32 tutor_skill) noexcept;

/// Level of an effective stress value against the thresholds of the rules.
[[nodiscard]] StressLevel stress_level(const DemographyRules& rules, core::Fixed32 effective_stress) noexcept;

/// The derived fertility of a character, 0...100: 100 x the age factor of `conception.age_factor` x the
/// health factor (low / high health of the same table as the conception chance), cut to the scale.
/// Traits are not included: they add to it when the effective value is built (attribute_breakdown).
[[nodiscard]] core::Fixed32
derived_fertility(const DemographyRules& rules, std::int32_t age, core::Fixed32 effective_health) noexcept;

}  // namespace pat::sim
