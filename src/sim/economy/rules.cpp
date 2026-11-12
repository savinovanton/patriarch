#include "pat/sim/economy/rules.hpp"

#include <format>
#include <utility>

namespace pat::sim::economy {

using core::Fixed32;

namespace {

/// Whole points as Fixed32 (constexpr, unlike Fixed32::from_int in debug builds).
constexpr Fixed32 pts(std::int32_t whole) {
    return Fixed32::from_raw(whole * Fixed32::one_raw);
}

/// One default building (E9-T16): every field is written, the output multiplier is 1000 for every good
/// but `good` (-1 = none). See the table of the default rules for the meaning of the arguments.
constexpr BuildingDef default_building(std::uint8_t slot,
                                       std::string_view key,
                                       std::string_view desc_key,
                                       Kopecks cost_kop,
                                       std::int32_t build_days,
                                       Kopecks upkeep_kop_month,
                                       int good,
                                       std::int32_t permille,
                                       std::int32_t min_development,
                                       std::uint32_t allowed_kinds,
                                       std::int32_t control_points = 0,
                                       std::int32_t oversight_permille = 0,
                                       std::int32_t recruit_permille = kPermilleOne,
                                       std::int32_t famine_protect_permille = 0) {
    BuildingDef def;
    def.slot = slot;
    def.key = key;
    def.desc_key = desc_key;
    def.cost_kop = cost_kop;
    def.build_days = build_days;
    def.upkeep_kop_month = upkeep_kop_month;
    for (std::size_t g = 0; g < kMaxGoods; ++g) {
        def.output_permille[g] = static_cast<int>(g) == good ? permille : kPermilleOne;
    }
    def.min_development = min_development;
    def.allowed_kinds = allowed_kinds;
    def.max_per_province = 1;
    def.control_points = control_points;
    def.oversight_permille = oversight_permille;
    def.recruit_permille = recruit_permille;
    def.famine_protect_permille = famine_protect_permille;
    return def;
}

// ==================================================================================================
// THE TABLE OF DEFAULT VALUES
//
// The only numeric constants of the economy in code. Every group of values cites its source.
//   "GDD 03 s.X"          - the number is given by the GDD section X;
//   "economy-model s.X"   - the number is given by docs/dev/economy-model.md (its examples);
//   "own placeholder"     - the GDD names the quantity but gives no number: a neutral value chosen
//                           here so the formulas have something to work with. All of them are
//                           [predl.] and are tuned by the task that implements the formula
//                           (E9-T06...T17) or replaced from data by E9-T18.
// Group order everywhere: Nobility, Clergy, Townsfolk, Peasants, Cossacks, Natives.
// Province kind order: Rural, City, Border, Industrial, Port, Steppe, Northern, Simplified.
// Goods order: grain, salt, fur, timber, hemp/flax, iron, copper/silver, cloth, fish, arms.
// ==================================================================================================
constexpr EconomyRules kDefaultEconomyRules{
    // ---- F1 population growth ---------------------------------------------------------------
    // GDD 03 s.14.1 only fixes the outcome: net growth 0.3...1.0 % per year in peace [predl.].
    // Own placeholders: every group is set to +0.5 % per year (birth - death = 5000 ppm); cities
    // are more deadly and more fertile than the countryside. Wealth moves it by 20 ppm per point.
    .population =
        {
            .birth_ppm_year = {{36000, 30000, 44000, 42000, 41000, 40000}},
            .death_ppm_year = {{31000, 25000, 39000, 37000, 36000, 35000}},
            .wealth_growth_ppm_per_point = 20,
            // own placeholder: 25 000 people per development point (development 4: 100 000)
            .capacity_per_development = 25000,
        },

    // ---- F2-F4 labour, output, prices -------------------------------------------------------
    .production =
        {
            // own placeholders: labour factor 0.3...1.5 of the starting workforce; each point of
            // negative peasant mood costs 0.5 % of labour
            .labour_min_permille = 300,
            .labour_max_permille = 1500,
            .labour_mood_slope_permille = 5,
            // GDD 03 s.5.2 has the (1 - losses) factor; own placeholder: 5 %
            .losses_permille = 50,
            // economy-model s.11.1 / GDD 03 s.13: grain 1200 kopecks (12 roubles). The GDD gives
            // no other price; the rest is a uniform own placeholder of 10 roubles until E19 brings
            // node prices.
            .base_price_kop = {{1200, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000}},
            // own placeholder: the owner keeps the whole income of a holding; what a city owner
            // really gets is regulated by tax.city_crown_share_permille (GDD 03 s.3.2a)
            .owner_share_permille = {{1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000}},
        },

    // ---- F5 consumption, stock, wealth ------------------------------------------------------
    .consumption =
        {
            // economy-model s.11.1: grain need per head per month, in thousandths of a unit:
            // nobility 40, clergy 40, townsfolk 38, peasants 35, cossacks 38. The GDD (s.5.3) names
            // salt and cloth as consumed but gives no norms: own placeholders (salt 3 for everybody;
            // cloth 6, 4, 3, 2, 3 by group). Natives have size 0 in M1.
            .need_per_head_milli = {{
                {{40, 3, 0, 0, 0, 0, 0, 6, 0, 0}},
                {{40, 3, 0, 0, 0, 0, 0, 4, 0, 0}},
                {{38, 3, 0, 0, 0, 0, 0, 3, 0, 0}},
                {{35, 3, 0, 0, 0, 0, 0, 2, 0, 0}},
                {{38, 3, 0, 0, 0, 0, 0, 3, 0, 0}},
                {{0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
            }},
            // own placeholders: a grain stock of up to 3 months of need; a missing grain supply
            // costs 60 wealth points (salt and cloth cost none until E19 brings trade: a province
            // that does not make them could not get them); wealth tends to 100 (norm) and closes
            // 20 % of the gap a month
            .stock_cap_months = 3,
            .shortage_wealth_points = {{60, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
            .wealth_base = {{100, 100, 100, 100, 100, 100}},
            .wealth_inertia_permille = 200,
        },

    // ---- F6 taxes ---------------------------------------------------------------------------
    .tax =
        {
            // GDD 03 s.4.1: nobility and clergy pay with service / privilege, not money (0 in M1).
            // The GDD gives no rates; own placeholders in kopecks per head per year.
            .head_tax_kop_year = {{0, 0, 150, 50, 20, 40}},
            // economy-model s.9 F6: the law factor is 1000 in M1
            .law_permille = 1000,
            // GDD 03 s.3.2: control < 40 lowers what is collected; own placeholder x0.7
            .low_control_tax_permille = 700,
            // Q6 (#141), interim decision: a city is a flag plus a share to the crown (GDD 03
            // s.3.2a "only part of the taxes goes to the owner"); the number is an own placeholder.
            .city_crown_share_permille = 300,
            // economy-model s.11.1 gives level 3 ("excessive"): tax x1.6 and mood pressure
            // peasants 14, townsfolk 12, cossacks 16. Levels 0...2 (low, normal, high; GDD 03 s.9)
            // are own placeholders around it.
            .levels = {{
                TaxLevel{.tax_permille = 700,
                         .mood_pressure = {{pts(0), pts(0), pts(-5), pts(-6), pts(-7), pts(0)}}},
                TaxLevel{.tax_permille = 1000,
                         .mood_pressure = {{pts(0), pts(0), pts(0), pts(0), pts(0), pts(0)}}},
                TaxLevel{.tax_permille = 1300,
                         .mood_pressure = {{pts(0), pts(0), pts(6), pts(7), pts(8), pts(0)}}},
                TaxLevel{.tax_permille = 1600,
                         .mood_pressure = {{pts(0), pts(0), pts(12), pts(14), pts(16), pts(0)}}},
            }},
            .level_count = 4,
            .default_level = 1,
        },

    // ---- F7 control and corruption ----------------------------------------------------------
    .control =
        {
            // GDD 03 s.3.2: control < 40 hurts collection and breeds corruption; < 15 is turmoil
            .low_threshold = 40,
            .anarchy_threshold = 15,
            // economy-model s.9 F7: stewardship and integrity are 0...100 with a neutral 50
            .administrator_neutral = 50,
            // own placeholders (E9-T10): an average administrator holds a province at 70, the best at 100
            // (+30 at stewardship 100), the worst at 40 (the low threshold); unrest of 100 costs 30. An
            // average one steals 15 % of the tax, the best nothing, the worst 30 %; below the low
            // threshold every missing point of control adds 0.5 %
            .control_base = 70,
            .steward_gain_permille = 600,
            .unrest_penalty_permille = 300,
            .control_inertia_permille = 150,
            .corruption_base_permille = 150,
            .integrity_gain_permille = 3,
            .low_control_corruption_permille = 5,
            .corruption_inertia_permille = 150,
        },

    // ---- F8 mood ----------------------------------------------------------------------------
    // GDD 03 s.4.2 lists the terms but no numbers; own placeholders: no inborn bias, 0.1 mood
    // point per wealth point above 100 and per control point above 50, a quarter of the gap a month.
    .mood =
        {
            .base_mood = {{pts(0), pts(0), pts(0), pts(0), pts(0), pts(0)}},
            .wealth_slope_permille = 100,
            .admin_slope_permille = 100,
            .inertia_permille = 250,
        },

    // ---- F9-F10 unrest and riots ------------------------------------------------------------
    .unrest =
        {
            // own placeholders (E9-T11): 4 unrest points per point of negative mood, so that a province
            // that pays the "excessive" tax (mood -14) settles in the riot state (about 60), the "high"
            // one in agitation (about 28) and the usual and low ones stay calm; influence per head
            // (cossacks are the most restless, GDD 03 s.4.1); half a point per missing control point
            .slope_permille = 4000,
            .influence_permille = {{150, 100, 200, 100, 250, 100}},
            .control_slope_permille = 500,
            .inertia_permille = 250,
            // GDD 03 s.4.3: calm < 25, unrest 25...50, riot 50...75, uprising >= 75
            .calm_below = 25,
            .riot_from = 50,
            .uprising_from = 75,
            // own placeholders: 2 % a month at 50 unrest, 25 % at 100; a riot loots 30 % of the
            // month's income ("part of the monthly income", GDD 03 s.4.3 / US-9.8) and costs 5 control
            .riot_chance_min_permille = 20,
            .riot_chance_max_permille = 250,
            .riot_loot_permille = 300,
            .riot_control_loss = 5,
        },

    // ---- F11 recruits -----------------------------------------------------------------------
    .recruit =
        {
            // GDD 03 s.10: potential is a share of peasants / cossacks / townsfolk; own
            // placeholders 3 %, 5 %, 1.5 %
            .share_permille = {{0, 0, 15, 30, 50, 0}},
            // GDD 03 s.9: voluntary / usual / intensified; own placeholders (x0.5, x1.0, x1.6)
            .levels =
                {
                    {
                        RecruitLevel{.recruit_permille = 500,
                                     .mood_pressure = {{pts(0), pts(0), pts(0), pts(0), pts(0), pts(0)}}},
                        RecruitLevel{.recruit_permille = 1000,
                                     .mood_pressure = {{pts(0), pts(0), pts(2), pts(2), pts(3), pts(0)}}},
                        RecruitLevel{.recruit_permille = 1600,
                                     .mood_pressure = {{pts(0), pts(0), pts(6), pts(8), pts(9), pts(0)}}},
                    }},
            .level_count = 3,
            .default_level = 1,
            // own placeholders: a tenth of the gap a month; 5 honor for an undelivered quota (F14)
            .regen_permille = 100,
            .quota_penalty_honor = 5,
        },

    // ---- F12-F14 loans ----------------------------------------------------------------------
    // Q3 (#138) is OPEN. Interim decision (economy-model s.15.2): a simple loan from the crown, a
    // limit tied to honor, a fixed rate. Every number is an own placeholder.
    .loans =
        {
            .rate_ppm_year = 100000,  // 10 % a year
            .term_months = 12,
            .base_limit_kop = 1'000'000,  // 10 000 roubles
            .per_honor_kop = 10'000,      // 100 roubles per honor point
            .bankruptcy_threshold_kop = 5'000'000,
            .bankruptcy_exit_kop = 2'500'000,
            // economy-model s.4.7: at most 3 loans per dynasty [predl.]
            .max_loans_per_dynasty = 3,
        },

    // ---- F15-F16 buildings ------------------------------------------------------------------
    // The GDD gives no costs; economy-model s.11.1 gives one example, the mill: 4000 roubles,
    // 120 days, 30 roubles a month, grain output x1.15. The other seven buildings of GDD 03 s.15 are
    // own placeholders [predl.] of the same scale (E9-T16); data/economy/buildings.pd carries the
    // same table (a test compares them). Arguments of default_building: slot, key, cost, build days,
    // upkeep per month, the good the building improves and its multiplier (-1 = none), minimal
    // development, allowed kinds (a bit per ProvinceKind: city 1, border 2, industrial 3, port 4,
    // steppe 5, northern 6; 0 = any), then the effects of E9-T17 [predl.]: control points, oversight
    // per-mille (corruption), recruit multiplier per-mille (1000 = none), famine protection per-mille. At
    // most
    // one of a type per province.
    .buildings =
        {
            .defs = {{default_building(
                          1, "building.mill", "building.mill.desc", 400'000, 120, 3'000, 0, 1150, 0, 0),
                      default_building(2,
                                       "building.manufactory",
                                       "building.manufactory.desc",
                                       1'200'000,
                                       240,
                                       10'000,
                                       7,
                                       1200,
                                       3,
                                       0x0A),
                      default_building(
                          3,
                          "building.works",
                          "building.works.desc",
                          2'000'000,
                          360,
                          15'000,
                          5,
                          1300,
                          3,
                          0x08),
                      default_building(
                          4, "building.tract", "building.tract.desc", 600'000, 180, 4'000, -1, 1000, 0, 0, 3),
                      default_building(
                          5, "building.wharf", "building.wharf.desc", 800'000, 150, 6'000, 8, 1100, 0, 0x10),
                      default_building(
                          6,
                          "building.ostrog",
                          "building.ostrog.desc",
                          1'000'000,
                          240,
                          12'000,
                          -1,
                          1000,
                          0,
                          0x64,
                          5,
                          0,
                          1250),
                      default_building(
                          7,
                          "building.church",
                          "building.church.desc",
                          500'000,
                          300,
                          2'000,
                          -1,
                          1000,
                          0,
                          0,
                          2,
                          30),
                      default_building(8,
                                       "building.granary",
                                       "building.granary.desc",
                                       300'000,
                                       90,
                                       2'000,
                                       -1,
                                       1000,
                                       0,
                                       0,
                                       0,
                                       0,
                                       1000,
                                       300)}},
            .count = 8,
            // own placeholder: 80 % of the paid cost comes back when a construction is cancelled
            .refund_permille = 800,
        },

    // Q7 (#142), interim decision: the crown has its own account.
    .crown_account_mode = CrownAccountMode::Separate,
    // Q4 (#139), interim decision: only in one's own provinces.
    .build_foreign_allowed = false,
};

// ---- validation helpers ------------------------------------------------------------------------

using Check = core::Result<void, EconomyRulesError>;
using Code = EconomyRulesErrorCode;

Check fail(Code code, std::string message) {
    return Check::err(EconomyRulesError{code, std::move(message)});
}

Check range(Code code, std::string_view name, std::int64_t value, std::int64_t lo, std::int64_t hi) {
    if (value < lo || value > hi) {
        return fail(code, std::format("{}: {} outside {}...{}", name, value, lo, hi));
    }
    return Check::ok();
}

Check range_at(Code code,
               std::string_view name,
               std::size_t index,
               std::int64_t value,
               std::int64_t lo,
               std::int64_t hi) {
    if (value < lo || value > hi) {
        return fail(code, std::format("{}[{}]: {} outside {}...{}", name, index, value, lo, hi));
    }
    return Check::ok();
}

template <class Array>
Check range_all(Code code, std::string_view name, const Array& values, std::int64_t lo, std::int64_t hi) {
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (auto res = range_at(code, name, i, values[i], lo, hi); !res) {
            return res;
        }
    }
    return Check::ok();
}

Check points_all(Code code, std::string_view name, const std::array<Fixed32, kGroupCount>& values) {
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (values[i] < pts(-100) || pts(100) < values[i]) {
            return fail(code, std::format("{}[{}]: {} outside -100...100", name, i, values[i]));
        }
    }
    return Check::ok();
}

constexpr std::int64_t kMaxMult = kMaxMultiplierPermille;

Check check_population(const PopulationRules& r) {
    if (auto res = range_all(Code::Population, "birth_ppm_year", r.birth_ppm_year, 0, kMaxRatePpm); !res) {
        return res;
    }
    if (auto res = range_all(Code::Population, "death_ppm_year", r.death_ppm_year, 0, kMaxRatePpm); !res) {
        return res;
    }
    if (auto res =
            range(Code::Population, "wealth_growth_ppm_per_point", r.wealth_growth_ppm_per_point, 0, 100000);
        !res) {
        return res;
    }
    return range(Code::Population, "capacity_per_development", r.capacity_per_development, 1, 10'000'000);
}

Check check_production(const ProductionRules& r) {
    if (auto res = range(Code::Production, "labour_min_permille", r.labour_min_permille, 0, kMaxMult); !res) {
        return res;
    }
    if (auto res = range(Code::Production, "labour_max_permille", r.labour_max_permille, 0, kMaxMult); !res) {
        return res;
    }
    if (r.labour_min_permille > r.labour_max_permille) {
        return fail(Code::Production,
                    std::format("labour_min_permille {} exceeds labour_max_permille {}",
                                r.labour_min_permille,
                                r.labour_max_permille));
    }
    if (auto res =
            range(Code::Production, "labour_mood_slope_permille", r.labour_mood_slope_permille, 0, kMaxMult);
        !res) {
        return res;
    }
    if (auto res = range(Code::Production, "losses_permille", r.losses_permille, 0, kPermilleOne); !res) {
        return res;
    }
    if (auto res = range_all(Code::Production, "base_price_kop", r.base_price_kop, 0, kMaxPostingKop); !res) {
        return res;
    }
    return range_all(Code::Production, "owner_share_permille", r.owner_share_permille, 0, kPermilleOne);
}

Check check_consumption(const ConsumptionRules& r) {
    for (std::size_t g = 0; g < r.need_per_head_milli.size(); ++g) {
        if (auto res =
                range_all(Code::Consumption, "need_per_head_milli", r.need_per_head_milli[g], 0, 1'000'000);
            !res) {
            return res;
        }
    }
    if (auto res = range(Code::Consumption, "stock_cap_months", r.stock_cap_months, 1, 120); !res) {
        return res;
    }
    if (auto res = range_all(Code::Consumption, "shortage_wealth_points", r.shortage_wealth_points, 0, 300);
        !res) {
        return res;
    }
    if (auto res = range_all(Code::Consumption, "wealth_base", r.wealth_base, 0, 300); !res) {
        return res;
    }
    return range(Code::Consumption, "wealth_inertia_permille", r.wealth_inertia_permille, 0, kPermilleOne);
}

Check check_tax(const TaxRules& r) {
    if (auto res = range_all(Code::Tax, "head_tax_kop_year", r.head_tax_kop_year, 0, kMaxPostingKop); !res) {
        return res;
    }
    if (auto res = range(Code::Tax, "law_permille", r.law_permille, 0, kMaxMult); !res) {
        return res;
    }
    if (auto res = range(Code::Tax, "low_control_tax_permille", r.low_control_tax_permille, 0, kPermilleOne);
        !res) {
        return res;
    }
    if (auto res =
            range(Code::Tax, "city_crown_share_permille", r.city_crown_share_permille, 0, kPermilleOne);
        !res) {
        return res;
    }
    if (auto res =
            range(Code::Tax, "level_count", r.level_count, 1, static_cast<std::int64_t>(kMaxPolicyLevels));
        !res) {
        return res;
    }
    if (auto res = range(Code::Tax, "default_level", r.default_level, 0, r.level_count - 1); !res) {
        return res;
    }
    for (std::size_t i = 0; i < static_cast<std::size_t>(r.level_count); ++i) {
        if (auto res = range_at(Code::Tax, "levels.tax_permille", i, r.levels[i].tax_permille, 0, kMaxMult);
            !res) {
            return res;
        }
        if (auto res = points_all(Code::Tax, "levels.mood_pressure", r.levels[i].mood_pressure); !res) {
            return res;
        }
    }
    return Check::ok();
}

Check check_control(const ControlRules& r) {
    if (auto res = range(Code::Control, "low_threshold", r.low_threshold, 1, 100); !res) {
        return res;
    }
    if (auto res = range(Code::Control, "anarchy_threshold", r.anarchy_threshold, 0, r.low_threshold - 1);
        !res) {
        return res;
    }
    if (auto res = range(Code::Control, "administrator_neutral", r.administrator_neutral, 0, 100); !res) {
        return res;
    }
    if (auto res = range(Code::Control, "control_base", r.control_base, 0, 100); !res) {
        return res;
    }
    if (auto res = range(Code::Control, "steward_gain_permille", r.steward_gain_permille, 0, kMaxMult);
        !res) {
        return res;
    }
    if (auto res = range(Code::Control, "unrest_penalty_permille", r.unrest_penalty_permille, 0, kMaxMult);
        !res) {
        return res;
    }
    if (auto res =
            range(Code::Control, "control_inertia_permille", r.control_inertia_permille, 0, kPermilleOne);
        !res) {
        return res;
    }
    if (auto res =
            range(Code::Control, "corruption_base_permille", r.corruption_base_permille, 0, kPermilleOne);
        !res) {
        return res;
    }
    if (auto res = range(Code::Control, "integrity_gain_permille", r.integrity_gain_permille, 0, kMaxMult);
        !res) {
        return res;
    }
    if (auto res = range(
            Code::Control, "low_control_corruption_permille", r.low_control_corruption_permille, 0, kMaxMult);
        !res) {
        return res;
    }
    return range(
        Code::Control, "corruption_inertia_permille", r.corruption_inertia_permille, 0, kPermilleOne);
}

Check check_mood(const MoodRules& r) {
    if (auto res = points_all(Code::Mood, "base_mood", r.base_mood); !res) {
        return res;
    }
    if (auto res = range(Code::Mood, "wealth_slope_permille", r.wealth_slope_permille, 0, kMaxMult); !res) {
        return res;
    }
    if (auto res = range(Code::Mood, "admin_slope_permille", r.admin_slope_permille, 0, kMaxMult); !res) {
        return res;
    }
    return range(Code::Mood, "inertia_permille", r.inertia_permille, 0, kPermilleOne);
}

Check check_unrest(const UnrestRules& r) {
    if (auto res = range(Code::Unrest, "slope_permille", r.slope_permille, 0, kMaxMult); !res) {
        return res;
    }
    if (auto res = range_all(Code::Unrest, "influence_permille", r.influence_permille, 0, kMaxMult); !res) {
        return res;
    }
    std::int64_t influence_sum = 0;
    for (const std::int32_t w : r.influence_permille) {
        influence_sum += w;
    }
    if (influence_sum == 0) {
        return fail(Code::Unrest, "influence_permille: at least one group needs a positive weight");
    }
    if (auto res = range(Code::Unrest, "control_slope_permille", r.control_slope_permille, 0, kMaxMult);
        !res) {
        return res;
    }
    if (auto res = range(Code::Unrest, "inertia_permille", r.inertia_permille, 0, kPermilleOne); !res) {
        return res;
    }
    if (r.calm_below <= 0 || r.riot_from <= r.calm_below || r.uprising_from <= r.riot_from ||
        r.uprising_from > 100) {
        return fail(Code::Unrest,
                    std::format("thresholds {}/{}/{} must satisfy 0 < calm < riot < uprising <= 100",
                                r.calm_below,
                                r.riot_from,
                                r.uprising_from));
    }
    if (auto res =
            range(Code::Unrest, "riot_chance_min_permille", r.riot_chance_min_permille, 0, kPermilleOne);
        !res) {
        return res;
    }
    if (auto res =
            range(Code::Unrest, "riot_chance_max_permille", r.riot_chance_max_permille, 0, kPermilleOne);
        !res) {
        return res;
    }
    if (r.riot_chance_min_permille > r.riot_chance_max_permille) {
        return fail(Code::Unrest,
                    std::format("riot_chance_min_permille {} exceeds riot_chance_max_permille {}",
                                r.riot_chance_min_permille,
                                r.riot_chance_max_permille));
    }
    if (auto res = range(Code::Unrest, "riot_loot_permille", r.riot_loot_permille, 0, kPermilleOne); !res) {
        return res;
    }
    return range(Code::Unrest, "riot_control_loss", r.riot_control_loss, 0, 100);
}

Check check_recruit(const RecruitRules& r) {
    if (auto res = range_all(Code::Recruit, "share_permille", r.share_permille, 0, kPermilleOne); !res) {
        return res;
    }
    if (auto res = range(
            Code::Recruit, "level_count", r.level_count, 1, static_cast<std::int64_t>(kMaxPolicyLevels));
        !res) {
        return res;
    }
    if (auto res = range(Code::Recruit, "default_level", r.default_level, 0, r.level_count - 1); !res) {
        return res;
    }
    for (std::size_t i = 0; i < static_cast<std::size_t>(r.level_count); ++i) {
        if (auto res = range_at(
                Code::Recruit, "levels.recruit_permille", i, r.levels[i].recruit_permille, 0, kMaxMult);
            !res) {
            return res;
        }
        if (auto res = points_all(Code::Recruit, "levels.mood_pressure", r.levels[i].mood_pressure); !res) {
            return res;
        }
    }
    if (auto res = range(Code::Recruit, "regen_permille", r.regen_permille, 0, kPermilleOne); !res) {
        return res;
    }
    return range(Code::Recruit, "quota_penalty_honor", r.quota_penalty_honor, 0, 1000);
}

Check check_loans(const LoanRules& r) {
    if (auto res = range(Code::Loans, "rate_ppm_year", r.rate_ppm_year, 0, kMaxRatePpm); !res) {
        return res;
    }
    if (auto res = range(Code::Loans, "term_months", r.term_months, 1, 600); !res) {
        return res;
    }
    if (auto res = range(Code::Loans, "base_limit_kop", r.base_limit_kop, 0, kMaxTreasuryKop); !res) {
        return res;
    }
    if (auto res = range(Code::Loans, "per_honor_kop", r.per_honor_kop, 0, kMaxPostingKop); !res) {
        return res;
    }
    if (auto res =
            range(Code::Loans, "bankruptcy_threshold_kop", r.bankruptcy_threshold_kop, 1, kMaxTreasuryKop);
        !res) {
        return res;
    }
    if (auto res = range(
            Code::Loans, "bankruptcy_exit_kop", r.bankruptcy_exit_kop, 0, r.bankruptcy_threshold_kop - 1);
        !res) {
        return res;
    }
    return range(Code::Loans, "max_loans_per_dynasty", r.max_loans_per_dynasty, 0, 64);
}

Check check_buildings(const BuildingRules& r) {
    if (auto res = range(Code::Buildings, "count", r.count, 0, static_cast<std::int64_t>(kMaxBuildingTypes));
        !res) {
        return res;
    }
    if (auto res = range(Code::Buildings, "refund_permille", r.refund_permille, 0, kPermilleOne); !res) {
        return res;
    }
    for (std::size_t i = 0; i < static_cast<std::size_t>(r.count); ++i) {
        const BuildingDef& def = r.defs[i];
        if (def.slot == 0) {
            return fail(Code::Buildings, std::format("defs[{}]: slot 0 is reserved for 'no building'", i));
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (r.defs[j].slot == def.slot) {
                return fail(Code::Buildings,
                            std::format("defs[{}]: slot {} already used by defs[{}]", i, def.slot, j));
            }
        }
        if (def.key.empty()) {
            return fail(Code::Buildings, std::format("defs[{}]: empty localisation key", i));
        }
        if (auto res = range_at(Code::Buildings, "cost_kop", i, def.cost_kop, 0, kMaxPostingKop); !res) {
            return res;
        }
        if (auto res = range_at(Code::Buildings, "build_days", i, def.build_days, 1, 3650); !res) {
            return res;
        }
        if (auto res =
                range_at(Code::Buildings, "upkeep_kop_month", i, def.upkeep_kop_month, 0, kMaxPostingKop);
            !res) {
            return res;
        }
        if (auto res = range_all(Code::Buildings, "output_permille", def.output_permille, 0, kMaxMult);
            !res) {
            return res;
        }
        if (auto res =
                range_at(Code::Buildings, "min_development", i, def.min_development, 0, kMaxDevelopment);
            !res) {
            return res;
        }
        if (auto res = range_at(Code::Buildings,
                                "allowed_kinds",
                                i,
                                def.allowed_kinds,
                                0,
                                (std::int64_t{1} << kProvinceKindCount) - 1);
            !res) {
            return res;
        }
        if (auto res = range_at(Code::Buildings, "control_points", i, def.control_points, -50, 50); !res) {
            return res;
        }
        if (auto res =
                range_at(Code::Buildings, "oversight_permille", i, def.oversight_permille, 0, kPermilleOne);
            !res) {
            return res;
        }
        if (auto res = range_at(Code::Buildings, "recruit_permille", i, def.recruit_permille, 0, kMaxMult);
            !res) {
            return res;
        }
        if (auto res = range_at(
                Code::Buildings, "famine_protect_permille", i, def.famine_protect_permille, 0, kPermilleOne);
            !res) {
            return res;
        }
        if (auto res = range_at(Code::Buildings,
                                "max_per_province",
                                i,
                                def.max_per_province,
                                1,
                                static_cast<std::int64_t>(kMaxSlots));
            !res) {
            return res;
        }
    }
    return Check::ok();
}

}  // namespace

EconomyRules EconomyRules::defaults() {
    return kDefaultEconomyRules;
}

core::Result<void, EconomyRulesError> validate(const EconomyRules& r) {
    if (auto res = check_population(r.population); !res) {
        return res;
    }
    if (auto res = check_production(r.production); !res) {
        return res;
    }
    if (auto res = check_consumption(r.consumption); !res) {
        return res;
    }
    if (auto res = check_tax(r.tax); !res) {
        return res;
    }
    if (auto res = check_control(r.control); !res) {
        return res;
    }
    if (auto res = check_mood(r.mood); !res) {
        return res;
    }
    if (auto res = check_unrest(r.unrest); !res) {
        return res;
    }
    if (auto res = check_recruit(r.recruit); !res) {
        return res;
    }
    if (auto res = check_loans(r.loans); !res) {
        return res;
    }
    if (auto res = check_buildings(r.buildings); !res) {
        return res;
    }
    if (r.crown_account_mode != CrownAccountMode::Separate &&
        r.crown_account_mode != CrownAccountMode::EmperorDynasty) {
        return fail(Code::Flags, "crown_account_mode is not a known mode");
    }
    return Check::ok();
}

}  // namespace pat::sim::economy
