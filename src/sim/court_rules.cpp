#include "pat/sim/court_rules.hpp"

#include "pat/sim/economy/types.hpp"

#include <algorithm>
#include <format>
#include <string_view>
#include <utility>

namespace pat::sim {

namespace {

// ==================================================================================================
// THE TABLE OF DEFAULT OFFICES, SERVANT ROLES AND PARTIES
//
// The numbers of the balance are the PAT_COURT_BALANCE_FIELDS list of the header. The twelve offices
// and the favourite follow docs/dev/court-model.md section 11.2 (E10-Q2, #360); every number is a
// [predl.] proposal. data/court/*.pd repeats these values and a test keeps the two equal.
// ==================================================================================================

// Slots of data/traits/m1.pd (stable; a test checks them against the file).
constexpr std::uint16_t kTraitDimwitted = 21;
constexpr std::uint16_t kTraitDisgraced = 23;
constexpr std::uint16_t kTraitEmperorFavorite = 24;

// Skill indices (Skill of world.hpp; a test pins them).
constexpr std::size_t kStewardship = 0;
constexpr std::size_t kDiplomacy = 1;
constexpr std::size_t kMartial = 2;
constexpr std::size_t kIntrigue = 3;
constexpr std::size_t kLearning = 4;

constexpr std::uint32_t kind_bit(economy::ProvinceKind kind) {
    return 1U << static_cast<unsigned>(kind);
}

using SkillArray = std::array<std::int32_t, kCourtSkillCount>;
using MinSkillArray = std::array<std::uint8_t, kCourtSkillCount>;

struct OfficeSpec {
    std::uint8_t slot;
    std::string_view id;
    OfficeGroup group;
    OfficeSeats seats;
    std::int32_t min_age;
    std::int32_t min_honor;
    MinSkillArray min_skill;
    SkillArray weights;
    economy::Kopecks salary_kop;
    std::int32_t honor_on_assign;
    std::int32_t honor_monthly;
    std::int32_t decision_delay_days;
    std::int32_t administrator_priority;
    bool exclude_dimwitted_and_disgraced;
    Date from_date;
    std::uint16_t grants_trait;
    std::int32_t council_vote_weight;
};

constexpr OfficeSeats fixed_seats(std::int32_t count) {
    return {SeatRule::Fixed, count, 0, 0};
}

// clang-format off
const std::array<OfficeSpec, 13> kOfficeSpecs{{
    {1, "bedchamberman", OfficeGroup::Court, fixed_seats(2), 16, 100, {0, 0, 0, 0, 0},
     {0, 2, 0, 2, 0}, 600, 10, 1, 90, 0, false, kNoDate, 0, 0},
    {2, "orderly", OfficeGroup::Court, fixed_seats(2), 16, 0, {0, 0, 6, 0, 0},
     {0, 0, 4, 1, 0}, 500, 10, 1, 90, 0, false, kNoDate, 0, 0},
    {3, "tsarevich_tutor", OfficeGroup::Court, fixed_seats(1), 30, 500, {0, 0, 0, 0, 10},
     {0, 1, 0, 0, 4}, 1200, 10, 1, 90, 0, true, kNoDate, 0, 0},
    {4, "court_steward", OfficeGroup::Court, fixed_seats(1), 25, 100, {10, 0, 0, 0, 0},
     {4, 0, 0, 1, 0}, 1500, 10, 1, 90, 0, false, kNoDate, 0, 0},
    {5, "voivode", OfficeGroup::Regional,
     {SeatRule::PerCrownProvince, 0, 64,
      kind_bit(economy::ProvinceKind::Rural) | kind_bit(economy::ProvinceKind::City)},
     25, 0, {8, 0, 0, 0, 0}, {4, 0, 1, 0, 0}, 1000, 5, 0, 30, 2, false, kNoDate, 0, 0},
    {6, "governor", OfficeGroup::Regional, {SeatRule::Listed, 0, 8, 0}, 30, 100, {10, 0, 0, 0, 0},
     {4, 1, 1, 0, 0}, 2500, 5, 0, 30, 3, false, Date::from_ymd(1708, 12, 18), 0, 0},
    {7, "fortress_commandant", OfficeGroup::Regional,
     {SeatRule::PerCrownProvince, 0, 32, kind_bit(economy::ProvinceKind::City)}, 25, 0,
     {0, 0, 8, 0, 0}, {1, 0, 4, 0, 0}, 800, 5, 0, 30, 1, false, kNoDate, 0, 0},
    {8, "guards_colonel", OfficeGroup::Military, fixed_seats(2), 25, 100, {0, 0, 12, 0, 0},
     {0, 0, 4, 1, 0}, 2000, 10, 1, 60, 0, false, kNoDate, 0, 0},
    {9, "general", OfficeGroup::Military, fixed_seats(3), 30, 500, {0, 0, 14, 0, 0},
     {1, 0, 5, 0, 0}, 3000, 10, 1, 60, 0, false, kNoDate, 0, 0},
    {10, "ambassadorial_chief", OfficeGroup::Chancery, fixed_seats(1), 30, 100, {0, 12, 0, 0, 0},
     {1, 4, 0, 0, 0}, 3500, 10, 1, 60, 0, true, kNoDate, 0, 0},
    {11, "treasurer", OfficeGroup::Chancery, fixed_seats(1), 30, 100, {12, 0, 0, 0, 0},
     {5, 0, 0, 1, 0}, 3500, 10, 1, 60, 0, false, kNoDate, 0, 0},
    {12, "duma_boyar", OfficeGroup::Council, fixed_seats(6), 30, 500, {0, 0, 0, 0, 0},
     {1, 2, 0, 0, 1}, 2000, 10, 1, 90, 0, false, kNoDate, 0, 1},
    {13, "favourite", OfficeGroup::Favourite, fixed_seats(1), 16, 0, {0, 0, 0, 0, 0},
     {0, 0, 0, 0, 0}, 0, 0, 0, 30, 0, false, kNoDate, kTraitEmperorFavorite, 0},
}};
// clang-format on

OfficeDef make_office(const OfficeSpec& spec) {
    OfficeDef def;
    def.slot = spec.slot;
    def.id = std::string(spec.id);
    def.name_key = std::format("office.{}.name", spec.id);
    def.desc_key = std::format("office.{}.desc", spec.id);
    def.group = spec.group;
    def.appointer = OfficeAppointer::Emperor;
    def.seats = spec.seats;
    def.requirements.min_age = spec.min_age;
    def.requirements.sex_allowed = SexRule::Male;
    def.requirements.min_honor = spec.min_honor;
    def.requirements.min_skill = spec.min_skill;
    if (spec.exclude_dimwitted_and_disgraced) {
        def.requirements.not_traits = {kTraitDimwitted, kTraitDisgraced};
    }
    def.skill_weights = spec.weights;
    def.salary_kop = spec.salary_kop;
    def.honor_on_assign = spec.honor_on_assign;
    def.honor_monthly = spec.honor_monthly;
    def.decision_delay_days = spec.decision_delay_days;
    def.from_date = spec.from_date;
    def.administrator_priority = spec.administrator_priority;
    def.grants_trait = spec.grants_trait;
    def.council_vote_weight = spec.council_vote_weight;
    return def;
}

ServantRoleDef make_role(std::uint8_t slot, std::string_view id, std::size_t skill) {
    ServantRoleDef def;
    def.slot = slot;
    def.id = std::string(id);
    def.name_key = std::format("servant.role.{}.name", id);
    def.effect_key = std::format("servant.role.{}.effect", id);
    def.skill = static_cast<std::uint8_t>(skill);
    def.slots = 1;
    def.skill_min = 6;
    def.skill_max = 15;
    def.wage_base_kop = 500;
    def.wage_per_skill_kop = 100;
    def.fee_months = 2;
    return def;
}

CourtPartyDef make_party(std::uint8_t slot, std::string_view id) {
    CourtPartyDef def;
    def.slot = slot;
    def.id = std::string(id);
    def.name_key = std::format("court.party.{}.name", id);
    def.agenda_key = std::format("court.party.{}.agenda", id);
    return def;
}

// ---- validation helpers ------------------------------------------------------------------------

using Check = core::Result<void, CourtRulesError>;

Check fail(std::string message, std::string field) {
    return Check::err(CourtRulesError{std::move(message), std::move(field)});
}

Check in_range(std::string_view name, std::int64_t value, std::int64_t lo, std::int64_t hi) {
    if (value < lo || value > hi) {
        return fail(std::format("{} = {} is out of range {}...{}", name, value, lo, hi), std::string(name));
    }
    return Check::ok();
}

#define PAT_COURT_CHECK(expr)                                                                                \
    do {                                                                                                     \
        if (auto res_ = (expr); !res_)                                                                       \
            return res_;                                                                                     \
    } while (false)

std::string path(std::string_view what, const std::string& id, std::string_view field) {
    return std::format("{} {}.{}", what, id, field);
}

Check check_seats(const OfficeDef& o) {
    const auto& s = o.seats;
    const auto seats_path = path("office", o.id, "seats");
    switch (s.rule) {
        case SeatRule::Fixed:
            PAT_COURT_CHECK(in_range(seats_path + ".slots", s.slots, 1, 64));
            if (s.max != 0 || s.kinds != 0) {
                return fail("a fixed seat rule takes `slots` only (no max and no kinds)", seats_path);
            }
            break;
        case SeatRule::PerCrownProvince:
            PAT_COURT_CHECK(in_range(seats_path + ".max", s.max, 1, 4096));
            if (s.kinds == 0 || (s.kinds >> economy::kProvinceKindCount) != 0) {
                return fail("per_crown_province needs a non-empty set of province kinds", seats_path);
            }
            if (s.slots != 0) {
                return fail("per_crown_province takes `max`, not `slots`", seats_path);
            }
            break;
        case SeatRule::Listed:
            PAT_COURT_CHECK(in_range(seats_path + ".max", s.max, 1, 4096));
            if (s.slots != 0 || s.kinds != 0) {
                return fail("listed seats take `max` only", seats_path);
            }
            break;
    }
    return Check::ok();
}

Check check_office(const OfficeDef& o) {
    PAT_COURT_CHECK(in_range(path("office", o.id, "slot"), o.slot, 1, kMaxOfficeTypes));
    if (o.id.empty()) {
        return fail("an office needs an id", std::format("office {}", o.slot));
    }
    PAT_COURT_CHECK(check_seats(o));
    const auto& r = o.requirements;
    PAT_COURT_CHECK(in_range(path("office", o.id, "requirements.min_age"), r.min_age, 0, 100));
    PAT_COURT_CHECK(
        in_range(path("office", o.id, "requirements.min_honor"), r.min_honor, -1'000'000, 1'000'000));
    for (std::size_t i = 0; i < r.min_skill.size(); ++i) {
        PAT_COURT_CHECK(
            in_range(path("office", o.id, std::format("requirements.skills[{}]", i)), r.min_skill[i], 0, 20));
    }
    for (std::size_t i = 0; i < o.skill_weights.size(); ++i) {
        PAT_COURT_CHECK(
            in_range(path("office", o.id, std::format("skill_weights[{}]", i)), o.skill_weights[i], 0, 100));
    }
    PAT_COURT_CHECK(in_range(path("office", o.id, "salary_kop"), o.salary_kop, 0, 100'000'000));
    PAT_COURT_CHECK(in_range(path("office", o.id, "honor_on_assign"), o.honor_on_assign, 0, 10'000));
    PAT_COURT_CHECK(in_range(path("office", o.id, "honor_monthly"), o.honor_monthly, 0, 10'000));
    PAT_COURT_CHECK(in_range(path("office", o.id, "decision_delay_days"), o.decision_delay_days, 1, 365));
    PAT_COURT_CHECK(
        in_range(path("office", o.id, "administrator_priority"), o.administrator_priority, 0, 100));
    PAT_COURT_CHECK(in_range(path("office", o.id, "council_vote_weight"), o.council_vote_weight, 0, 100));
    if (o.from_date != kNoDate && o.to_date != kNoDate && o.to_date < o.from_date) {
        return fail("to_date must not be earlier than from_date", path("office", o.id, "to_date"));
    }
    if (o.group == OfficeGroup::Regional && o.seats.rule == SeatRule::Fixed) {
        return fail("a regional office needs per_crown_province or listed seats",
                    path("office", o.id, "seats"));
    }
    if (o.group != OfficeGroup::Regional && o.seats.rule != SeatRule::Fixed) {
        return fail("only regional offices may use per_crown_province or listed seats",
                    path("office", o.id, "seats"));
    }
    if (o.group == OfficeGroup::Favourite && (o.seats.rule != SeatRule::Fixed || o.seats.slots != 1)) {
        return fail("the favourite has exactly one seat", path("office", o.id, "seats"));
    }
    if (o.administrator_priority != 0 && o.group != OfficeGroup::Regional) {
        return fail("only regional offices administer provinces",
                    path("office", o.id, "administrator_priority"));
    }
    return Check::ok();
}

Check check_offices(const std::vector<OfficeDef>& offices) {
    if (offices.size() > kMaxOfficeTypes) {
        return fail(std::format("at most {} office types are supported", kMaxOfficeTypes), "offices");
    }
    std::size_t favourites = 0;
    for (std::size_t i = 0; i < offices.size(); ++i) {
        PAT_COURT_CHECK(check_office(offices[i]));
        if (i > 0 && offices[i].slot <= offices[i - 1].slot) {
            return fail(std::format("office slot {} must be above the previous slot {}",
                                    offices[i].slot,
                                    offices[i - 1].slot),
                        path("office", offices[i].id, "slot"));
        }
        favourites += offices[i].group == OfficeGroup::Favourite ? 1U : 0U;
    }
    if (favourites > 1) {
        return fail("at most one office may belong to the favourite group", "offices");
    }
    return Check::ok();
}

Check check_servants(const std::vector<ServantRoleDef>& roles) {
    if (roles.size() > kMaxServantRoles) {
        return fail(std::format("at most {} servant roles are supported", kMaxServantRoles), "servants");
    }
    for (std::size_t i = 0; i < roles.size(); ++i) {
        const auto& r = roles[i];
        PAT_COURT_CHECK(in_range(path("servant_role", r.id, "slot"), r.slot, 1, kMaxServantRoles));
        if (i > 0 && r.slot <= roles[i - 1].slot) {
            return fail(std::format("servant role slot {} must be above the previous slot {}",
                                    r.slot,
                                    roles[i - 1].slot),
                        path("servant_role", r.id, "slot"));
        }
        PAT_COURT_CHECK(in_range(path("servant_role", r.id, "skill"), r.skill, 0, kCourtSkillCount - 1));
        PAT_COURT_CHECK(in_range(path("servant_role", r.id, "slots"), r.slots, 1, 16));
        PAT_COURT_CHECK(in_range(path("servant_role", r.id, "skill_min"), r.skill_min, 0, 20));
        PAT_COURT_CHECK(in_range(path("servant_role", r.id, "skill_max"), r.skill_max, 0, 20));
        if (r.skill_min > r.skill_max) {
            return fail("skill_min must not exceed skill_max", path("servant_role", r.id, "skill_min"));
        }
        PAT_COURT_CHECK(
            in_range(path("servant_role", r.id, "wage_base_kop"), r.wage_base_kop, 0, 10'000'000));
        PAT_COURT_CHECK(
            in_range(path("servant_role", r.id, "wage_per_skill_kop"), r.wage_per_skill_kop, 0, 1'000'000));
        PAT_COURT_CHECK(in_range(path("servant_role", r.id, "fee_months"), r.fee_months, 0, 120));
    }
    return Check::ok();
}

Check check_parties(const std::vector<CourtPartyDef>& parties) {
    if (parties.size() > kMaxCourtParties) {
        return fail(std::format("at most {} parties are supported", kMaxCourtParties), "parties");
    }
    for (std::size_t i = 0; i < parties.size(); ++i) {
        PAT_COURT_CHECK(
            in_range(path("court_party", parties[i].id, "slot"), parties[i].slot, 1, kMaxCourtParties));
        if (i > 0 && parties[i].slot <= parties[i - 1].slot) {
            return fail(std::format("party slot {} must be above the previous slot {}",
                                    parties[i].slot,
                                    parties[i - 1].slot),
                        path("court_party", parties[i].id, "slot"));
        }
    }
    return Check::ok();
}

Check check_balance(const CourtBalance& b) {
#define PAT_COURT_FIELD_CHECK(group, type, name, def, lo, hi)                                                \
    PAT_COURT_CHECK(in_range(#group "." #name, b.name, lo, hi));
    PAT_COURT_BALANCE_FIELDS(PAT_COURT_FIELD_CHECK)
#undef PAT_COURT_FIELD_CHECK
    if (b.favor_low_x100 >= b.favor_high_x100) {
        return fail("favor.favor_low_x100 must be below favor.favor_high_x100", "favor.favor_low_x100");
    }
    if (b.service_cap_x100 < b.service_per_office_x100) {
        return fail("favor.service_cap_x100 must not be below favor.service_per_office_x100",
                    "favor.service_cap_x100");
    }
    return Check::ok();
}

}  // namespace

CourtRules CourtRules::defaults() {
    CourtRules rules;
    for (const OfficeSpec& spec : kOfficeSpecs) {
        rules.offices.push_back(make_office(spec));
    }
    rules.servants = {make_role(1, "steward", kStewardship),
                      make_role(2, "spy", kIntrigue),
                      make_role(3, "secretary", kLearning),
                      make_role(4, "guard", kMartial)};
    rules.parties = {make_party(1, "old_nobility"), make_party(2, "new_service"), make_party(3, "church")};
    return rules;
}

const OfficeDef* CourtRules::find_office(std::uint8_t slot) const noexcept {
    const auto it = std::ranges::find(offices, slot, &OfficeDef::slot);
    return it != offices.end() ? &*it : nullptr;
}

const ServantRoleDef* CourtRules::find_servant_role(std::uint8_t slot) const noexcept {
    const auto it = std::ranges::find(servants, slot, &ServantRoleDef::slot);
    return it != servants.end() ? &*it : nullptr;
}

const CourtPartyDef* CourtRules::find_party(std::uint8_t slot) const noexcept {
    const auto it = std::ranges::find(parties, slot, &CourtPartyDef::slot);
    return it != parties.end() ? &*it : nullptr;
}

core::Result<void, CourtRulesError> validate(const CourtRules& rules) {
    PAT_COURT_CHECK(check_offices(rules.offices));
    PAT_COURT_CHECK(check_servants(rules.servants));
    PAT_COURT_CHECK(check_parties(rules.parties));
    return check_balance(rules.balance);
}

}  // namespace pat::sim
