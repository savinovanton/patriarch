#include "pat/core/assert.hpp"
#include "pat/sim/world.hpp"

#include <algorithm>
#include <format>
#include <limits>
#include <utility>

// The economy part of World (E9-T02, docs/dev/economy-model.md): enabling, row management, the money
// functions and the invariants. Everything here is integer arithmetic; no random numbers.

namespace pat::sim {

namespace {

using core::Fixed32;
using economy::AccountKind;
using economy::Kopecks;
using economy::OwnerKind;

constexpr Fixed32 kZero = Fixed32::zero();

[[nodiscard]] bool in_range(Fixed32 v, Fixed32 lo, Fixed32 hi) noexcept {
    return v >= lo && v <= hi;
}

[[nodiscard]] std::uint8_t to_u8(OwnerKind kind) noexcept {
    return static_cast<std::uint8_t>(kind);
}

/// Persons in a province whose population cache says `thousands` (Q16.16), truncated and capped.
[[nodiscard]] std::uint32_t persons_from_thousands(Fixed32 thousands) noexcept {
    const std::int64_t persons = (static_cast<std::int64_t>(thousands.raw()) * 1000) / Fixed32::one_raw;
    return static_cast<std::uint32_t>(std::clamp<std::int64_t>(persons, 0, economy::kMaxProvincePersons));
}

}  // namespace

// ---- enabling and rows --------------------------------------------------------------------------

void World::economy_add_dynasty_rows() {
    economy::EconomyState& e = economy_;
    constexpr std::size_t kLines = economy::kLedgerLineCount;
    constexpr std::size_t kRing = economy::kLedgerRingMonths;
    e.dynasties.manpower_reserve.push_back(0);
    e.dynasties.bankrupt.push_back(0);
    economy::AccountTable& a = e.accounts;
    a.treasury_.push_back(0);
    a.opening_.push_back(0);
    a.open_lines_.insert(a.open_lines_.end(), kLines, 0);
    a.ring_opening_.insert(a.ring_opening_.end(), kRing, 0);
    a.ring_lines_.insert(a.ring_lines_.end(), kRing * kLines, 0);
}

void World::economy_add_province_rows(std::size_t index) {
    economy::EconomyState& e = economy_;
    economy::ProvinceEconomy& p = e.provinces;
    p.kind.push_back(static_cast<std::uint8_t>(economy::ProvinceKind::Rural));
    p.owner_kind.push_back(to_u8(provinces_.owner[index].valid() ? OwnerKind::Dynasty : OwnerKind::Unowned));
    p.tax_policy.push_back(0);
    p.recruit_policy.push_back(0);
    p.slots_total.push_back(0);
    p.trade_node.push_back(0);
    p.administrator.emplace_back();
    p.control.push_back(economy::kDefaultControl);
    p.corruption.push_back(kZero);
    p.unrest.push_back(kZero);
    p.labour_base.push_back(0);
    p.grain_stock.push_back(kZero);
    p.recruit_pool.push_back(0);
    p.income_last_month.push_back(0);
    p.last_slot_date.push_back(kNoDate);
    p.capacity.insert(p.capacity.end(), economy::kMaxGoods, kZero);
    p.slot_building.insert(p.slot_building.end(), economy::kMaxSlots, 0);

    // The people already counted in the cache go to the peasants, the mass of the population.
    const std::uint32_t persons = persons_from_thousands(provinces_.population[index]);
    economy::PopulationGroups& g = e.groups;
    for (std::size_t group = 0; group < economy::kGroupCount; ++group) {
        const bool peasants = group == economy::index_of(economy::Group::Peasants);
        g.size.push_back(peasants ? persons : 0);
        g.mood.push_back(kZero);
        g.wealth.push_back(economy::kDefaultWealth);
        g.growth_modifier.push_back(0);
        g.growth_carry.push_back(0);
    }
    refresh_population_cache(ProvinceId(static_cast<std::uint32_t>(index)));
}

bool World::enable_economy(DynastyId crown_dynasty) {
    economy::EconomyState& e = economy_;
    if (e.meta.present != 0) {
        return e.meta.crown_dynasty == crown_dynasty;
    }
    if (crown_dynasty.valid() && !exists(crown_dynasty)) {
        return false;
    }
    e.meta.present = 1;
    e.meta.crown_dynasty = crown_dynasty;
    const Date today_date = today();
    e.meta.open_month_start = Date::from_ymd(today_date.year(), today_date.month(), 1);
    // The crown has a row of its own even when its money is held on a dynasty's account (the row
    // stays zero then), so that the layout is the same in both modes (spec section 5.3).
    economy::AccountTable& a = e.accounts;
    a.treasury_.push_back(0);
    a.opening_.push_back(0);
    a.open_lines_.insert(a.open_lines_.end(), economy::kLedgerLineCount, 0);
    a.ring_opening_.insert(a.ring_opening_.end(), economy::kLedgerRingMonths, 0);
    a.ring_lines_.insert(a.ring_lines_.end(), economy::kLedgerRingMonths * economy::kLedgerLineCount, 0);
    for (std::size_t d = 0; d < dynasties_.size(); ++d) {
        economy_add_dynasty_rows();
    }
    for (std::size_t p = 0; p < provinces_.size(); ++p) {
        economy_add_province_rows(p);
    }
    return true;
}

economy::EconomyColumnsMut World::economy_columns_mut() noexcept {
    economy::EconomyState& e = economy_;
    economy::EconomyColumnsMut out;
    out.mood = e.groups.mood;
    out.wealth = e.groups.wealth;
    out.growth_modifier = e.groups.growth_modifier;
    out.growth_carry = e.groups.growth_carry;
    out.kind = e.provinces.kind;
    out.tax_policy = e.provinces.tax_policy;
    out.recruit_policy = e.provinces.recruit_policy;
    out.slots_total = e.provinces.slots_total;
    out.trade_node = e.provinces.trade_node;
    out.control = e.provinces.control;
    out.corruption = e.provinces.corruption;
    out.unrest = e.provinces.unrest;
    out.labour_base = e.provinces.labour_base;
    out.grain_stock = e.provinces.grain_stock;
    out.recruit_pool = e.provinces.recruit_pool;
    out.income_last_month = e.provinces.income_last_month;
    out.last_slot_date = e.provinces.last_slot_date;
    out.capacity = e.provinces.capacity;
    out.manpower_reserve = e.dynasties.manpower_reserve;
    out.bankrupt = e.dynasties.bankrupt;
    return out;
}

void World::set_economy_rules_hash(std::uint64_t rules_hash) noexcept {
    economy_.meta.rules_hash = rules_hash;
}

// ---- provinces ----------------------------------------------------------------------------------

void World::set_province_development(ProvinceId province, std::uint8_t development) {
    PAT_ASSERT(exists(province), "set_province_development: unknown province");
    if (exists(province)) {
        provinces_.development[province.index()] =
            std::clamp<std::uint8_t>(development, 1, static_cast<std::uint8_t>(economy::kMaxDevelopment));
    }
}

bool World::set_province_owner_kind(ProvinceId province, economy::OwnerKind kind) {
    if (economy_.meta.present == 0 || !exists(province) || kind == OwnerKind::Dynasty ||
        to_u8(kind) >= economy::kOwnerKindCount) {
        return false;
    }
    set_province_owner(province, DynastyId());  // drops the dynasty owner, if any
    economy_.provinces.owner_kind[province.index()] = to_u8(kind);
    return true;
}

std::uint64_t World::population_persons(ProvinceId province) const noexcept {
    if (economy_.meta.present == 0 || !exists(province)) {
        return 0;
    }
    std::uint64_t total = 0;
    for (std::size_t g = 0; g < economy::kGroupCount; ++g) {
        total += economy_.groups.size[province.index() * economy::kGroupCount + g];
    }
    return total;
}

void World::refresh_population_cache(ProvinceId province) noexcept {
    if (economy_.meta.present == 0 || !exists(province)) {
        return;
    }
    const std::uint64_t persons =
        std::min<std::uint64_t>(population_persons(province), economy::kMaxProvincePersons);
    provinces_.population[province.index()] = Fixed32::from_ratio(static_cast<std::int64_t>(persons), 1000);
}

std::uint32_t World::set_group_size(ProvinceId province, economy::Group group, std::uint32_t persons) {
    if (economy_.meta.present == 0 || !exists(province) || economy::index_of(group) >= economy::kGroupCount) {
        return 0;
    }
    const std::size_t row = economy::group_row(province.index(), group);
    const std::uint64_t others = population_persons(province) - economy_.groups.size[row];
    const std::uint64_t room =
        others < economy::kMaxProvincePersons ? economy::kMaxProvincePersons - others : 0;
    const auto stored = static_cast<std::uint32_t>(std::min<std::uint64_t>(persons, room));
    economy_.groups.size[row] = stored;
    refresh_population_cache(province);
    return stored;
}

bool World::set_province_administrator(ProvinceId province, CharacterId administrator) {
    if (economy_.meta.present == 0 || !exists(province)) {
        return false;
    }
    if (administrator.valid() && !is_alive(administrator)) {
        return false;
    }
    economy_.provinces.administrator[province.index()] = administrator;
    return true;
}

void World::economy_remove_administrator(CharacterId id) noexcept {
    if (economy_.meta.present == 0 || !id.valid()) {
        return;
    }
    for (CharacterId& administrator : economy_.provinces.administrator) {
        if (administrator == id) {
            administrator = CharacterId();
        }
    }
}

bool World::set_slot_building(ProvinceId province, std::size_t slot, std::uint8_t building) {
    if (economy_.meta.present == 0 || !exists(province) || slot >= economy::kMaxSlots ||
        slot >= economy_.provinces.slots_total[province.index()]) {
        return false;
    }
    economy_.provinces.slot_building[province.index() * economy::kMaxSlots + slot] = building;
    return true;
}

// ---- money --------------------------------------------------------------------------------------

std::optional<std::size_t> World::resolve_account(economy::AccountRef account) const noexcept {
    if (economy_.meta.present == 0) {
        return std::nullopt;
    }
    if (account.kind == AccountKind::Crown) {
        const DynastyId holder = economy_.meta.crown_dynasty;
        return holder.valid() ? 1 + holder.index() : 0;
    }
    if (account.kind == AccountKind::Dynasty && exists(account.dynasty)) {
        return 1 + account.dynasty.index();
    }
    return std::nullopt;
}

Kopecks World::treasury(economy::AccountRef account) const noexcept {
    const auto row = resolve_account(account);
    return row ? economy_.accounts.treasury_[*row] : 0;
}

Kopecks World::preview_posting(std::size_t account, std::size_t line, Kopecks amount) const noexcept {
    const economy::AccountTable& t = economy_.accounts;
    amount = std::clamp(amount, -economy::kMaxPostingKop, economy::kMaxPostingKop);
    Kopecks applied = economy::apply_posting(t.treasury_[account], amount).applied;
    // The line stays within its own bound too (a line can drift when other lines cancel it out).
    const Kopecks current = t.open_lines_[account * economy::kLedgerLineCount + line];
    const Kopecks target = std::clamp(
        economy::saturating_add(current, applied), -economy::kMaxLedgerLineKop, economy::kMaxLedgerLineKop);
    applied = economy::saturating_sub(target, current);
    return applied;
}

Kopecks World::post_money(economy::AccountRef account, economy::LedgerCode code, Kopecks amount_kop) {
    PAT_ASSERT(economy_.meta.present != 0, "post_money: the world has no economy");
    const auto row = resolve_account(account);
    const auto line = economy::ledger_index(code);
    PAT_ASSERT(row.has_value(), "post_money: unknown account");
    PAT_ASSERT(line.has_value(), "post_money: unissued ledger code");
    PAT_ASSERT(!economy::is_transfer_code(code), "post_money: a transfer code is posted by transfer() only");
    if (!row || !line || amount_kop == 0 || economy::is_transfer_code(code)) {
        return 0;
    }
    return post_line(*row, *line, amount_kop);
}

Kopecks World::post_line(std::size_t row, std::size_t line, Kopecks amount_kop) {
    const Kopecks applied = preview_posting(row, line, amount_kop);
    economy::AccountTable& t = economy_.accounts;
    t.treasury_[row] += applied;
    t.open_lines_[row * economy::kLedgerLineCount + line] += applied;
    return applied;
}

Kopecks World::transfer(economy::AccountRef from,
                        economy::AccountRef to,
                        economy::LedgerCode out_code,
                        economy::LedgerCode in_code,
                        Kopecks amount_kop) {
    PAT_ASSERT(economy_.meta.present != 0, "transfer: the world has no economy");
    PAT_ASSERT(economy::is_ledger_pair(out_code, in_code), "transfer: the codes are not a pair");
    const auto from_row = resolve_account(from);
    const auto to_row = resolve_account(to);
    if (!from_row || !to_row || amount_kop <= 0 || !economy::is_ledger_pair(out_code, in_code)) {
        return 0;
    }
    const auto out_line = economy::ledger_index(out_code);
    const auto in_line = economy::ledger_index(in_code);
    if (!out_line || !in_line) {
        return 0;
    }
    // What both sides can take; the smaller amount moves, so the pair keeps summing to zero.
    const Kopecks can_leave = -preview_posting(*from_row, *out_line, -amount_kop);
    const Kopecks can_enter = preview_posting(*to_row, *in_line, amount_kop);
    const Kopecks moved = std::min(can_leave, can_enter);
    if (moved <= 0) {
        return 0;
    }
    const Kopecks left = -post_line(*from_row, *out_line, -moved);
    const Kopecks entered = post_line(*to_row, *in_line, moved);
    PAT_ASSERT(left == moved && entered == moved, "transfer: the two sides moved different amounts");
    return std::min(left, entered);
}

bool World::close_ledger_month(Date new_month_start) {
    economy::EconomyState& e = economy_;
    if (e.meta.present == 0 || !is_sane_date(new_month_start) || new_month_start.day() != 1 ||
        new_month_start <= e.meta.open_month_start) {
        return false;
    }
    constexpr std::size_t kLines = economy::kLedgerLineCount;
    constexpr std::size_t kRing = economy::kLedgerRingMonths;
    economy::AccountTable& t = e.accounts;
    for (std::size_t a = 0; a < t.size(); ++a) {
        Kopecks* ring_opening = t.ring_opening_.data() + a * kRing;
        std::move_backward(ring_opening, ring_opening + (kRing - 1), ring_opening + kRing);
        ring_opening[0] = t.opening_[a];

        Kopecks* ring_lines = t.ring_lines_.data() + a * kRing * kLines;
        std::move_backward(ring_lines, ring_lines + (kRing - 1) * kLines, ring_lines + kRing * kLines);
        Kopecks* open_lines = t.open_lines_.data() + a * kLines;
        std::copy_n(open_lines, kLines, ring_lines);
        std::fill_n(open_lines, kLines, Kopecks{0});

        t.opening_[a] = t.treasury_[a];
    }
    std::move_backward(t.ring_month_start_.begin(), t.ring_month_start_.end() - 1, t.ring_month_start_.end());
    t.ring_month_start_[0] = e.meta.open_month_start;
    e.meta.open_month_start = new_month_start;
    e.meta.closed_months = static_cast<std::uint8_t>(std::min<std::size_t>(kRing, e.meta.closed_months + 1U));
    return true;
}

std::optional<std::uint32_t>
World::log_incident(economy::IncidentKind kind, ProvinceId province, DynastyId dynasty, std::int32_t param) {
    PAT_ASSERT(economy_.meta.present != 0, "log_incident: the world has no economy");
    const auto kind_value = static_cast<std::uint8_t>(kind);
    if (economy_.meta.present == 0 || kind_value < 1 || kind_value > economy::kMaxIncidentKind) {
        return std::nullopt;
    }
    economy::IncidentLog& log = economy_.incidents;
    if (log.size() >= economy::kIncidentLogSize) {  // the ring forgets the oldest entry
        log.seq.erase(log.seq.begin());
        log.date.erase(log.date.begin());
        log.province.erase(log.province.begin());
        log.dynasty.erase(log.dynasty.begin());
        log.kind.erase(log.kind.begin());
        log.param.erase(log.param.begin());
    }
    const std::uint32_t seq = economy_.meta.next_incident_seq++;
    log.seq.push_back(seq);
    log.date.push_back(today());
    log.province.push_back(exists(province) ? province : ProvinceId());
    log.dynasty.push_back(exists(dynasty) ? dynasty : DynastyId());
    log.kind.push_back(kind_value);
    log.param.push_back(param);
    return seq;
}

// ---- invariants ---------------------------------------------------------------------------------

void World::check_economy_invariants(std::vector<std::string>& bad) const {
    const economy::EconomyState& e = economy_;
    auto fail = [&](std::string message) { bad.push_back("economy: " + std::move(message)); };

    if (e.meta.present > 1) {
        fail("the present flag is neither 0 nor 1");
        return;
    }
    if (e.meta.present == 0) {
        // E-S1: without the economy every table is empty and every scalar is at its default.
        if (!(e == economy::EconomyState{})) {
            fail("the world has no economy but holds economy data");
        }
        return;
    }

    constexpr std::size_t kLines = economy::kLedgerLineCount;
    constexpr std::size_t kRing = economy::kLedgerRingMonths;
    const std::size_t province_count = provinces_.size();
    const std::size_t dynasty_count = dynasties_.size();
    const economy::AccountTable& acc = e.accounts;
    const economy::ProvinceEconomy& pe = e.provinces;
    const economy::PopulationGroups& grp = e.groups;

    // ---- E-S1: lengths -----------------------------------------------------------------------
    bool sizes_ok = true;
    auto expect = [&](const char* what, std::size_t got, std::size_t want) {
        if (got != want) {
            fail(std::format("{} has {} rows, expected {}", what, got, want));
            sizes_ok = false;
        }
    };
    expect("dynasty manpower_reserve", e.dynasties.manpower_reserve.size(), dynasty_count);
    expect("dynasty bankrupt", e.dynasties.bankrupt.size(), dynasty_count);
    expect("account treasury", acc.treasury_.size(), 1 + dynasty_count);
    expect("account opening", acc.opening_.size(), 1 + dynasty_count);
    expect("account open lines", acc.open_lines_.size(), (1 + dynasty_count) * kLines);
    expect("account ring opening", acc.ring_opening_.size(), (1 + dynasty_count) * kRing);
    expect("account ring lines", acc.ring_lines_.size(), (1 + dynasty_count) * kRing * kLines);
    expect("province kind", pe.kind.size(), province_count);
    expect("province owner_kind", pe.owner_kind.size(), province_count);
    expect("province tax_policy", pe.tax_policy.size(), province_count);
    expect("province recruit_policy", pe.recruit_policy.size(), province_count);
    expect("province slots_total", pe.slots_total.size(), province_count);
    expect("province trade_node", pe.trade_node.size(), province_count);
    expect("province administrator", pe.administrator.size(), province_count);
    expect("province control", pe.control.size(), province_count);
    expect("province corruption", pe.corruption.size(), province_count);
    expect("province unrest", pe.unrest.size(), province_count);
    expect("province labour_base", pe.labour_base.size(), province_count);
    expect("province grain_stock", pe.grain_stock.size(), province_count);
    expect("province recruit_pool", pe.recruit_pool.size(), province_count);
    expect("province income_last_month", pe.income_last_month.size(), province_count);
    expect("province last_slot_date", pe.last_slot_date.size(), province_count);
    expect("province capacity", pe.capacity.size(), province_count * economy::kMaxGoods);
    expect("province slot_building", pe.slot_building.size(), province_count * economy::kMaxSlots);
    const std::size_t group_rows = province_count * economy::kGroupCount;
    expect("group size", grp.size.size(), group_rows);
    expect("group mood", grp.mood.size(), group_rows);
    expect("group wealth", grp.wealth.size(), group_rows);
    expect("group growth_modifier", grp.growth_modifier.size(), group_rows);
    expect("group growth_carry", grp.growth_carry.size(), group_rows);
    const economy::ConstructionQueue& q = e.construction;
    const std::size_t queue_rows = q.size();
    expect("construction slot", q.slot.size(), queue_rows);
    expect("construction building", q.building.size(), queue_rows);
    expect("construction payer", q.payer.size(), queue_rows);
    expect("construction start_date", q.start_date.size(), queue_rows);
    expect("construction finish_date", q.finish_date.size(), queue_rows);
    expect("construction paid_kop", q.paid_kop.size(), queue_rows);
    expect("construction seq", q.seq.size(), queue_rows);
    const economy::LoanTable& ln = e.loans;
    const std::size_t loan_rows = ln.size();
    expect("loan borrower", ln.borrower.size(), loan_rows);
    expect("loan principal", ln.principal.size(), loan_rows);
    expect("loan rate", ln.rate_ppm_year.size(), loan_rows);
    expect("loan issued", ln.issued.size(), loan_rows);
    expect("loan due", ln.due.size(), loan_rows);
    expect("loan interest_carry", ln.interest_carry.size(), loan_rows);
    expect("loan overdue", ln.overdue.size(), loan_rows);
    expect("loan collateral", ln.collateral.size(), loan_rows);
    const economy::QuotaTable& qt = e.quotas;
    const std::size_t quota_rows = qt.size();
    expect("quota dynasty", qt.dynasty.size(), quota_rows);
    expect("quota kind", qt.kind.size(), quota_rows);
    expect("quota amount", qt.amount.size(), quota_rows);
    expect("quota period_start", qt.period_start.size(), quota_rows);
    expect("quota deadline", qt.deadline.size(), quota_rows);
    const economy::QuotaOutcomeRing& oc = e.outcomes;
    const std::size_t outcome_rows = oc.size();
    expect("outcome dynasty", oc.dynasty.size(), outcome_rows);
    expect("outcome kind", oc.kind.size(), outcome_rows);
    expect("outcome due", oc.due.size(), outcome_rows);
    expect("outcome delivered", oc.delivered.size(), outcome_rows);
    expect("outcome settled_on", oc.settled_on.size(), outcome_rows);
    expect("outcome flags", oc.flags.size(), outcome_rows);
    const economy::IncidentLog& lg = e.incidents;
    const std::size_t incident_rows = lg.size();
    expect("incident date", lg.date.size(), incident_rows);
    expect("incident province", lg.province.size(), incident_rows);
    expect("incident dynasty", lg.dynasty.size(), incident_rows);
    expect("incident kind", lg.kind.size(), incident_rows);
    expect("incident param", lg.param.size(), incident_rows);
    if (!sizes_ok) {
        return;  // everything below indexes these columns
    }

    // ---- scalars and rings (E-S2) --------------------------------------------------------------
    const economy::EconomyMeta& meta = e.meta;
    if (meta.crown_dynasty.valid() && !exists(meta.crown_dynasty)) {
        fail("the crown dynasty does not exist");
    }
    if (!is_sane_date(meta.open_month_start) || meta.open_month_start.day() != 1) {
        fail("the open month does not start on the first day of a month");
    }
    if (static_cast<std::size_t>(meta.closed_months) > kRing) {
        fail(std::format("{} closed months, at most {} fit in the ring", meta.closed_months, kRing));
    }
    if (oc.size() > economy::kQuotaOutcomeRingSize) {
        fail("the quota outcome ring is over capacity");
    }
    if (lg.size() > economy::kIncidentLogSize) {
        fail("the incident log is over capacity");
    }

    // ---- accounts: E-M1, E-M2, E-M3, E-M4 ------------------------------------------------------
    bool amounts_ok = true;
    auto check_balance = [&](Kopecks value, const char* what, std::size_t account) {
        if (value < -economy::kMaxTreasuryKop || value > economy::kMaxTreasuryKop) {
            fail(std::format("account {} {} {} is beyond the treasury bound", account, what, value));
            amounts_ok = false;
        }
    };
    auto check_lines = [&](std::span<const Kopecks> lines, const char* what, std::size_t account) {
        for (const Kopecks line : lines) {
            if (line < -economy::kMaxLedgerLineKop || line > economy::kMaxLedgerLineKop) {
                fail(std::format("account {} has a {} line {} beyond the bound", account, what, line));
                amounts_ok = false;
                return;
            }
        }
    };
    const std::size_t closed = std::min<std::size_t>(meta.closed_months, kRing);
    for (std::size_t a = 0; a < acc.size(); ++a) {
        check_balance(acc.treasury(a), "treasury", a);
        check_balance(acc.opening(a), "opening balance", a);
        check_lines(acc.open_lines(a), "open", a);
        for (std::size_t m = 0; m < kRing; ++m) {
            check_balance(acc.ring_opening(a, m), "ring opening balance", a);
            check_lines(acc.ring_lines(a, m), "closed", a);
        }
    }
    if (amounts_ok) {
        for (std::size_t a = 0; a < acc.size(); ++a) {
            if (acc.treasury(a) != acc.opening(a) + economy::AccountTable::sum(acc.open_lines(a))) {
                fail(std::format("account {} treasury {} != opening {} + open lines {}",
                                 a,
                                 acc.treasury(a),
                                 acc.opening(a),
                                 economy::AccountTable::sum(acc.open_lines(a))));
            }
            for (std::size_t m = 0; m < kRing; ++m) {
                if (m >= closed) {
                    const auto lines = acc.ring_lines(a, m);
                    const bool empty =
                        acc.ring_opening(a, m) == 0 &&
                        std::all_of(lines.begin(), lines.end(), [](Kopecks k) { return k == 0; });
                    if (!empty) {
                        fail(std::format("account {} holds data in the unused ring month {}", a, m));
                    }
                    continue;
                }
                const Kopecks closing =
                    acc.ring_opening(a, m) + economy::AccountTable::sum(acc.ring_lines(a, m));
                const Kopecks next_opening = m == 0 ? acc.opening(a) : acc.ring_opening(a, m - 1);
                if (closing != next_opening) {
                    fail(std::format("account {} closed month {} ends at {} but the next one starts at {}",
                                     a,
                                     m,
                                     closing,
                                     next_opening));
                }
            }
        }
        // E-M3: both lines of a transfer pair cancel out over all accounts, in every month.
        auto pair_sum = [&](std::size_t month, bool open, std::size_t line) {
            Kopecks total = 0;
            for (std::size_t a = 0; a < acc.size(); ++a) {
                const auto lines = open ? acc.open_lines(a) : acc.ring_lines(a, month);
                total = economy::saturating_add(total, lines[line]);
            }
            return total;
        };
        for (const economy::LedgerPair& pair : economy::kLedgerPairs) {
            const std::size_t out_line = *economy::ledger_index(pair.out);
            const std::size_t in_line = *economy::ledger_index(pair.in);
            for (std::size_t m = 0; m <= closed; ++m) {
                const bool open = m == closed;
                const Kopecks net =
                    economy::saturating_add(pair_sum(m, open, out_line), pair_sum(m, open, in_line));
                if (net != 0) {
                    fail(std::format("the transfer pair {}/{} does not cancel out in {} (net {})",
                                     static_cast<unsigned>(pair.out),
                                     static_cast<unsigned>(pair.in),
                                     open ? std::string("the open month") : std::format("closed month {}", m),
                                     net));
                }
            }
        }
    }
    // The months of the ring are dated: the newest ends before the open month, then going back.
    {
        Date later = meta.open_month_start;
        for (std::size_t m = 0; m < kRing; ++m) {
            const Date start = acc.ring_month_start(m);
            if (m >= closed) {
                if (start != Date()) {
                    fail(std::format("the unused ring month {} has a date", m));
                }
                continue;
            }
            if (!is_sane_date(start) || start.day() != 1 || start >= later) {
                fail(std::format("closed month {} is dated out of order", m));
            }
            later = start;
        }
    }

    // ---- dynasties ----------------------------------------------------------------------------
    for (std::size_t d = 0; d < dynasty_count; ++d) {
        if (e.dynasties.bankrupt[d] > 1) {
            fail(std::format("dynasty {} bankrupt flag is neither 0 nor 1", d));
        }
    }

    // ---- provinces and groups: E-S2, E-P1..E-P4 ------------------------------------------------
    for (std::size_t p = 0; p < province_count; ++p) {
        if (static_cast<std::size_t>(pe.kind[p]) >= economy::kProvinceKindCount) {
            fail(std::format("province {} has an unknown kind", p));
        }
        if (pe.owner_kind[p] >= economy::kOwnerKindCount) {
            fail(std::format("province {} has an unknown owner kind", p));
        } else if ((pe.owner_kind[p] == to_u8(OwnerKind::Dynasty)) != provinces_.owner[p].valid()) {
            fail(std::format("province {}: the owner kind Dynasty must go with a dynasty owner", p));
        }
        if (static_cast<std::size_t>(pe.slots_total[p]) > economy::kMaxSlots) {
            fail(std::format(
                "province {} has {} slots, at most {}", p, pe.slots_total[p], economy::kMaxSlots));
        }
        if (pe.administrator[p].valid() && !is_alive(pe.administrator[p])) {
            fail(std::format("province {} administrator {} is not alive", p, pe.administrator[p].value()));
        }
        if (!in_range(pe.control[p], kZero, economy::kPointsMax)) {
            fail(std::format("province {} control {} is out of 0..100", p, pe.control[p].raw()));
        }
        if (!in_range(pe.unrest[p], kZero, economy::kPointsMax)) {
            fail(std::format("province {} unrest {} is out of 0..100", p, pe.unrest[p].raw()));
        }
        if (!in_range(pe.corruption[p], kZero, economy::kCorruptionMax)) {
            fail(std::format("province {} corruption {} is out of 0..1", p, pe.corruption[p].raw()));
        }
        if (!in_range(pe.grain_stock[p], kZero, economy::kStockMax)) {
            fail(std::format("province {} grain stock {} is out of range", p, pe.grain_stock[p].raw()));
        }
        for (std::size_t good = 0; good < economy::kMaxGoods; ++good) {
            if (!in_range(pe.capacity[p * economy::kMaxGoods + good], kZero, economy::kStockMax)) {
                fail(std::format("province {} capacity of good {} is out of range", p, good));
            }
        }
        for (std::size_t slot = 0; slot < economy::kMaxSlots; ++slot) {
            if (pe.slot_building[p * economy::kMaxSlots + slot] != 0 && slot >= pe.slots_total[p]) {
                fail(std::format(
                    "province {} has a building in slot {} beyond its {} slots", p, slot, pe.slots_total[p]));
            }
        }
        if (pe.last_slot_date[p] != kNoDate &&
            (!is_sane_date(pe.last_slot_date[p]) || pe.last_slot_date[p] > today())) {
            fail(std::format("province {} was processed on an impossible date", p));
        }
        if (provinces_.development[p] < 1 || provinces_.development[p] > economy::kMaxDevelopment) {
            fail(std::format("province {} development {} is out of 1..{}",
                             p,
                             provinces_.development[p],
                             economy::kMaxDevelopment));
        }
        if (pe.income_last_month[p] < -economy::kMaxTreasuryKop ||
            pe.income_last_month[p] > economy::kMaxTreasuryKop) {
            fail(std::format("province {} last income is beyond the treasury bound", p));
        }
        std::uint64_t persons = 0;
        for (std::size_t g = 0; g < economy::kGroupCount; ++g) {
            const std::size_t row = p * economy::kGroupCount + g;
            persons += grp.size[row];
            if (!in_range(grp.mood[row], economy::kMoodMin, economy::kMoodMax)) {
                fail(std::format(
                    "province {} group {} mood {} is out of -100..100", p, g, grp.mood[row].raw()));
            }
            if (!in_range(grp.wealth[row], kZero, economy::kWealthMax)) {
                fail(std::format(
                    "province {} group {} wealth {} is out of 0..300", p, g, grp.wealth[row].raw()));
            }
            if (grp.growth_modifier[row] < -economy::kMaxGrowthModifierPpm ||
                grp.growth_modifier[row] > economy::kMaxGrowthModifierPpm) {
                fail(std::format("province {} group {} growth modifier {} is out of range",
                                 p,
                                 g,
                                 grp.growth_modifier[row]));
            }
            if (grp.growth_carry[row] < -economy::kMaxGrowthCarry ||
                grp.growth_carry[row] > economy::kMaxGrowthCarry) {
                fail(std::format(
                    "province {} group {} growth carry {} is out of range", p, g, grp.growth_carry[row]));
            }
        }
        if (persons > economy::kMaxProvincePersons) {
            fail(std::format(
                "province {} has {} people, over the cap {}", p, persons, economy::kMaxProvincePersons));
        } else if (provinces_.population[p] !=
                   Fixed32::from_ratio(static_cast<std::int64_t>(persons), 1000)) {
            fail(std::format("province {}: the population cache {} != {} people / 1000",
                             p,
                             provinces_.population[p].raw(),
                             persons));
        }
    }

    // ---- construction: E-B1 --------------------------------------------------------------------
    {
        std::vector<std::uint32_t> keys;
        keys.reserve(queue_rows);
        for (std::size_t i = 0; i < queue_rows; ++i) {
            const ProvinceId province = q.province[i];
            if (!exists(province)) {
                fail(std::format("construction entry {} is in an unknown province", i));
                continue;
            }
            const std::size_t p = province.index();
            if (q.slot[i] >= economy::kMaxSlots || q.slot[i] >= pe.slots_total[p]) {
                fail(std::format(
                    "construction entry {} uses slot {} beyond the slots of province {}", i, q.slot[i], p));
                continue;
            }
            if (pe.slot_building[p * economy::kMaxSlots + q.slot[i]] != 0) {
                fail(std::format(
                    "construction entry {} stands on the occupied slot {} of province {}", i, q.slot[i], p));
            }
            keys.push_back(static_cast<std::uint32_t>(p) * economy::kMaxSlots + q.slot[i]);
            if (q.building[i] == 0) {
                fail(std::format("construction entry {} builds nothing", i));
            }
            if (!exists(q.payer[i])) {
                fail(std::format("construction entry {} has an unknown payer", i));
            }
            if (!is_sane_date(q.start_date[i]) || !is_sane_date(q.finish_date[i]) ||
                q.finish_date[i] <= q.start_date[i]) {
                fail(std::format("construction entry {} does not finish after it starts", i));
            }
            if (q.paid_kop[i] < 0 || q.paid_kop[i] > economy::kMaxPostingKop) {
                fail(std::format("construction entry {} has an impossible payment", i));
            }
            if (q.seq[i] >= meta.next_construction_seq) {
                fail(std::format("construction entry {} has a sequence number from the future", i));
            }
            if (i > 0 && (q.finish_date[i - 1] > q.finish_date[i] ||
                          (q.finish_date[i - 1] == q.finish_date[i] && q.seq[i - 1] >= q.seq[i]))) {
                fail(std::format("construction entry {} is out of (finish date, seq) order", i));
            }
        }
        std::sort(keys.begin(), keys.end());
        if (std::adjacent_find(keys.begin(), keys.end()) != keys.end()) {
            fail("two construction entries share a slot");
        }
    }

    // ---- loans: E-L1 ---------------------------------------------------------------------------
    for (std::size_t i = 0; i < loan_rows; ++i) {
        if (ln.principal[i] < 0 || ln.principal[i] > economy::kMaxTreasuryKop) {
            fail(std::format("loan {} has an impossible principal", i));
        }
        if (!exists(ln.borrower[i])) {
            fail(std::format("loan {} has an unknown borrower", i));
        }
        if (ln.loan_id[i] >= meta.next_loan_id || (i > 0 && ln.loan_id[i - 1] >= ln.loan_id[i])) {
            fail(std::format("loan {} breaks the ascending order of ids", i));
        }
        if (ln.interest_carry[i] < 0 || ln.interest_carry[i] > economy::kMaxInterestCarry) {
            fail(std::format("loan {} interest carry {} is out of range", i, ln.interest_carry[i]));
        }
        if (ln.rate_ppm_year[i] < -1'000'000 || ln.rate_ppm_year[i] > 1'000'000) {
            fail(std::format("loan {} rate {} is out of range", i, ln.rate_ppm_year[i]));
        }
        if (ln.overdue[i] > 1) {
            fail(std::format("loan {} overdue flag is neither 0 nor 1", i));
        }
        if (ln.collateral[i].valid() && !exists(ln.collateral[i])) {
            fail(std::format("loan {} has an unknown collateral", i));
        }
        if (!is_sane_date(ln.issued[i]) || !is_sane_date(ln.due[i])) {
            fail(std::format("loan {} has an implausible date", i));
        }
    }

    // ---- quotas, outcomes, incidents: E-Q1 -----------------------------------------------------
    for (std::size_t i = 0; i < quota_rows; ++i) {
        if (qt.quota_id[i] >= meta.next_quota_id || (i > 0 && qt.quota_id[i - 1] >= qt.quota_id[i])) {
            fail(std::format("quota {} breaks the ascending order of ids", i));
        }
        if (!exists(qt.dynasty[i])) {
            fail(std::format("quota {} has an unknown dynasty", i));
        }
        if (qt.kind[i] >= economy::kQuotaKindCount) {
            fail(std::format("quota {} has an unknown kind", i));
        }
        if (qt.amount[i] <= 0) {
            fail(std::format("quota {} asks for nothing", i));
        }
        if (!is_sane_date(qt.period_start[i]) || !is_sane_date(qt.deadline[i]) ||
            qt.deadline[i] <= qt.period_start[i]) {
            fail(std::format("quota {} ends before its period starts", i));
        }
    }
    for (std::size_t i = 0; i < outcome_rows; ++i) {
        if (oc.seq[i] >= meta.next_outcome_seq || (i > 0 && oc.seq[i - 1] >= oc.seq[i])) {
            fail(std::format("quota outcome {} breaks the ascending order of sequence numbers", i));
        }
        if (!exists(oc.dynasty[i]) || oc.kind[i] >= economy::kQuotaKindCount ||
            (oc.flags[i] & ~economy::kQuotaOutcomeKnownFlags) != 0 || oc.due[i] < 0 || oc.delivered[i] < 0 ||
            !is_sane_date(oc.settled_on[i])) {
            fail(std::format("quota outcome {} is malformed", i));
        }
    }
    for (std::size_t i = 0; i < incident_rows; ++i) {
        if (lg.seq[i] >= meta.next_incident_seq || (i > 0 && lg.seq[i - 1] >= lg.seq[i])) {
            fail(std::format("incident {} breaks the ascending order of sequence numbers", i));
        }
        if (lg.kind[i] < 1 || lg.kind[i] > economy::kMaxIncidentKind || !is_sane_date(lg.date[i]) ||
            (lg.province[i].valid() && !exists(lg.province[i])) ||
            (lg.dynasty[i].valid() && !exists(lg.dynasty[i]))) {
            fail(std::format("incident {} is malformed", i));
        }
    }
}

}  // namespace pat::sim
