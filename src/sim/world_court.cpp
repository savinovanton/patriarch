// World-owned court tables (E10-T02, docs/dev/court-model.md sections 4, 12 and 20.4) and the influence
// balances (docs/dev/influence-model.md section 4): the storage API, the derived indexes and the
// structural invariants C-1, C-2, C-4...C-9, C-12, C-13, C-16 and I-1...I-3. No rules here: the invariants
// that need CourtRules are check_court_invariants() (court_invariants.cpp); the commands and systems are
// E10-T05 and later; persistence and hashing are E10-T03.

#include "pat/sim/court/portfolio.hpp"
#include "pat/sim/court/tables.hpp"
#include "pat/sim/influence.hpp"
#include "pat/sim/world.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <limits>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "sorted_rows.hpp"

namespace pat::sim {
namespace {

using detail::append_ring;
using detail::find_row;
using detail::kMaxCounter;
using detail::ordered_unique;
using detail::row_position;

using Seat = std::pair<court::OfficeTypeId, std::uint32_t>;

Seat seat_of(const court::Office& row) {
    return {row.type, row.locus};
}

bool date_or_none(Date date) {
    return date == kNoDate || is_sane_date(date);
}

bool type_ok(court::OfficeTypeId type) {
    return type >= 1 && type <= court::kMaxOfficeTypeSlot;
}

template <class Enum>
bool enum_in(Enum value, std::uint8_t first, std::uint8_t last) {
    const auto raw = static_cast<std::uint8_t>(value);
    return raw >= first && raw <= last;
}

bool decision_shape(const court::Decision& row) {
    if (!type_ok(row.type) || !enum_in(row.result, 1, court::kLastDecisionResult) ||
        !is_sane_date(row.decided) || row.n > court::kDecisionContenders) {
        return false;
    }
    for (std::size_t i = 0; i < row.contenders.size(); ++i) {
        const court::Contender& contender = row.contenders[i];
        if (i >= row.n) {
            if (!(contender == court::Contender{})) {
                return false;  // hidden state beyond the counted contenders
            }
            continue;
        }
        std::int64_t sum = 0;
        for (const std::int32_t term : contender.terms) {
            sum += term;
        }
        if (sum != contender.score) {
            return false;  // C-9: the terms explain the score exactly
        }
    }
    return true;
}

bool order_shape(const court::PortfolioOrder& row) {
    return type_ok(row.type) && row.order < court::kMaxOrdersPerPortfolio && is_sane_date(row.date) &&
           row.cost_kop >= 0 && row.cost_favor_x100 >= 0 && row.power_permille >= 0 && row.noise_x100 >= 0 &&
           row.execution_permille >= 0 && row.execution_permille <= court::kMaxExecutionPermille;
}

bool portfolio_shape(const court::Portfolio& row) {
    if (!type_ok(row.type) || row.course >= court::kMaxCourses || !date_or_none(row.course_since) ||
        !enum_in(row.state, 1, court::kLastPortfolioState) || !date_or_none(row.next_step) ||
        (row.flags & ~court::kPortfolioFlagMask) != 0) {
        return false;
    }
    if ((row.next_step == kNoDate) != (row.state == court::PortfolioState::Active)) {
        return false;
    }
    return std::ranges::all_of(row.order_ready, date_or_none);
}

bool favor_shape(const court::Favor& row) {
    return row.favor_x100 >= court::kMinFavorX100 && row.favor_x100 <= court::kMaxFavorX100 &&
           static_cast<std::uint8_t>(row.party) <= court::kLastParty && date_or_none(row.party_support_on) &&
           row.gift_month >= -1 && row.gift_points_x100 >= 0 && row.pressure_band <= court::kMaxPressureBand;
}

std::pair<court::OfficeTypeId, std::uint32_t> portfolio_key(const court::Portfolio& row) {
    return {row.type, row.locus};
}

court::Indexes build_indexes(const court::State& state) {
    court::Indexes index;
    for (const court::Office& row : state.offices) {
        index.office_by_holder.emplace_back(row.holder, row.id);
        index.office_by_seat.emplace_back(seat_of(row), row.id);
        if ((row.flags & court::kOfficeProvinceSeat) != 0) {
            index.offices_by_province.emplace_back(ProvinceId(row.locus), row.id);
        }
    }
    for (std::size_t i = 0; i < state.servants.size(); ++i) {
        index.servant_by_role.push_back(
            {state.servants.dynasty[i], state.servants.role[i], state.servants.id[i]});
    }
    std::ranges::sort(index.office_by_holder);
    std::ranges::sort(index.office_by_seat);
    std::ranges::sort(index.offices_by_province);
    std::ranges::sort(index.servant_by_role);
    return index;
}

template <class Pairs, class Value>
void insert_sorted(Pairs& pairs, const Value& value) {
    pairs.insert(std::ranges::lower_bound(pairs, value), value);
}

template <class Pairs, class Value>
void erase_sorted(Pairs& pairs, const Value& value) {
    const auto position = std::ranges::lower_bound(pairs, value);
    if (position != pairs.end() && *position == value) {
        pairs.erase(position);
    }
}

/// Office ids of the pairs whose first element equals `key` (the pairs are sorted).
template <class Pairs, class Key>
std::vector<court::OfficeId> ids_with_key(const Pairs& pairs, const Key& key) {
    std::vector<court::OfficeId> ids;
    auto position = std::ranges::lower_bound(pairs, key, {}, [](const auto& pair) { return pair.first; });
    for (; position != pairs.end() && position->first == key; ++position) {
        ids.push_back(position->second);
    }
    return ids;
}

}  // namespace

// ---- offices --------------------------------------------------------------------------------------------

std::optional<court::OfficeId> World::add_court_office(court::Office row) {
    if (!dynasty_mode() || !is_sane_date(today()) || court_.offices.size() >= court::kMaxOffices ||
        court_.meta.next_office_id == kMaxCounter) {
        return std::nullopt;
    }
    if (!type_ok(row.type) || (row.flags & ~court::kOfficeFlagMask) != 0 || !is_alive(row.holder) ||
        !characters_.dynasty[row.holder.index()].valid() ||
        (row.appointer.valid() && !exists(row.appointer)) || (row.patron.valid() && !exists(row.patron)) ||
        !is_sane_date(row.since) || row.since > today()) {
        return std::nullopt;
    }
    if ((row.flags & court::kOfficeProvinceSeat) != 0 && !exists(ProvinceId(row.locus))) {
        return std::nullopt;
    }
    if (court_office_at(row.type, row.locus) != nullptr || court_vacancy_at(row.type, row.locus) != nullptr ||
        court_offices_of(row.holder).size() >= court::kMaxOfficesPerHolder) {
        return std::nullopt;
    }
    row.id = court_.meta.next_office_id++;
    court_.offices.push_back(row);  // ids ascend
    court_index_office(row);
    return row.id;
}

bool World::remove_court_office(court::OfficeId id) {
    const court::Office* row = find_court_office(id);
    if (row == nullptr) {
        return false;
    }
    court_unindex_office(*row);
    return detail::erase_row(court_.offices, id, &court::Office::id);
}

const court::Office* World::find_court_office(court::OfficeId id) const {
    return find_row(court_.offices, id, &court::Office::id);
}

const court::Office* World::court_office_at(court::OfficeTypeId type, std::uint32_t locus) const {
    const auto ids = ids_with_key(court_index_.office_by_seat, Seat{type, locus});
    return ids.empty() ? nullptr : find_court_office(ids.front());
}

std::vector<court::OfficeId> World::court_offices_of(CharacterId holder) const {
    return ids_with_key(court_index_.office_by_holder, holder);
}

std::vector<court::OfficeId> World::court_offices_in(ProvinceId province) const {
    return ids_with_key(court_index_.offices_by_province, province);
}

void World::court_index_office(const court::Office& row) {
    insert_sorted(court_index_.office_by_holder, std::pair{row.holder, row.id});
    insert_sorted(court_index_.office_by_seat, std::pair{seat_of(row), row.id});
    if ((row.flags & court::kOfficeProvinceSeat) != 0) {
        insert_sorted(court_index_.offices_by_province, std::pair{ProvinceId(row.locus), row.id});
    }
}

void World::court_unindex_office(const court::Office& row) {
    erase_sorted(court_index_.office_by_holder, std::pair{row.holder, row.id});
    erase_sorted(court_index_.office_by_seat, std::pair{seat_of(row), row.id});
    erase_sorted(court_index_.offices_by_province, std::pair{ProvinceId(row.locus), row.id});
}

// ---- disgraces --------------------------------------------------------------------------------------------

bool World::set_court_disgrace(const court::Disgrace& row) {
    if (!dynasty_mode() || !is_sane_date(today()) || !is_alive(row.character) || !is_sane_date(row.until) ||
        row.until <= today() || !enum_in(row.reason, 1, court::kLastDisgraceReason)) {
        return false;
    }
    if (court_disgrace(row.character) == nullptr && court_.disgraces.size() >= court::kMaxDisgraces) {
        return false;
    }
    detail::replace_row(court_.disgraces, row, &court::Disgrace::character);
    return true;
}

bool World::clear_court_disgrace(CharacterId character) {
    return detail::erase_row(court_.disgraces, character, &court::Disgrace::character);
}

const court::Disgrace* World::court_disgrace(CharacterId character) const {
    return find_row(court_.disgraces, character, &court::Disgrace::character);
}

// ---- vacancies and decisions ---------------------------------------------------------------------------

bool World::court_vacancy_ok(const court::Vacancy& row) const {
    return dynasty_mode() && type_ok(row.type) && is_sane_date(row.opened) && row.opened <= today() &&
           is_sane_date(row.decide_by) && row.decide_by >= row.opened &&
           enum_in(row.reason, 1, court::kLastVacancyReason) &&
           (!row.previous_holder.valid() || exists(row.previous_holder)) &&
           row.postponed <= court::kMaxPostponements;
}

std::optional<court::VacancyId> World::add_court_vacancy(court::Vacancy row) {
    if (!court_vacancy_ok(row) || court_.vacancies.size() >= court::kMaxVacancies ||
        court_.meta.next_vacancy_id == kMaxCounter || court_office_at(row.type, row.locus) != nullptr ||
        court_vacancy_at(row.type, row.locus) != nullptr) {
        return std::nullopt;
    }
    row.id = court_.meta.next_vacancy_id++;
    court_.vacancies.push_back(row);  // ids ascend
    return row.id;
}

bool World::update_court_vacancy(const court::Vacancy& row) {
    const auto position = row_position(court_.vacancies, row.id, &court::Vacancy::id);
    if (position == court_.vacancies.end() || position->id != row.id || position->type != row.type ||
        position->locus != row.locus || !court_vacancy_ok(row)) {
        return false;
    }
    *position = row;
    return true;
}

bool World::remove_court_vacancy(court::VacancyId id) {
    if (std::ranges::any_of(court_.petitions,
                            [id](const court::Petition& row) { return row.vacancy == id; })) {
        return false;
    }
    return detail::erase_row(court_.vacancies, id, &court::Vacancy::id);
}

const court::Vacancy* World::find_court_vacancy(court::VacancyId id) const {
    return find_row(court_.vacancies, id, &court::Vacancy::id);
}

const court::Vacancy* World::court_vacancy_at(court::OfficeTypeId type, std::uint32_t locus) const {
    const auto found = std::ranges::find_if(
        court_.vacancies, [&](const court::Vacancy& row) { return row.type == type && row.locus == locus; });
    return found == court_.vacancies.end() ? nullptr : &*found;
}

std::optional<court::DecisionSeq> World::append_court_decision(court::Decision row) {
    if (!dynasty_mode() || !decision_shape(row)) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < row.n; ++i) {
        const court::Contender& contender = row.contenders[i];
        if (!exists(contender.candidate) || !exists(contender.dynasty)) {
            return std::nullopt;
        }
    }
    return append_ring(court_.decisions, court::kDecisionRingSize, court_.meta.next_decision_seq, row);
}

// ---- petitions ------------------------------------------------------------------------------------------

std::optional<court::PetitionId> World::add_court_petition(court::Petition row) {
    if (!dynasty_mode() || !is_sane_date(today()) || court_.petitions.size() >= court::kMaxPetitions ||
        court_.meta.next_petition_id == kMaxCounter || find_court_vacancy(row.vacancy) == nullptr ||
        !exists(row.petitioner) || !is_alive(row.candidate) ||
        characters_.dynasty[row.candidate.index()] != row.petitioner || !is_sane_date(row.submitted) ||
        row.submitted > today() || row.gift_kop < 0 || (row.flags & ~court::kPetitionFlagMask) != 0) {
        return std::nullopt;
    }
    std::size_t of_dynasty = 0;
    for (const court::Petition& other : court_.petitions) {
        if (other.vacancy == row.vacancy &&
            (other.petitioner == row.petitioner || other.candidate == row.candidate)) {
            return std::nullopt;
        }
        of_dynasty += other.petitioner == row.petitioner ? 1 : 0;
    }
    if (of_dynasty >= court::kMaxPetitionsPerDynasty) {
        return std::nullopt;
    }
    row.id = court_.meta.next_petition_id++;
    court_.petitions.push_back(row);  // ids ascend
    return row.id;
}

bool World::remove_court_petition(court::PetitionId id) {
    return detail::erase_row(court_.petitions, id, &court::Petition::id);
}

const court::Petition* World::find_court_petition(court::PetitionId id) const {
    return find_row(court_.petitions, id, &court::Petition::id);
}

// ---- servants -------------------------------------------------------------------------------------------

bool World::court_servant_ok(const court::Servant& row) const {
    return dynasty_mode() && exists(row.dynasty) && enum_in(row.role, 1, court::kLastServantRole) &&
           row.sex <= static_cast<std::uint8_t>(Sex::Female) && row.skill <= court::kMaxServantSkill &&
           row.loyalty <= court::kMaxServantLoyalty;
}

std::optional<std::size_t> World::court_servant_index(court::ServantId id) const {
    const auto& ids = court_.servants.id;
    const auto position = std::ranges::lower_bound(ids, id);
    if (position == ids.end() || *position != id) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(position - ids.begin());
}

std::optional<court::ServantId> World::add_court_servant(court::Servant row) {
    court::ServantTable& t = court_.servants;
    if (!court_servant_ok(row) || t.size() >= court::kMaxServants ||
        court_.meta.next_servant_id == kMaxCounter) {
        return std::nullopt;
    }
    row.id = court_.meta.next_servant_id++;
    t.id.push_back(row.id);  // ids ascend
    t.dynasty.push_back(row.dynasty);
    t.name.push_back(row.name);
    t.birth_year.push_back(row.birth_year);
    t.hired_month.push_back(row.hired_month);
    t.role.push_back(row.role);
    t.sex.push_back(row.sex);
    t.skill.push_back(row.skill);
    t.loyalty.push_back(row.loyalty);
    t.wage_kop.push_back(row.wage_kop);
    t.arrears.push_back(row.arrears);
    t.portrait.push_back(row.portrait);
    insert_sorted(court_index_.servant_by_role, court::ServantKey{row.dynasty, row.role, row.id});
    return row.id;
}

bool World::update_court_servant(const court::Servant& row) {
    const auto index = court_servant_index(row.id);
    if (!index || !court_servant_ok(row)) {
        return false;
    }
    court::ServantTable& t = court_.servants;
    const std::size_t i = *index;
    if (t.dynasty[i] != row.dynasty || t.role[i] != row.role) {
        erase_sorted(court_index_.servant_by_role, court::ServantKey{t.dynasty[i], t.role[i], row.id});
        insert_sorted(court_index_.servant_by_role, court::ServantKey{row.dynasty, row.role, row.id});
    }
    t.dynasty[i] = row.dynasty;
    t.name[i] = row.name;
    t.birth_year[i] = row.birth_year;
    t.hired_month[i] = row.hired_month;
    t.role[i] = row.role;
    t.sex[i] = row.sex;
    t.skill[i] = row.skill;
    t.loyalty[i] = row.loyalty;
    t.wage_kop[i] = row.wage_kop;
    t.arrears[i] = row.arrears;
    t.portrait[i] = row.portrait;
    return true;
}

bool World::remove_court_servant(court::ServantId id) {
    const auto index = court_servant_index(id);
    if (!index) {
        return false;
    }
    court::ServantTable& t = court_.servants;
    const auto at = static_cast<std::ptrdiff_t>(*index);
    erase_sorted(court_index_.servant_by_role, court::ServantKey{t.dynasty[*index], t.role[*index], id});
    t.id.erase(t.id.begin() + at);
    t.dynasty.erase(t.dynasty.begin() + at);
    t.name.erase(t.name.begin() + at);
    t.birth_year.erase(t.birth_year.begin() + at);
    t.hired_month.erase(t.hired_month.begin() + at);
    t.role.erase(t.role.begin() + at);
    t.sex.erase(t.sex.begin() + at);
    t.skill.erase(t.skill.begin() + at);
    t.loyalty.erase(t.loyalty.begin() + at);
    t.wage_kop.erase(t.wage_kop.begin() + at);
    t.arrears.erase(t.arrears.begin() + at);
    t.portrait.erase(t.portrait.begin() + at);
    return true;
}

std::optional<court::Servant> World::find_court_servant(court::ServantId id) const {
    const auto index = court_servant_index(id);
    if (!index) {
        return std::nullopt;
    }
    return court_.servants.row(*index);
}

std::vector<court::ServantId> World::court_servants_of(DynastyId dynasty, court::ServantRole role) const {
    std::vector<court::ServantId> ids;
    const auto& keys = court_index_.servant_by_role;
    auto position = std::ranges::lower_bound(keys, court::ServantKey{dynasty, role, 0});
    for (; position != keys.end() && position->dynasty == dynasty && position->role == role; ++position) {
        ids.push_back(position->id);
    }
    return ids;
}

std::size_t World::court_servant_count(DynastyId dynasty) const {
    const auto& keys = court_index_.servant_by_role;
    const auto first =
        std::ranges::lower_bound(keys, dynasty, {}, [](const court::ServantKey& key) { return key.dynasty; });
    const auto last =
        std::ranges::upper_bound(keys, dynasty, {}, [](const court::ServantKey& key) { return key.dynasty; });
    return static_cast<std::size_t>(last - first);
}

// ---- favor, market, portfolios ------------------------------------------------------------------------

bool World::set_court_favor(const court::Favor& row) {
    if (!dynasty_mode() || !exists(row.dynasty) || !favor_shape(row)) {
        return false;
    }
    if (court::is_default(row)) {
        (void)detail::erase_row(court_.favor, row.dynasty, &court::Favor::dynasty);
    } else {
        detail::replace_row(court_.favor, row, &court::Favor::dynasty);
    }
    return true;
}

const court::Favor* World::court_favor_row(DynastyId dynasty) const {
    return find_row(court_.favor, dynasty, &court::Favor::dynasty);
}

std::int32_t World::court_favor_x100(DynastyId dynasty) const {
    if (dynasty.valid() && economy_present() && economy_.meta.crown_dynasty == dynasty) {
        return court::kMaxFavorX100;  // the crown needs no favor (section 4.8)
    }
    const court::Favor* row = court_favor_row(dynasty);
    return row != nullptr ? row->favor_x100 : court::kDefaultFavorX100;
}

bool World::set_court_market(std::int32_t month, std::uint64_t seed) {
    if (!dynasty_mode() || month < -1) {
        return false;
    }
    court_.meta.market_month = month;
    court_.meta.market_seed = seed;
    return true;
}

bool World::set_court_portfolio(const court::Portfolio& row) {
    if (!dynasty_mode() || !portfolio_shape(row) || !exists(row.controller) ||
        (row.set_by.valid() && !exists(row.set_by)) || (row.target.valid() && !exists(row.target))) {
        return false;
    }
    if (find_court_portfolio(row.type, row.locus) == nullptr &&
        court_.portfolios.size() >= court::kMaxPortfolios) {
        return false;
    }
    detail::replace_row(court_.portfolios, row, portfolio_key);
    return true;
}

bool World::remove_court_portfolio(court::OfficeTypeId type, std::uint32_t locus) {
    return detail::erase_row(court_.portfolios, std::pair{type, locus}, portfolio_key);
}

const court::Portfolio* World::find_court_portfolio(court::OfficeTypeId type, std::uint32_t locus) const {
    return find_row(court_.portfolios, std::pair{type, locus}, portfolio_key);
}

std::optional<std::uint32_t> World::append_court_order(court::PortfolioOrder row) {
    if (!dynasty_mode() || !order_shape(row) || row.date > today() || !exists(row.issuer) ||
        (row.holder.valid() && !exists(row.holder))) {
        return std::nullopt;
    }
    return append_ring(court_.orders, court::kOrderRingSize, court_.meta.next_order_seq, row);
}

void World::rebuild_court_indexes() {
    court_index_ = build_indexes(court_);
}

// ---- influence ------------------------------------------------------------------------------------------

std::int32_t World::influence_x100(DynastyId dynasty) const {
    const InfluenceRow* row = find_row(influence_.rows, dynasty, &InfluenceRow::dynasty);
    return row != nullptr ? row->balance_x100 : 0;
}

std::optional<std::int32_t>
World::add_influence(DynastyId dynasty, std::int32_t delta_x100, InfluenceReason reason) {
    if (!dynasty_mode() || !is_sane_date(today()) || !exists(dynasty) ||
        !is_known_influence_reason(static_cast<std::uint16_t>(reason))) {
        return std::nullopt;
    }
    const std::int32_t before = influence_x100(dynasty);
    const auto after = static_cast<std::int32_t>(
        std::clamp<std::int64_t>(std::int64_t{before} + delta_x100, 0, kMaxInfluenceX100));
    const std::int32_t applied = after - before;
    if (applied == 0) {
        return 0;
    }
    if (after == 0) {
        (void)detail::erase_row(influence_.rows, dynasty, &InfluenceRow::dynasty);
    } else {
        detail::replace_row(influence_.rows, InfluenceRow{dynasty, after}, &InfluenceRow::dynasty);
    }
    if (reason != InfluenceReason::Monthly) {
        if (influence_.log.size() >= kInfluenceLogSize) {
            influence_.log.erase(influence_.log.begin());
        }
        influence_.log.push_back({today(), dynasty, applied, reason});
    }
    return applied;
}

// ---- invariants -----------------------------------------------------------------------------------------

void World::check_court_tables(std::vector<std::string>& bad) const {
    const auto fail = [&bad](std::string message) { bad.push_back(std::move(message)); };
    const court::State& c = court_;
    const court::Meta& meta = c.meta;

    if (!dynasty_mode() && !(c == court::State{})) {
        fail("C-1: court tables outside dynasty mode");
    }
    // C-1: sizes, order, counters.
    if (c.offices.size() > court::kMaxOffices || c.vacancies.size() > court::kMaxVacancies ||
        c.petitions.size() > court::kMaxPetitions || c.servants.size() > court::kMaxServants ||
        c.disgraces.size() > court::kMaxDisgraces || c.decisions.size() > court::kDecisionRingSize ||
        c.portfolios.size() > court::kMaxPortfolios || c.orders.size() > court::kOrderRingSize) {
        fail("C-1: a court table exceeds its bound");
    }
    if (!c.servants.columns_consistent()) {
        fail("C-1: servant columns differ in length");
        return;
    }
    if (!ordered_unique(c.offices, &court::Office::id) ||
        !ordered_unique(c.disgraces, &court::Disgrace::character) ||
        !ordered_unique(c.vacancies, &court::Vacancy::id) ||
        !ordered_unique(c.decisions, &court::Decision::seq) ||
        !ordered_unique(c.petitions, &court::Petition::id) ||
        !ordered_unique(c.servants.id, [](court::ServantId id) { return id; }) ||
        !ordered_unique(c.favor, &court::Favor::dynasty) || !ordered_unique(c.portfolios, portfolio_key) ||
        !ordered_unique(c.orders, &court::PortfolioOrder::seq)) {
        fail("C-1: court table keys are not unique and ordered");
        return;  // the lookups below require ordered tables
    }
    if (meta.next_office_id == 0 || meta.next_vacancy_id == 0 || meta.next_petition_id == 0 ||
        meta.next_servant_id == 0 || meta.next_decision_seq == 0 || meta.next_order_seq == 0 ||
        meta.market_month < -1) {
        fail("C-1: bad court counters");
    }
    const auto below = [](const auto& rows, auto key, std::uint32_t next) {
        return rows.empty() || std::invoke(key, rows.back()) < next;
    };
    if (!below(c.offices, &court::Office::id, meta.next_office_id) ||
        !below(c.vacancies, &court::Vacancy::id, meta.next_vacancy_id) ||
        !below(c.petitions, &court::Petition::id, meta.next_petition_id) ||
        !below(c.decisions, &court::Decision::seq, meta.next_decision_seq) ||
        !below(c.orders, &court::PortfolioOrder::seq, meta.next_order_seq) ||
        (!c.servants.empty() && c.servants.id.back() >= meta.next_servant_id)) {
        fail("C-1: a court id is not below its counter");
    }

    // A character a row refers to as a living person: alive, or dead not more than a day ago (the daily
    // court.holders reacts to the death on the next day, D10).
    const auto living_or_just_dead = [&](CharacterId id) {
        return exists(id) && (is_alive(id) || characters_.death_date[id.index()] >= today() - 1);
    };

    // C-2: offices.
    std::map<CharacterId, std::size_t> held;
    std::vector<Seat> seats;
    for (const court::Office& row : c.offices) {
        if (!type_ok(row.type) || (row.flags & ~court::kOfficeFlagMask) != 0 || !is_sane_date(row.since)) {
            fail(std::format("C-2: office {} has a bad type, flags or date", row.id));
        }
        if (!living_or_just_dead(row.holder) || !characters_.dynasty[row.holder.index()].valid()) {
            fail(std::format("C-2: office {}: the holder is not a living member of a dynasty", row.id));
        } else {
            ++held[row.holder];
        }
        if ((row.appointer.valid() && !exists(row.appointer)) ||
            (row.patron.valid() && !exists(row.patron))) {
            fail(std::format("C-2: office {} names an unknown appointer or patron", row.id));
        }
        if ((row.flags & court::kOfficeProvinceSeat) != 0 && !exists(ProvinceId(row.locus))) {
            fail(std::format("C-2: office {} sits in an unknown province", row.id));
        }
        seats.push_back(seat_of(row));
    }
    std::ranges::sort(seats);
    if (std::ranges::adjacent_find(seats) != seats.end()) {
        fail("C-2: a seat is held twice");
    }
    for (const auto& [holder, count] : held) {
        if (count > court::kMaxOfficesPerHolder) {
            fail(std::format("C-2: character {} holds too many seats", holder.value()));
        }
    }

    // C-4: vacancies.
    std::vector<Seat> open;
    for (const court::Vacancy& row : c.vacancies) {
        if (!type_ok(row.type) || !is_sane_date(row.opened) || !is_sane_date(row.decide_by) ||
            row.decide_by < row.opened || !enum_in(row.reason, 1, court::kLastVacancyReason) ||
            (row.previous_holder.valid() && !exists(row.previous_holder)) ||
            row.postponed > court::kMaxPostponements) {
            fail(std::format("C-4: vacancy {} is malformed", row.id));
        }
        if (std::ranges::binary_search(seats, Seat{row.type, row.locus})) {
            fail(std::format("C-4: vacancy {} names a held seat", row.id));
        }
        open.emplace_back(row.type, row.locus);
    }
    std::ranges::sort(open);
    if (std::ranges::adjacent_find(open) != open.end()) {
        fail("C-4: a seat has two vacancies");
    }

    // C-5: petitions.
    std::vector<std::pair<DynastyId, court::VacancyId>> by_petitioner;
    std::vector<std::pair<court::VacancyId, CharacterId>> by_candidate;
    std::map<DynastyId, std::size_t> per_dynasty;
    for (const court::Petition& row : c.petitions) {
        if (find_court_vacancy(row.vacancy) == nullptr || !exists(row.petitioner)) {
            fail(std::format("C-5: petition {} names an unknown vacancy or dynasty", row.id));
        }
        if (!living_or_just_dead(row.candidate) ||
            characters_.dynasty[row.candidate.index()] != row.petitioner) {
            fail(std::format("C-5: petition {}: the candidate is not a living member of the petitioner",
                             row.id));
        }
        if (!is_sane_date(row.submitted) || row.gift_kop < 0 ||
            (row.flags & ~court::kPetitionFlagMask) != 0) {
            fail(std::format("C-5: petition {} is malformed", row.id));
        }
        by_petitioner.emplace_back(row.petitioner, row.vacancy);
        by_candidate.emplace_back(row.vacancy, row.candidate);
        if (++per_dynasty[row.petitioner] == court::kMaxPetitionsPerDynasty + 1) {
            fail(std::format("C-5: dynasty {} has too many petitions", row.petitioner.value()));
        }
    }
    std::ranges::sort(by_petitioner);
    std::ranges::sort(by_candidate);
    if (std::ranges::adjacent_find(by_petitioner) != by_petitioner.end() ||
        std::ranges::adjacent_find(by_candidate) != by_candidate.end()) {
        fail("C-5: duplicate petitions");
    }

    // C-6: servants.
    for (std::size_t i = 0; i < c.servants.size(); ++i) {
        if (!court_servant_ok(c.servants.row(i))) {
            fail(std::format("C-6: servant {} is malformed or serves an unknown dynasty", c.servants.id[i]));
        }
    }

    // C-7: favor.
    for (const court::Favor& row : c.favor) {
        if (!exists(row.dynasty) || !favor_shape(row) || court::is_default(row)) {
            fail(std::format("C-7: bad favor row of dynasty {}", row.dynasty.value()));
        }
    }

    // C-8: disgraces (the trait `disgraced` is a rule: check_court_invariants).
    for (const court::Disgrace& row : c.disgraces) {
        if (!exists(row.character) || !is_sane_date(row.until) || row.until <= today() ||
            !enum_in(row.reason, 1, court::kLastDisgraceReason)) {
            fail(std::format("C-8: bad disgrace of character {}", row.character.value()));
        }
    }

    // C-9: decisions.
    for (const court::Decision& row : c.decisions) {
        if (!decision_shape(row)) {
            fail(std::format("C-9: decision {} is malformed", row.seq));
        }
        for (std::size_t i = 0; i < std::min<std::size_t>(row.n, court::kDecisionContenders); ++i) {
            if (!exists(row.contenders[i].candidate) || !exists(row.contenders[i].dynasty)) {
                fail(std::format("C-9: decision {} names an unknown contender", row.seq));
            }
        }
    }

    // C-13: portfolios (structure; the courses of a type are rules of E10-T23).
    for (const court::Portfolio& row : c.portfolios) {
        if (!portfolio_shape(row) || !exists(row.controller) || (row.set_by.valid() && !exists(row.set_by)) ||
            (row.target.valid() && !exists(row.target))) {
            fail(std::format("C-13: bad portfolio of seat ({}, {})", row.type, row.locus));
        }
    }
    // C-16: the ring of orders.
    for (const court::PortfolioOrder& row : c.orders) {
        if (!order_shape(row) || !exists(row.issuer) || (row.holder.valid() && !exists(row.holder))) {
            fail(std::format("C-16: order {} is malformed", row.seq));
        }
    }

    // C-12: the derived indexes equal a rebuild.
    if (!(court_index_ == build_indexes(c))) {
        fail("C-12: the court indexes differ from a rebuild");
    }
}

void World::check_influence_invariants(std::vector<std::string>& bad) const {
    const auto fail = [&bad](std::string message) { bad.push_back(std::move(message)); };
    if (!dynasty_mode() && !(influence_ == InfluenceState{})) {
        fail("I-1: influence outside dynasty mode");
    }
    // I-1, I-2: rows.
    if (!ordered_unique(influence_.rows, &InfluenceRow::dynasty)) {
        fail("I-1: influence rows are not unique and ordered");
    }
    for (const InfluenceRow& row : influence_.rows) {
        if (!exists(row.dynasty) || row.balance_x100 < 1 || row.balance_x100 > kMaxInfluenceX100) {
            fail(std::format("I-2: bad influence balance of dynasty {}", row.dynasty.value()));
        }
    }
    // I-3: the ring.
    if (influence_.log.size() > kInfluenceLogSize) {
        fail("I-3: the influence log exceeds its bound");
    }
    for (std::size_t i = 0; i < influence_.log.size(); ++i) {
        const InfluenceLogEntry& entry = influence_.log[i];
        const auto reason = static_cast<std::uint16_t>(entry.reason);
        if (!is_sane_date(entry.date) || !exists(entry.dynasty) || !is_known_influence_reason(reason) ||
            entry.reason == InfluenceReason::Monthly || entry.delta_x100 == 0 ||
            (i > 0 && entry.date < influence_.log[i - 1].date)) {
            fail(std::format("I-3: bad influence log entry {}", i));
        }
    }
}

}  // namespace pat::sim
