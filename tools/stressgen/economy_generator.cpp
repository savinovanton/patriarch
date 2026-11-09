#include "pat/stress/economy_generator.hpp"

#include "pat/core/rng.hpp"
#include "pat/sim/economy/ledger.hpp"
#include "pat/sim/economy/production.hpp"
#include "pat/sim/economy/rules.hpp"
#include "pat/sim/economy/tables.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <utility>
#include <vector>

namespace pat::stress {

namespace {

using core::Fixed32;
using core::Pcg32;
using sim::Date;
using sim::DynastyId;
using sim::ProvinceId;
using sim::economy::AccountRef;
using sim::economy::Kopecks;
using sim::economy::LedgerCode;
using sim::economy::ProvinceKind;

/// Building types the synthetic provinces use (the eight of GDD 03 section 15; slot ids 1..8).
constexpr std::uint32_t kBuildingTypes = 8;
/// Goods that M1 fills: grain, salt, fur, timber, iron, cloth (economy-model section 4).
constexpr std::array<std::size_t, 6> kProducedGoods{0, 1, 2, 3, 5, 7};
/// The incident log is a ring of 256: write more so that it has wrapped, like in a long game.
constexpr std::uint32_t kIncidentsToLog = 300;

/// The loans, quotas, outcomes, the construction queue and the counters have no writer in World yet
/// (E9-T14..T16), so the generator writes the tables the way the tests of the economy do. The world is
/// not const, so the cast is well defined; check_invariants() validates the result.
sim::economy::EconomyState& writable_state(sim::World& world) {
    return const_cast<sim::economy::EconomyState&>(world.economy());
}

/// First day of the month `delta` months away from the month of `any`.
Date shift_month(Date any, int delta) {
    const int total = any.year() * 12 + (any.month() - 1) + delta;
    return Date::from_ymd(total / 12, total % 12 + 1, 1);
}

ProvinceKind roll_kind(Pcg32& rng) {
    const auto roll = rng.bounded(1000);
    if (roll < 450) {
        return ProvinceKind::Rural;
    }
    if (roll < 530) {
        return ProvinceKind::City;
    }
    if (roll < 650) {
        return ProvinceKind::Border;
    }
    if (roll < 710) {
        return ProvinceKind::Industrial;
    }
    if (roll < 770) {
        return ProvinceKind::Port;
    }
    if (roll < 850) {
        return ProvinceKind::Steppe;
    }
    if (roll < 950) {
        return ProvinceKind::Northern;
    }
    return ProvinceKind::Simplified;
}

struct Site {
    ProvinceId province;
    std::uint8_t slot = 0;
    DynastyId payer;
    Date start;
    Date finish;
    Kopecks paid = 0;
    std::uint8_t building = 0;
};

/// Kopecks `count` x a draw in [lo, hi].
Kopecks scaled(Pcg32& rng, std::int64_t count, std::int32_t lo, std::int32_t hi) {
    return count * static_cast<Kopecks>(rng.range(lo, hi));
}

template <class... T>
std::size_t used_bytes(const std::vector<T>&... columns) {
    return ((columns.size() * sizeof(T)) + ...);
}

template <class... T>
std::size_t reserved_bytes(const std::vector<T>&... columns) {
    return ((columns.capacity() * sizeof(T)) + ...);
}

}  // namespace

core::Result<void> populate_economy(sim::World& world, const EconomyConfig& config) {
    using R = core::Result<void>;
    namespace eco = sim::economy;
    if (world.economy_present()) {
        return R::err({3, "the world has an economy already"});
    }
    if (config.closed_months > eco::kLedgerRingMonths || config.building_sites_permille > 1000) {
        return R::err({3, "economy counts are out of range"});
    }
    if (!world.enable_economy()) {
        return R::err({3, "cannot enable the economy"});
    }
    world.set_economy_rules_hash(kStressRulesHash);

    const auto province_count = static_cast<std::uint32_t>(world.provinces().size());
    const auto dynasty_count = static_cast<std::uint32_t>(world.dynasties().size());
    const Date today = world.today();
    auto& provinces_rng = world.rng().stream("stress.economy.provinces");
    auto& ledger_rng = world.rng().stream("stress.economy.ledger");
    auto& tables_rng = world.rng().stream("stress.economy.tables");
    eco::EconomyState& state = writable_state(world);

    // ---- provinces and people ------------------------------------------------------------------
    const eco::EconomyColumnsMut c = world.economy_columns_mut();
    std::vector<Site> sites;
    for (std::uint32_t p = 0; p < province_count; ++p) {
        const ProvinceId id(p);
        Pcg32& rng = provinces_rng;
        const ProvinceKind kind = roll_kind(rng);
        c.kind[p] = static_cast<std::uint8_t>(kind);
        world.set_province_development(id, static_cast<std::uint8_t>(rng.range(1, eco::kMaxDevelopment)));
        if (rng.chance_permille(60)) {
            (void)world.set_province_owner_kind(id, eco::OwnerKind::Crown);
        }

        // The population cache became peasants when the economy was switched on; split it.
        const std::uint64_t total = world.population_persons(id);
        auto share = [&](std::uint32_t lo, std::uint32_t span) {
            return static_cast<std::uint32_t>(total * (lo + rng.bounded(span)) / 1000);
        };
        const std::uint32_t nobility = share(5, 11);
        const std::uint32_t clergy = share(5, 8);
        std::uint32_t townsfolk = share(20, 61);
        if (kind == ProvinceKind::City) {
            townsfolk = share(150, 151);
        } else if (kind == ProvinceKind::Industrial || kind == ProvinceKind::Port) {
            townsfolk = share(80, 71);
        }
        const std::uint32_t cossacks =
            (kind == ProvinceKind::Border || kind == ProvinceKind::Steppe) ? share(100, 201) : 0;
        const auto others = static_cast<std::uint64_t>(nobility) + clergy + townsfolk + cossacks;
        const auto peasants = static_cast<std::uint32_t>(total - others);
        (void)world.set_group_size(id, eco::Group::Peasants, peasants);
        (void)world.set_group_size(id, eco::Group::Nobility, nobility);
        (void)world.set_group_size(id, eco::Group::Clergy, clergy);
        (void)world.set_group_size(id, eco::Group::Townsfolk, townsfolk);
        (void)world.set_group_size(id, eco::Group::Cossacks, cossacks);
        for (std::size_t g = 0; g < eco::kGroupCount; ++g) {
            const std::size_t row = eco::group_row(p, static_cast<eco::Group>(g));
            c.mood[row] = Fixed32::from_raw(rng.range(-30 * Fixed32::one_raw, 30 * Fixed32::one_raw));
            c.wealth[row] = Fixed32::from_int(rng.range(60, 160));
            c.growth_modifier[row] = rng.range(-5000, 5000);
            c.growth_carry[row] = rng.range(-eco::kMaxGrowthCarry, eco::kMaxGrowthCarry);
        }

        // Policies, state of order and the stock.
        c.tax_policy[p] = static_cast<std::uint8_t>(rng.bounded(4));
        c.recruit_policy[p] = static_cast<std::uint8_t>(rng.bounded(3));
        c.control[p] = Fixed32::from_raw(rng.range(40 * Fixed32::one_raw, 100 * Fixed32::one_raw));
        c.corruption[p] = Fixed32::from_raw(rng.range(0, Fixed32::one_raw / 2));
        c.unrest[p] = Fixed32::from_raw(rng.range(0, 60 * Fixed32::one_raw));
        c.labour_base[p] = peasants + townsfolk;
        c.grain_stock[p] = Fixed32::from_int(rng.range(10, 400));
        c.recruit_pool[p] = peasants / 100 * (1 + rng.bounded(4));
        c.income_last_month[p] = static_cast<Kopecks>(total) * (1 + rng.bounded(6));
        c.last_slot_date[p] = rng.chance_permille(900) ? today - rng.range(1, 31) : sim::kNoDate;
        c.trade_node[p] = static_cast<std::uint16_t>(rng.bounded(31));
        for (const std::size_t good : kProducedGoods) {
            c.capacity[p * eco::kMaxGoods + good] = Fixed32::from_int(rng.range(5, 800));
        }
        // Grain is sized to the people (E9-T07): a peaceful province feeds itself with a fifth to
        // spare at its starting workforce and control, so the 50 years of peace of the tests and the
        // benchmarks are not a famine. The draw above is kept so that the other goods stay as they were.
        {
            std::array<std::uint32_t, eco::kGroupCount> sizes{};
            for (std::size_t g = 0; g < eco::kGroupCount; ++g) {
                sizes[g] = world.economy().groups.size[eco::group_row(p, static_cast<eco::Group>(g))];
            }
            const eco::EconomyRules rules = eco::EconomyRules::defaults();
            const eco::Quantity need = eco::compute_need(rules.consumption, sizes, eco::kGrainGood);
            const std::int64_t control = std::max<std::int64_t>(1, eco::control_permille(c.control[p]));
            const std::int64_t kept = 1000 - rules.production.losses_permille;
            const std::int64_t raw = need * 1200 / 1000 * 1000 / control * 1000 / kept;
            c.capacity[p * eco::kMaxGoods + eco::kGrainGood] = Fixed32::from_raw(
                static_cast<std::int32_t>(std::min<std::int64_t>(raw, eco::kStockMax.raw())));
        }

        // Administrator: a living member of the owning dynasty, for about half of the provinces.
        const DynastyId owner = world.provinces().owner[p];
        if (owner.valid() && rng.chance_permille(500)) {
            const auto members = world.members_of(owner);
            if (!members.empty()) {
                (void)world.set_province_administrator(
                    id, members[rng.bounded(static_cast<std::uint32_t>(members.size()))]);
            }
        }

        // Buildings fill the first slots; a construction site keeps the last 2-3 slots free.
        std::uint32_t site_slots = 0;
        auto slots = static_cast<std::uint32_t>(rng.range(2, static_cast<std::int32_t>(eco::kMaxSlots)));
        if (owner.valid() && rng.chance_permille(static_cast<std::int32_t>(config.building_sites_permille))) {
            site_slots = 2 + rng.bounded(2);
            slots = std::max<std::uint32_t>(slots, 4);
        }
        c.slots_total[p] = static_cast<std::uint8_t>(slots);
        for (std::uint32_t s = 0; s + site_slots < slots; ++s) {
            if (rng.chance_permille(700)) {
                (void)world.set_slot_building(
                    id, s, static_cast<std::uint8_t>(1 + rng.bounded(kBuildingTypes)));
            }
        }
        for (std::uint32_t s = slots - site_slots; s < slots && site_slots > 0; ++s) {
            Site site;
            site.province = id;
            site.slot = static_cast<std::uint8_t>(s);
            site.payer = owner;
            site.start = today - rng.range(1, 120);
            site.finish = today + rng.range(1, 300);
            site.paid = scaled(rng, 1, 100'000, 2'000'000);
            site.building = static_cast<std::uint8_t>(1 + rng.bounded(kBuildingTypes));
            sites.push_back(site);
        }
    }

    // ---- construction queue: ordered by (finish date, seq) ---------------------------------------
    std::sort(sites.begin(), sites.end(), [](const Site& a, const Site& b) {
        if (a.finish != b.finish) {
            return a.finish < b.finish;
        }
        if (a.province != b.province) {
            return a.province.value() < b.province.value();
        }
        return a.slot < b.slot;
    });
    eco::ConstructionQueue& queue = state.construction;
    std::uint32_t seq = 0;
    for (const Site& site : sites) {
        queue.province.push_back(site.province);
        queue.slot.push_back(site.slot);
        queue.building.push_back(site.building);
        queue.payer.push_back(site.payer);
        queue.start_date.push_back(site.start);
        queue.finish_date.push_back(site.finish);
        queue.paid_kop.push_back(site.paid);
        queue.seq.push_back(seq++);
    }
    state.meta.next_construction_seq = seq + tables_rng.bounded(50);

    // ---- dynasties ---------------------------------------------------------------------------
    for (std::uint32_t d = 0; d < dynasty_count; ++d) {
        c.manpower_reserve[d] = tables_rng.bounded(2000);
        c.bankrupt[d] = static_cast<std::uint8_t>(tables_rng.chance_permille(40) ? 1 : 0);
    }

    // ---- the statement: closed months in the year before the snapshot, then the open month -------
    // The statement starts `closed_months` months before the month of the snapshot, so that the open
    // month is the month of `today` (the settlement system closes it at the start of the next month).
    const int closed = static_cast<int>(config.closed_months);
    const Date first_month = shift_month(today, -closed);
    state.meta.open_month_start = first_month;

    const AccountRef crown = AccountRef::crown();
    Pcg32& rng = ledger_rng;
    (void)world.post_money(crown, LedgerCode::Adjustment, scaled(rng, 1, 500'000'000, 900'000'000));
    for (std::uint32_t d = 0; d < dynasty_count; ++d) {
        (void)world.post_money(
            AccountRef::of(DynastyId(d)), LedgerCode::Adjustment, scaled(rng, 1, 50'000, 5'000'000));
    }
    const auto fill_month = [&]() {
        for (std::uint32_t d = 0; d < dynasty_count; ++d) {
            const AccountRef who = AccountRef::of(DynastyId(d));
            const auto owned = static_cast<std::int64_t>(world.provinces_of(DynastyId(d)).size());
            if (owned > 0) {
                const Kopecks peasants_tax = scaled(rng, owned, 20'000, 400'000);
                (void)world.post_money(who, LedgerCode::TaxPeasants, peasants_tax);
                (void)world.post_money(who, LedgerCode::TaxTownsfolk, scaled(rng, owned, 5'000, 120'000));
                (void)world.post_money(who, LedgerCode::TaxCossacks, scaled(rng, owned, 0, 60'000));
                (void)world.post_money(who, LedgerCode::HoldingIncome, scaled(rng, owned, 10'000, 250'000));
                (void)world.post_money(
                    who, LedgerCode::Embezzlement, -(peasants_tax / 1000) * rng.range(20, 120));
                (void)world.post_money(who, LedgerCode::BuildingUpkeep, -scaled(rng, owned, 5'000, 60'000));
            }
            (void)world.post_money(who, LedgerCode::CourtUpkeep, -scaled(rng, 1, 30'000, 300'000));
            if (rng.chance_permille(300)) {
                (void)world.post_money(who, LedgerCode::SalariesOut, -scaled(rng, 1, 10'000, 150'000));
            }
            if (rng.chance_permille(250)) {
                (void)world.post_money(who, LedgerCode::Construction, -scaled(rng, 1, 100'000, 2'000'000));
            }
            if (rng.chance_permille(50)) {
                (void)world.post_money(who, LedgerCode::RaidLoss, -scaled(rng, 1, 10'000, 400'000));
            }
            if (rng.chance_permille(100)) {
                (void)world.post_money(who, LedgerCode::GrantsIn, scaled(rng, 1, 20'000, 500'000));
            }
            if (rng.chance_permille(120)) {
                (void)world.transfer(crown,
                                     who,
                                     LedgerCode::LoanIssued,
                                     LedgerCode::LoanTaken,
                                     scaled(rng, 1, 500'000, 3'000'000));
            }
            if (rng.chance_permille(100)) {
                (void)world.transfer(who,
                                     crown,
                                     LedgerCode::LoanRepaid,
                                     LedgerCode::LoanCollected,
                                     scaled(rng, 1, 100'000, 1'000'000));
            }
            if (rng.chance_permille(200)) {
                (void)world.transfer(who,
                                     crown,
                                     LedgerCode::QuotaMoney,
                                     LedgerCode::QuotaCollected,
                                     scaled(rng, 1, 20'000, 600'000));
            }
            if (rng.chance_permille(300)) {
                (void)world.transfer(who,
                                     crown,
                                     LedgerCode::Interest,
                                     LedgerCode::InterestCollected,
                                     scaled(rng, 1, 5'000, 80'000));
            }
        }
        (void)world.post_money(crown, LedgerCode::TaxPeasants, scaled(rng, 1, 2'000'000, 20'000'000));
        (void)world.post_money(crown, LedgerCode::HoldingIncome, scaled(rng, 1, 1'000'000, 10'000'000));
        (void)world.post_money(crown, LedgerCode::ArmyUpkeep, -scaled(rng, 1, 5'000'000, 30'000'000));
        (void)world.post_money(crown, LedgerCode::CourtUpkeep, -scaled(rng, 1, 1'000'000, 5'000'000));
        if (rng.chance_permille(200)) {
            (void)world.post_money(crown, LedgerCode::Tribute, scaled(rng, 1, 100'000, 3'000'000));
        }
    };
    for (int m = 0; m < closed; ++m) {
        fill_month();
        if (!world.close_ledger_month(shift_month(first_month, m + 1))) {
            return R::err({3, "cannot close a synthetic statement month"});
        }
    }
    fill_month();  // the open month

    // ---- loans, quotas and their outcomes --------------------------------------------------------
    Pcg32& trng = tables_rng;
    eco::LoanTable& loans = state.loans;
    std::uint32_t loan_id = 0;
    for (std::uint32_t d = 0; d < dynasty_count; ++d) {
        if (!trng.chance_permille(450)) {
            continue;
        }
        const std::uint32_t count = 1 + trng.bounded(3);  // up to 3 per dynasty, as the rules propose
        for (std::uint32_t i = 0; i < count; ++i) {
            loans.loan_id.push_back(loan_id);
            loan_id += 1 + trng.bounded(2);
            loans.borrower.push_back(DynastyId(d));
            loans.principal.push_back(scaled(trng, 1, 100'000, 5'000'000));
            loans.rate_ppm_year.push_back(trng.range(50'000, 120'000));
            loans.issued.push_back(today - trng.range(10, 400));
            loans.due.push_back(shift_month(today, 1 + static_cast<int>(trng.bounded(24))));
            loans.interest_carry.push_back(trng.range(0, eco::kMaxInterestCarry));
            loans.overdue.push_back(static_cast<std::uint8_t>(trng.chance_permille(80) ? 1 : 0));
            loans.collateral.emplace_back();
        }
    }
    state.meta.next_loan_id = loan_id + trng.bounded(5);

    eco::QuotaTable& quotas = state.quotas;
    std::uint32_t quota_id = 0;
    for (std::uint32_t d = 0; d < dynasty_count; ++d) {
        if (!trng.chance_permille(300)) {
            continue;
        }
        const bool money = trng.chance_permille(500);
        const Date start = shift_month(today, -static_cast<int>(trng.bounded(6)));
        quotas.quota_id.push_back(quota_id);
        quota_id += 1 + trng.bounded(2);
        quotas.dynasty.push_back(DynastyId(d));
        quotas.kind.push_back(
            static_cast<std::uint8_t>(money ? eco::QuotaKind::Money : eco::QuotaKind::Recruits));
        quotas.amount.push_back(money ? scaled(trng, 1, 50'000, 2'000'000) : trng.range(5, 200));
        quotas.period_start.push_back(start);
        quotas.deadline.push_back(shift_month(start, 12));
    }
    state.meta.next_quota_id = quota_id + trng.bounded(5);

    eco::QuotaOutcomeRing& outcomes = state.outcomes;
    std::uint32_t outcome_seq = trng.bounded(100);
    for (std::size_t i = 0; i < eco::kQuotaOutcomeRingSize && dynasty_count > 0; ++i) {
        const bool money = trng.chance_permille(500);
        const std::int64_t due = money ? scaled(trng, 1, 50'000, 2'000'000) : trng.range(5, 200);
        const std::uint32_t outcome = trng.bounded(3);  // 0 paid in full, 1 short, 2 short with penalty
        outcomes.seq.push_back(outcome_seq);
        outcome_seq += 1 + trng.bounded(3);
        outcomes.dynasty.push_back(DynastyId(trng.bounded(dynasty_count)));
        outcomes.kind.push_back(
            static_cast<std::uint8_t>(money ? eco::QuotaKind::Money : eco::QuotaKind::Recruits));
        outcomes.due.push_back(due);
        outcomes.delivered.push_back(outcome == 0 ? due : due / 2);
        outcomes.settled_on.push_back(today - static_cast<std::int32_t>(700 - 40 * i + trng.bounded(30)));
        std::uint8_t flags = 0;
        if (outcome >= 1) {
            flags = eco::kQuotaOutcomeShort;
        }
        if (outcome == 2) {
            flags = static_cast<std::uint8_t>(flags | eco::kQuotaOutcomePenalty);
        }
        outcomes.flags.push_back(flags);
    }
    state.meta.next_outcome_seq = outcome_seq + trng.bounded(4);

    // ---- incidents: more than the ring holds ----------------------------------------------------
    for (std::uint32_t i = 0; i < kIncidentsToLog; ++i) {
        const auto kind = static_cast<eco::IncidentKind>(1 + trng.bounded(8));
        const ProvinceId province =
            province_count > 0 ? ProvinceId(trng.bounded(province_count)) : ProvinceId();
        const DynastyId dynasty = dynasty_count > 0 ? DynastyId(trng.bounded(dynasty_count)) : DynastyId();
        (void)world.log_incident(kind, province, dynasty, trng.range(-1000, 1000));
    }

    if (const auto violations = world.check_invariants(); !violations.empty()) {
        return R::err({4, violations.front()});
    }
    return R::ok();
}

EconomyMemory economy_memory(const sim::World& world) {
    namespace eco = sim::economy;
    EconomyMemory out;
    out.provinces = world.provinces().size();
    out.dynasties = world.dynasties().size();
    if (!world.economy_present()) {
        return out;
    }
    const eco::EconomyState& e = world.economy();
    out.groups_used = used_bytes(
        e.groups.size, e.groups.mood, e.groups.wealth, e.groups.growth_modifier, e.groups.growth_carry);
    out.groups_allocated = reserved_bytes(
        e.groups.size, e.groups.mood, e.groups.wealth, e.groups.growth_modifier, e.groups.growth_carry);
    const eco::ProvinceEconomy& p = e.provinces;
    out.province_used = used_bytes(p.kind,
                                   p.owner_kind,
                                   p.tax_policy,
                                   p.recruit_policy,
                                   p.slots_total,
                                   p.trade_node,
                                   p.administrator,
                                   p.control,
                                   p.corruption,
                                   p.unrest,
                                   p.labour_base,
                                   p.grain_stock,
                                   p.recruit_pool,
                                   p.income_last_month,
                                   p.last_slot_date,
                                   p.capacity,
                                   p.slot_building);
    out.province_allocated = reserved_bytes(p.kind,
                                            p.owner_kind,
                                            p.tax_policy,
                                            p.recruit_policy,
                                            p.slots_total,
                                            p.trade_node,
                                            p.administrator,
                                            p.control,
                                            p.corruption,
                                            p.unrest,
                                            p.labour_base,
                                            p.grain_stock,
                                            p.recruit_pool,
                                            p.income_last_month,
                                            p.last_slot_date,
                                            p.capacity,
                                            p.slot_building);
    out.dynasty_used = used_bytes(e.dynasties.manpower_reserve, e.dynasties.bankrupt);
    out.dynasty_allocated = reserved_bytes(e.dynasties.manpower_reserve, e.dynasties.bankrupt);
    const eco::ConstructionQueue& q = e.construction;
    out.construction_used =
        used_bytes(q.province, q.slot, q.building, q.payer, q.start_date, q.finish_date, q.paid_kop, q.seq);
    out.construction_allocated = reserved_bytes(
        q.province, q.slot, q.building, q.payer, q.start_date, q.finish_date, q.paid_kop, q.seq);
    const eco::LoanTable& l = e.loans;
    out.loans_used = used_bytes(l.loan_id,
                                l.borrower,
                                l.principal,
                                l.rate_ppm_year,
                                l.issued,
                                l.due,
                                l.interest_carry,
                                l.overdue,
                                l.collateral);
    out.loans_allocated = reserved_bytes(l.loan_id,
                                         l.borrower,
                                         l.principal,
                                         l.rate_ppm_year,
                                         l.issued,
                                         l.due,
                                         l.interest_carry,
                                         l.overdue,
                                         l.collateral);
    const eco::QuotaTable& qt = e.quotas;
    out.quotas_used = used_bytes(qt.quota_id, qt.dynasty, qt.kind, qt.amount, qt.period_start, qt.deadline);
    out.quotas_allocated =
        reserved_bytes(qt.quota_id, qt.dynasty, qt.kind, qt.amount, qt.period_start, qt.deadline);
    const eco::QuotaOutcomeRing& o = e.outcomes;
    out.outcomes_used = used_bytes(o.seq, o.dynasty, o.kind, o.due, o.delivered, o.settled_on, o.flags);
    out.outcomes_allocated =
        reserved_bytes(o.seq, o.dynasty, o.kind, o.due, o.delivered, o.settled_on, o.flags);
    const eco::IncidentLog& g = e.incidents;
    out.incidents_used = used_bytes(g.seq, g.date, g.province, g.dynasty, g.kind, g.param);
    out.incidents_allocated = reserved_bytes(g.seq, g.date, g.province, g.dynasty, g.kind, g.param);

    // The account table is private and sized exactly: per row the balance, the opening balance, the
    // open lines, and the ring (opening balances and lines of 12 closed months); plus the ring dates.
    constexpr std::size_t kMoney = sizeof(Kopecks);
    constexpr std::size_t kRow = kMoney * (2 + eco::kLedgerLineCount + eco::kLedgerRingMonths +
                                           eco::kLedgerRingMonths * eco::kLedgerLineCount);
    out.ledger_bytes = e.accounts.size() * kRow + eco::kLedgerRingMonths * sizeof(Date);
    return out;
}

}  // namespace pat::stress
