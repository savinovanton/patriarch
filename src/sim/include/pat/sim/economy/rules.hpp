#pragma once

#include "pat/core/fixed.hpp"
#include "pat/core/result.hpp"
#include "pat/sim/economy/money.hpp"
#include "pat/sim/economy/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// EconomyRules (E9-T04, docs/dev/economy-model.md sections 9, 11 and 15, GDD 03): every number the
// economy formulas F1...F16 read. Rules are CONTENT, not state: they are not part of the World, are
// handed to the economy systems as an argument, and are loaded from `.pd` files (data/economy/, E9-T18,
// pat::content::load_economy_content). EconomyRules::defaults() is the ONLY place in the code with
// numeric constants of the economy (rules.cpp, one named table, with the GDD section next to every
// group of values) and the fallback for tests and tools; a test checks that the shipped files equal it.
//
// Like DemographyRules this is pure data plus pure functions. There is no floating point:
//   * money is Kopecks (int64, `_kop`);
//   * ratios and multipliers are integer per-mille (`_permille`, 1000 = x1.0);
//   * yearly rates are integer ppm per year (`_ppm_year`, 10^-6);
//   * mood, wealth and control points are Fixed32 or whole points;
//   * head counts are people.
//
// A test can build any rules it needs by copying defaults() and editing fields; validate() checks a
// rules instance for internal consistency (ranges, ordering of thresholds, table sizes).
//
// ALL numbers in the default table are [predl.] placeholders: the GDD gives few, the rest are this
// task's own neutral values so that the formulas have something to work with (open question Q1,
// #136). Values that depend on an open owner decision (Q3 loans #138, Q4 foreign building #139,
// Q6 cities #141, Q7 crown account #142) are marked in the table and follow the interim decision of
// economy-model section 15.2.

namespace pat::sim::economy {

inline constexpr std::size_t kMaxPolicyLevels = 8;
inline constexpr std::size_t kMaxBuildingTypes = 16;
/// Largest accepted multiplier (per-mille of one); keeps products far from overflow.
inline constexpr std::int32_t kMaxMultiplierPermille = 10000;
/// Largest accepted yearly rate in ppm (+-100% per year).
inline constexpr std::int32_t kMaxRatePpm = 1'000'000;

// ---- groups of rules ---------------------------------------------------------------------------

/// F1 population growth (GDD 03 section 4.2).
struct PopulationRules {
    [[nodiscard]] bool operator==(const PopulationRules&) const = default;

    /// Yearly birth and death rates per group, ppm of the group size.
    std::array<std::int32_t, kGroupCount> birth_ppm_year{};
    std::array<std::int32_t, kGroupCount> death_ppm_year{};
    /// Added to the yearly net rate per wealth point above 100 (below 100: subtracted).
    std::int32_t wealth_growth_ppm_per_point = 0;
    /// A province holds at most `capacity_per_development x development` people before crowding
    /// slows growth.
    std::int32_t capacity_per_development = 1;
};

/// F2-F4 labour, output and holding income (GDD 03 section 5.2).
struct ProductionRules {
    [[nodiscard]] bool operator==(const ProductionRules&) const = default;

    std::int32_t labour_min_permille = 0;
    std::int32_t labour_max_permille = 0;
    /// Labour lost per point of negative mood of the peasants, per-mille per point.
    std::int32_t labour_mood_slope_permille = 0;
    /// Share of output lost to waste and theft (the `losses` factor).
    std::int32_t losses_permille = 0;
    /// Static price of one unit of each good (until E19 brings node prices).
    std::array<Kopecks, kMaxGoods> base_price_kop{};
    /// Share of a holding's income that reaches the owner, per province kind.
    std::array<std::int32_t, kProvinceKindCount> owner_share_permille{};
};

/// F5 consumption, stock and wealth (GDD 03 section 5.3).
struct ConsumptionRules {
    [[nodiscard]] bool operator==(const ConsumptionRules&) const = default;

    /// Units of each good one person of a group needs per month, in thousandths of a unit.
    std::array<std::array<std::int32_t, kMaxGoods>, kGroupCount> need_per_head_milli{};
    /// The grain stock of a province is capped at this many months of need.
    std::int32_t stock_cap_months = 1;
    /// Wealth points lost when a good is completely missing (100% shortage).
    std::array<std::int32_t, kMaxGoods> shortage_wealth_points{};
    /// The wealth every group tends to when nothing is missing (100 = normal).
    std::array<std::int32_t, kGroupCount> wealth_base{};
    /// Share of the gap to the target wealth closed each month.
    std::int32_t wealth_inertia_permille = 0;
};

/// One step of the tax policy (GDD 03 section 9): a tax multiplier and the pressure it puts on
/// the mood of each group.
struct TaxLevel {
    std::int32_t tax_permille = 1000;
    std::array<core::Fixed32, kGroupCount> mood_pressure{};

    [[nodiscard]] bool operator==(const TaxLevel&) const = default;
};

/// F6 taxes (GDD 03 sections 3.2, 3.2a, 7.1).
struct TaxRules {
    [[nodiscard]] bool operator==(const TaxRules&) const = default;

    /// Yearly head tax per person, in kopecks. Nobility and clergy pay in service, not money (M1).
    std::array<Kopecks, kGroupCount> head_tax_kop_year{};
    /// Legal multiplier on tax (reforms of the poll tax and the like; 1000 in M1).
    std::int32_t law_permille = 1000;
    /// Extra multiplier on tax when control is below `control.low_threshold`.
    std::int32_t low_control_tax_permille = 1000;
    /// Share of a city's townsfolk tax that goes to the crown instead of the owner (Q6, #141).
    std::int32_t city_crown_share_permille = 0;
    std::array<TaxLevel, kMaxPolicyLevels> levels{};
    std::int32_t level_count = 0;
    std::int32_t default_level = 0;
};

/// F7 control and corruption (GDD 03 sections 3.2, 7.4).
struct ControlRules {
    [[nodiscard]] bool operator==(const ControlRules&) const = default;

    /// Below this the owner collects less and corruption grows; at 40 in the GDD.
    std::int32_t low_threshold = 0;
    /// Below this the province is in turmoil and the owner gets nothing; 15 in the GDD.
    std::int32_t anarchy_threshold = 0;
    /// An administrator with this stewardship / integrity (0...100) changes nothing.
    std::int32_t administrator_neutral = 50;
    std::int32_t control_base = 0;
    /// Control points per point of stewardship above neutral, per-mille of a point.
    std::int32_t steward_gain_permille = 0;
    /// Control points lost per point of unrest, per-mille of a point.
    std::int32_t unrest_penalty_permille = 0;
    std::int32_t control_inertia_permille = 0;
    /// Corruption as per-mille of the tax collected.
    std::int32_t corruption_base_permille = 0;
    /// Corruption per-mille removed per point of integrity above neutral.
    std::int32_t integrity_gain_permille = 0;
    std::int32_t corruption_inertia_permille = 0;
};

/// F8 mood of a group (GDD 03 section 4.2).
struct MoodRules {
    [[nodiscard]] bool operator==(const MoodRules&) const = default;

    std::array<core::Fixed32, kGroupCount> base_mood{};
    /// Mood points per wealth point above 100, and per control point above 50, per-mille of a point.
    std::int32_t wealth_slope_permille = 0;
    std::int32_t admin_slope_permille = 0;
    std::int32_t inertia_permille = 0;
};

/// F9-F10 unrest and riots (GDD 03 section 4.3).
struct UnrestRules {
    [[nodiscard]] bool operator==(const UnrestRules&) const = default;

    /// Unrest points per point of negative mood, per-mille.
    std::int32_t slope_permille = 0;
    /// Weight of each group in the province's unrest (multiplied by its size).
    std::array<std::int32_t, kGroupCount> influence_permille{};
    /// Unrest points per control point below the low threshold, per-mille.
    std::int32_t control_slope_permille = 0;
    std::int32_t inertia_permille = 0;
    /// State boundaries: below `calm_below` calm, below `riot_from` unrest, below `uprising_from`
    /// riots, from `uprising_from` uprising.
    std::int32_t calm_below = 0;
    std::int32_t riot_from = 0;
    std::int32_t uprising_from = 0;
    /// Monthly chance of a riot at `riot_from` and at 100 unrest (linear in between), per-mille.
    std::int32_t riot_chance_min_permille = 0;
    std::int32_t riot_chance_max_permille = 0;
    /// Share of the last month's income looted in a riot, and control lost.
    std::int32_t riot_loot_permille = 0;
    std::int32_t riot_control_loss = 0;
};

/// F11 recruits (GDD 03 section 10).
struct RecruitLevel {
    std::int32_t recruit_permille = 1000;
    std::array<core::Fixed32, kGroupCount> mood_pressure{};

    [[nodiscard]] bool operator==(const RecruitLevel&) const = default;
};

struct RecruitRules {
    [[nodiscard]] bool operator==(const RecruitRules&) const = default;

    /// Share of each group's size that forms the recruit potential.
    std::array<std::int32_t, kGroupCount> share_permille{};
    std::array<RecruitLevel, kMaxPolicyLevels> levels{};
    std::int32_t level_count = 0;
    std::int32_t default_level = 0;
    /// Share of the gap to the potential closed each month.
    std::int32_t regen_permille = 0;
    /// Honor lost when a recruit quota is not delivered (F14).
    std::int32_t quota_penalty_honor = 0;
};

/// F12-F14 loans from the crown (GDD 03 section 7.3; open question Q3, #138).
struct LoanRules {
    [[nodiscard]] bool operator==(const LoanRules&) const = default;

    std::int32_t rate_ppm_year = 0;
    std::int32_t term_months = 1;
    Kopecks base_limit_kop = 0;
    Kopecks per_honor_kop = 0;
    /// A dynasty whose debt reaches `bankruptcy_threshold_kop` is bankrupt until it falls below
    /// `bankruptcy_exit_kop`.
    Kopecks bankruptcy_threshold_kop = 0;
    Kopecks bankruptcy_exit_kop = 0;
    std::int32_t max_loans_per_dynasty = 0;
};

/// One building type (GDD 03 section 8). Costs and effects come from data; the code only knows
/// the stable `slot` number that is written into saves.
struct BuildingDef {
    std::uint8_t slot = 0;  ///< 1...255; 0 means "no building" in a province slot
    std::string_view key;   ///< localisation key
    Kopecks cost_kop = 0;
    std::int32_t build_days = 1;
    Kopecks upkeep_kop_month = 0;
    /// Output multiplier per good, per-mille (1000 = no effect).
    std::array<std::int32_t, kMaxGoods> output_permille{};

    [[nodiscard]] bool operator==(const BuildingDef&) const = default;
};

/// F15-F16 buildings.
struct BuildingRules {
    [[nodiscard]] bool operator==(const BuildingRules&) const = default;

    std::array<BuildingDef, kMaxBuildingTypes> defs{};
    std::int32_t count = 0;
    /// Share of the paid cost returned when a construction is cancelled.
    std::int32_t refund_permille = 0;
};

struct EconomyRules {
    PopulationRules population;
    ProductionRules production;
    ConsumptionRules consumption;
    TaxRules tax;
    ControlRules control;
    MoodRules mood;
    UnrestRules unrest;
    RecruitRules recruit;
    LoanRules loans;
    BuildingRules buildings;
    /// Q7 (#142).
    CrownAccountMode crown_account_mode = CrownAccountMode::Separate;
    /// Q4 (#139): may a dynasty build in a province it does not own.
    bool build_foreign_allowed = false;

    /// The default rules: the one table of numeric constants (rules.cpp).
    [[nodiscard]] static EconomyRules defaults();

    [[nodiscard]] bool operator==(const EconomyRules&) const = default;
};

// ---- validation --------------------------------------------------------------------------------

enum class EconomyRulesErrorCode : std::uint8_t {
    Population,   ///< growth rates, capacity
    Production,   ///< labour bounds, losses, prices, owner shares
    Consumption,  ///< needs, stock cap, wealth targets
    Tax,          ///< head taxes, multipliers, policy levels
    Control,      ///< thresholds, inertia, corruption
    Mood,         ///< mood slopes and inertia
    Unrest,       ///< thresholds, riot chances
    Recruit,      ///< shares, levels, regeneration
    Loans,        ///< rates, limits, bankruptcy thresholds
    Buildings,    ///< building definitions
    Flags,        ///< enumerations out of range
};

struct EconomyRulesError {
    EconomyRulesErrorCode code = EconomyRulesErrorCode::Population;
    std::string message;

    [[nodiscard]] bool operator==(const EconomyRulesError&) const = default;
};

/// Checks a rules instance for consistency and returns the first problem found:
/// every per-mille/ppm/kopeck value within its range, thresholds in order (anarchy < low control;
/// calm < riot < uprising <= 100), labour_min <= labour_max, chance min <= max, bankruptcy exit
/// below its threshold, level tables non-empty with a valid default, building slots unique and
/// non-zero. Rules that pass can be used by the formulas without further checks.
[[nodiscard]] core::Result<void, EconomyRulesError> validate(const EconomyRules& rules);

}  // namespace pat::sim::economy
