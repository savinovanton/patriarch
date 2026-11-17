#pragma once

#include "pat/core/result.hpp"
#include "pat/sim/date.hpp"
#include "pat/sim/economy/money.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Court rules (E10-T04, docs/dev/court-model.md sections 8 and 11, ADR-0009): the CONTENT of offices,
// servant roles, parties and the balance of appointments, favor and servants.
//
// Rules are content, not state: they are not part of the World and are passed to the pure functions
// of the later court tasks (eligibility T05, vacancies T07, petitions T08, servants T12/T13, favor
// T14). `sim` does not know about data files: pat::content::build_court_rules() turns the loaded
// `data/court/*.pd` and `data/balance/court.pd` into this value. CourtRules::defaults() is the ONLY
// place in the code with numeric constants of the court (the balance table below and court_rules.cpp);
// a test keeps the files equal to it. All numbers are [predl.] proposals of the specification.
//
// No floating point: weights are per-mille of one, favor is in hundredths of a point (x100), money is
// kopecks.

namespace pat::sim {

/// Group of an office (section 11.1); the order is stable.
enum class OfficeGroup : std::uint8_t { Court, Regional, Military, Chancery, Council, Favourite };
inline constexpr std::size_t kOfficeGroupCount = 6;

/// Who appoints (section 11.1); only the emperor is implemented in M1.
enum class OfficeAppointer : std::uint8_t { Emperor, Council, DynastyHead, Church };
inline constexpr std::size_t kOfficeAppointerCount = 4;

/// Which seats of an office exist (section 4.2).
enum class SeatRule : std::uint8_t { Fixed, PerCrownProvince, Listed };
inline constexpr std::size_t kSeatRuleCount = 3;

enum class SexRule : std::uint8_t { Male, Female, Any };
inline constexpr std::size_t kSexRuleCount = 3;

/// Office types have a stable `slot` 1...63 (OfficeTypeId); the number of types is bounded by it.
inline constexpr std::size_t kMaxOfficeTypes = 63;
/// Servant roles and parties are numbered the same way (section 3): roles 1...15, parties 1...7.
inline constexpr std::size_t kMaxServantRoles = 15;
inline constexpr std::size_t kMaxCourtParties = 7;
inline constexpr std::size_t kCourtSkillCount = 5;  ///< equals kSkillCount of world.hpp (a test pins it)

struct OfficeSeats {
    SeatRule rule = SeatRule::Fixed;
    /// Fixed: number of seats 1...64.
    std::int32_t slots = 1;
    /// PerCrownProvince and Listed: upper bound of seats 1...4096; 0 for Fixed.
    std::int32_t max = 0;
    /// PerCrownProvince: bit `k` stands for economy::ProvinceKind `k`; 0 for the other rules.
    std::uint32_t kinds = 0;

    [[nodiscard]] bool operator==(const OfficeSeats&) const = default;
};

struct OfficeRequirements {
    std::int32_t min_age = 0;
    SexRule sex_allowed = SexRule::Male;
    /// Honor of the house (the "rank" of M1, section 8.1); soft requirement.
    std::int32_t min_honor = 0;
    /// Minimal skill per Skill index (0 = no demand); soft requirement.
    std::array<std::uint8_t, kCourtSkillCount> min_skill{};
    /// Trait slots (u16 of the character) that exclude a candidate; hard requirement.
    std::vector<std::uint16_t> not_traits;

    [[nodiscard]] bool operator==(const OfficeRequirements&) const = default;
};

/// One office type (section 11.1).
struct OfficeDef {
    std::uint8_t slot = 0;  ///< stable OfficeTypeId 1...63, written into saves
    std::string id;         ///< content id, e.g. "voivode"
    std::string name_key;
    std::string desc_key;
    OfficeGroup group = OfficeGroup::Court;
    OfficeAppointer appointer = OfficeAppointer::Emperor;
    OfficeSeats seats;
    OfficeRequirements requirements;
    /// Points per skill level in the "ability" term of the utility (term 1).
    std::array<std::int32_t, kCourtSkillCount> skill_weights{};
    economy::Kopecks salary_kop = 0;  ///< per month
    std::int32_t honor_on_assign = 0;
    std::int32_t honor_monthly = 0;
    std::int32_t decision_delay_days = 30;  ///< 30...90 in the defaults
    /// Window of dates in which the seats exist; kNoDate = unbounded on that side.
    Date from_date = kNoDate;
    Date to_date = kNoDate;
    /// Regional offices: the holder with the greatest priority administers the province (section 8.8).
    std::int32_t administrator_priority = 0;
    /// Trait slot granted to the holder (the favourite), 0 = none.
    std::uint16_t grants_trait = 0;
    std::int32_t council_vote_weight = 0;  ///< reserved for E34

    [[nodiscard]] bool operator==(const OfficeDef&) const = default;
};

/// Servant role (section 11.3). `skill` is the Skill index that counts as the skill of the role.
struct ServantRoleDef {
    std::uint8_t slot = 0;  ///< stable ServantRole 1...15
    std::string id;
    std::string name_key;
    std::string effect_key;
    std::uint8_t skill = 0;
    std::int32_t slots = 1;  ///< servants of the role per house (D17)
    std::int32_t skill_min = 0;
    std::int32_t skill_max = 0;
    economy::Kopecks wage_base_kop = 0;
    economy::Kopecks wage_per_skill_kop = 0;
    std::int32_t fee_months = 0;

    [[nodiscard]] bool operator==(const ServantRoleDef&) const = default;
};

/// A static party of the court (section 8.10); membership comes from the scenario.
struct CourtPartyDef {
    std::uint8_t slot = 0;  ///< stable PartyId 1...7 (0 = outside the parties)
    std::string id;
    std::string name_key;
    std::string agenda_key;

    [[nodiscard]] bool operator==(const CourtPartyDef&) const = default;
};

// ---- the balance (section 11.4) --------------------------------------------------------------------
// X(group, type, name, default, min, max). The group is the block of data/balance/court.pd and the
// prefix of a diagnostic path; every name is unique. `min`/`max` are the corridor that keeps the
// formulas free of overflow (validate()). One list builds the struct, the defaults, the checks and
// the reader of content/court_content.cpp, so a field cannot be added to one of them only.
// clang-format off
#define PAT_COURT_BALANCE_FIELDS(X)                                                                      \
    X(limits, std::int32_t, max_petitions_per_dynasty, 3, 1, 64)                                         \
    X(limits, std::int32_t, max_offices_per_person, 1, 1, 8)                                             \
    X(limits, std::int64_t, gift_max_kop, 100'000, 0, economy::kMaxPostingKop)                           \
    X(limits, std::int32_t, disgrace_max_days, 730, 1, 36'500)                                           \
    X(limits, std::int32_t, retire_age, 65, 30, 120)                                                     \
    X(limits, std::int32_t, quit_after_months, 3, 1, 120)                                                \
    X(decision, std::int32_t, candidate_prefilter, 16, 1, 256)                                           \
    X(decision, std::int32_t, min_utility, 20, -1'000, 1'000)                                            \
    X(decision, std::int32_t, max_postponements, 2, 0, 16)                                               \
    X(decision, std::int32_t, postpone_days, 30, 1, 365)                                                 \
    X(decision, std::int32_t, relax_after_days, 180, 0, 3'650)                                           \
    X(decision, std::int32_t, self_nomination_min_favor, 40, 0, 100)                                     \
    X(utility, std::int32_t, favor_weight_permille, 300, 0, 10'000)                                      \
    X(utility, std::int32_t, opinion_weight_permille, 200, 0, 10'000)                                    \
    X(utility, std::int32_t, suspicion_weight_permille, 400, 0, 10'000)                                  \
    X(utility, std::int32_t, honor_per_point, 50, 1, 10'000)                                             \
    X(utility, std::int32_t, rank_cap, 20, 0, 1'000)                                                     \
    X(utility, std::int32_t, party_balance_points, 15, 0, 1'000)                                         \
    X(utility, std::int64_t, gift_unit_kop, 5'000, 1, economy::kMaxPostingKop)                           \
    X(utility, std::int32_t, gift_cap, 25, 0, 1'000)                                                     \
    X(utility, std::int32_t, service_points, 15, 0, 1'000)                                               \
    X(utility, std::int32_t, party_support_points, 10, 0, 1'000)                                         \
    X(utility, std::int32_t, favourite_points, 12, 0, 1'000)                                             \
    X(utility, std::int32_t, exceptional_penalty, 20, 0, 1'000)                                          \
    X(utility, std::int32_t, exceptional_max_honor_gap, 200, 0, 100'000)                                 \
    X(utility, std::int32_t, exceptional_max_skill_gap, 3, 0, 20)                                        \
    X(support, std::int32_t, services_max, 5, 0, 255)                                                    \
    X(support, std::int32_t, party_support_cooldown_days, 365, 0, 36'500)                                \
    X(favor, std::int32_t, favor_base_x100, 5'000, 0, 10'000)                                            \
    X(favor, std::int32_t, opinion_to_favor_x100, 25, 0, 1'000)                                          \
    X(favor, std::int32_t, suspicion_to_favor_x100, 20, 0, 1'000)                                        \
    X(favor, std::int32_t, pull_permille, 100, 0, 1'000)                                                 \
    X(favor, std::int32_t, service_per_office_x100, 300, 0, 10'000)                                      \
    X(favor, std::int32_t, service_cap_x100, 900, 0, 10'000)                                             \
    X(favor, std::int32_t, quota_met_x100, 100, 0, 10'000)                                               \
    X(favor, std::int32_t, quota_short_x100, 500, 0, 10'000)                                             \
    X(favor, std::int32_t, favourite_monthly_x100, 150, 0, 10'000)                                       \
    X(favor, std::int32_t, arrears_favor_x100, 200, 0, 10'000)                                           \
    X(favor, std::int32_t, disgrace_favor_x100, 300, 0, 10'000)                                          \
    X(favor, std::int32_t, favor_month_cap_x100, 600, 0, 10'000)                                         \
    X(favor, std::int32_t, favor_low_x100, 2'000, 0, 10'000)                                             \
    X(favor, std::int32_t, favor_high_x100, 8'000, 0, 10'000)                                            \
    X(favor, std::int32_t, favor_on_grant_x100, 200, 0, 10'000)                                          \
    X(favor, std::int32_t, gift_per_100_rub_x100, 25, 0, 10'000)                                         \
    X(favor, std::int32_t, gift_month_cap_x100, 1'000, 0, 10'000)                                        \
    X(opinion, std::int32_t, reject_opinion, 8, 0, 100)                                                  \
    X(opinion, std::int32_t, grant_opinion, 5, 0, 100)                                                   \
    X(opinion, std::int32_t, dismiss_opinion, 15, 0, 100)                                                \
    X(opinion, std::int32_t, disgrace_one_off_x100, 500, 0, 10'000)                                      \
    X(servants, std::int32_t, loyalty_base, 60, 0, 100)                                                  \
    X(servants, std::int32_t, loyalty_pull_permille, 100, 0, 1'000)                                      \
    X(servants, std::int32_t, paid_bonus, 1, 0, 100)                                                     \
    X(servants, std::int32_t, arrears_loss, 8, 0, 100)                                                   \
    X(servants, std::int32_t, unreliable_below, 35, 0, 100)                                              \
    X(servants, std::int32_t, betrayal_base_permille, 50, 0, 1'000)                                      \
    X(servants, std::int32_t, betrayal_per_gap_permille, 10, 0, 1'000)                                   \
    X(servants, std::int64_t, betrayal_bribe_unit_kop, 10'000, 1, economy::kMaxPostingKop)               \
    X(servants, std::int32_t, betrayal_per_unit_permille, 20, 0, 1'000)                                  \
    X(servants, std::int32_t, betrayal_per_secrecy_permille, 15, 0, 1'000)                               \
    X(servants, std::int32_t, steward_control_per_skill_x100, 50, 0, 10'000)                             \
    X(servants, std::int32_t, steward_corruption_per_skill_permille, 5, 0, 1'000)                        \
    X(parties, std::int32_t, party_base_weight, 100, 0, 100'000)                                         \
    X(parties, std::int32_t, party_honor_cap, 1'500, 0, 100'000)                                         \
    X(parties, std::int32_t, party_honor_div, 10, 1, 10'000)                                             \
    X(parties, std::int32_t, cohesion_pair_cap, 200, 1, 10'000)                                          \
    X(ai, std::int32_t, ai_min_utility, 25, -1'000, 1'000)                                               \
    X(ai, std::int32_t, ai_gift_permille, 50, 0, 1'000)                                                  \
    X(ai, std::int64_t, ai_gift_cap_kop, 30'000, 0, economy::kMaxPostingKop)                             \
    X(money, std::int32_t, refund_void_permille, 1'000, 0, 1'000)                                        \
    X(money, std::int32_t, refund_withdraw_permille, 0, 0, 1'000)
// clang-format on

/// Balance numbers; member names are the keys of the blocks of data/balance/court.pd.
struct CourtBalance {
#define PAT_COURT_FIELD_DECL(group, type, name, def, lo, hi) type name = def;
    PAT_COURT_BALANCE_FIELDS(PAT_COURT_FIELD_DECL)
#undef PAT_COURT_FIELD_DECL

    [[nodiscard]] bool operator==(const CourtBalance&) const = default;
};

/// Number of balance fields (for the consistency test with the schema).
inline constexpr std::size_t kCourtBalanceFieldCount = 0
#define PAT_COURT_FIELD_COUNT(group, type, name, def, lo, hi) +1
    PAT_COURT_BALANCE_FIELDS(PAT_COURT_FIELD_COUNT)
#undef PAT_COURT_FIELD_COUNT
    ;

struct CourtRules {
    std::vector<OfficeDef> offices;        ///< ascending `slot`
    std::vector<ServantRoleDef> servants;  ///< ascending `slot`
    std::vector<CourtPartyDef> parties;    ///< ascending `slot`
    CourtBalance balance;

    /// The default rules: the one table of numeric constants (court_rules.cpp and the list above).
    [[nodiscard]] static CourtRules defaults();

    [[nodiscard]] const OfficeDef* find_office(std::uint8_t slot) const noexcept;
    [[nodiscard]] const ServantRoleDef* find_servant_role(std::uint8_t slot) const noexcept;
    [[nodiscard]] const CourtPartyDef* find_party(std::uint8_t slot) const noexcept;

    [[nodiscard]] bool operator==(const CourtRules&) const = default;
};

struct CourtRulesError {
    std::string message;
    /// Stable path for content diagnostics: `<office|servant|party> <id>.<field>` or `<group>.<name>`.
    std::string field;

    [[nodiscard]] bool operator==(const CourtRulesError&) const = default;
};

/// Checks a rules instance and returns the first problem: unique ascending slots, consistent seats,
/// date windows, requirement and weight corridors, ordered favor thresholds, balance corridors. Rules
/// that pass can be used by the formulas without further checks.
[[nodiscard]] core::Result<void, CourtRulesError> validate(const CourtRules& rules);

}  // namespace pat::sim
