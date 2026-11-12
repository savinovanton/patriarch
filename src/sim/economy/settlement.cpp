#include "pat/sim/economy/settlement.hpp"

#include "pat/core/assert.hpp"
#include "pat/sim/economy/loans.hpp"
#include "pat/sim/economy/taxes.hpp"
#include "pat/sim/world.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace pat::sim::economy {

namespace {

/// The account of row `row` of the table: row 0 is the crown, row `1 + d` is dynasty `d`.
[[nodiscard]] AccountRef account_of_row(std::size_t row) noexcept {
    return row == 0 ? AccountRef::crown() : AccountRef::of(DynastyId(static_cast<std::uint32_t>(row - 1)));
}

#if PAT_ASSERTS_ENABLED
/// After the rotation every account must start the new month clean: the balance is the opening balance
/// and no line is open (E-M1 for an empty month).
[[nodiscard]] bool books_start_clean(const World& world) noexcept {
    const AccountTable& accounts = world.economy().accounts;
    for (std::size_t row = 0; row < accounts.size(); ++row) {
        if (accounts.treasury(row) != accounts.opening(row)) {
            return false;
        }
        for (const Kopecks line : accounts.open_lines(row)) {
            if (line != 0) {
                return false;
            }
        }
    }
    return true;
}
#endif

}  // namespace

bool is_expense_source_code(LedgerCode code) noexcept {
    switch (code) {
        case LedgerCode::CourtUpkeep:
        case LedgerCode::SalariesOut:
        case LedgerCode::ArmyUpkeep:
        case LedgerCode::GiftsBribes:
        case LedgerCode::ChurchDonations:
        case LedgerCode::DowryOut:
            return true;
        default:
            return false;
    }
}

UpkeepTable make_upkeep_table(const BuildingRules& rules) noexcept {
    UpkeepTable table{};
    const auto count = static_cast<std::size_t>(
        std::clamp<std::int32_t>(rules.count, 0, static_cast<std::int32_t>(kMaxBuildingTypes)));
    for (std::size_t i = 0; i < count; ++i) {
        const BuildingDef& def = rules.defs[i];
        if (def.slot != 0) {
            table[def.slot] = std::clamp<Kopecks>(def.upkeep_kop_month, 0, kMaxPostingKop);
        }
    }
    return table;
}

Kopecks province_building_upkeep(const World& world, const UpkeepTable& table, ProvinceId province) noexcept {
    if (!world.economy_present() || !world.exists(province)) {
        return 0;
    }
    const ProvinceEconomy& pe = world.economy().provinces;
    const std::size_t p = province.index();
    const std::size_t slots = std::min<std::size_t>(pe.slots_total[p], kMaxSlots);
    Kopecks total = 0;
    for (std::size_t slot = 0; slot < slots; ++slot) {
        total = saturating_add(total, table[pe.slot_building[p * kMaxSlots + slot]]);
    }
    return total;
}

std::vector<Kopecks> building_upkeep_by_account(const World& world, const BuildingRules& rules) {
    if (!world.economy_present()) {
        return {};
    }
    const UpkeepTable table = make_upkeep_table(rules);
    std::vector<Kopecks> by_account(world.economy().accounts.size(), 0);
    const auto count = static_cast<std::uint32_t>(world.provinces().size());
    for (std::uint32_t index = 0; index < count; ++index) {
        const ProvinceId province(index);
        const Kopecks upkeep = province_building_upkeep(world, table, province);
        if (upkeep == 0) {
            continue;
        }
        const auto payer = income_recipient(world, province);
        const auto row = payer ? world.resolve_account(*payer) : std::nullopt;
        if (row) {
            by_account[*row] = saturating_add(by_account[*row], upkeep);
        }
    }
    return by_account;
}

SettlementReport
settle_month(World& world, const EconomyRules& rules, const ExpenseSources& sources, Date month_start) {
    SettlementReport report;
    if (!world.economy_present() || month_start.day() != 1 ||
        month_start <= world.economy().meta.open_month_start) {
        return report;
    }
    report.closed_month_start = world.economy().meta.open_month_start;

    // Step 1: the flat expenses of the closing month, account by account in ascending row order.
    const std::vector<Kopecks> upkeep = building_upkeep_by_account(world, rules.buildings);
    const std::size_t accounts = world.economy().accounts.size();
    const DynastyId holder = world.economy().meta.crown_dynasty;  // invalid unless Q7 "one treasury"
    std::vector<ExpenseLine> lines;
    const auto collect = [&](AccountRef who) {
        lines.clear();
        for (const auto& source : sources) {
            if (source) {
                source->collect(world, who, report.closed_month_start, lines);
            }
        }
        // ascending by code; equal codes keep the order of the sources
        std::stable_sort(lines.begin(), lines.end(), [](const ExpenseLine& a, const ExpenseLine& b) {
            return static_cast<std::uint16_t>(a.code) < static_cast<std::uint16_t>(b.code);
        });
        for (const ExpenseLine& line : lines) {
            PAT_ASSERT(is_expense_source_code(line.code),
                       "an expense source used a code that is not its own");
            if (line.amount_kop > 0 && is_expense_source_code(line.code)) {
                const Kopecks posted = -world.post_money(who, line.code, -line.amount_kop);
                report.other_expenses += posted;
                report.refused_kop = saturating_add(report.refused_kop, line.amount_kop - posted);
            }
        }
    };
    for (std::size_t row = 0; row < accounts; ++row) {
        const AccountRef who = account_of_row(row);
        // In the "one treasury" mode row 0 is dead: the crown's money lives in the holder's row.
        const bool dead_crown_row = row == 0 && world.resolve_account(AccountRef::crown()) != std::size_t{0};
        if (dead_crown_row) {
            continue;
        }
        if (upkeep[row] > 0) {
            const Kopecks posted = -world.post_money(who, LedgerCode::BuildingUpkeep, -upkeep[row]);
            report.building_upkeep += posted;
            report.refused_kop = saturating_add(report.refused_kop, upkeep[row] - posted);
        }
        collect(who);
        if (holder.valid() && row == 1 + holder.index()) {
            collect(AccountRef::crown());  // the same row: the crown's own expenses
        }
    }

    // Step 2: interest and first overdue dates, in stable loan-id order. Quotas belong to E9-T14.
    const LoanSettlementReport loans = world.settle_loans(rules.loans, month_start);
    report.loan_interest = loans.interest_kop;
    report.refused_kop = saturating_add(report.refused_kop, loans.refused_kop);

    // Step 3: rotation of the ring; the month's lines become history, the open ones restart at zero.
    const bool rotated = world.close_ledger_month(month_start);
    PAT_ASSERT(rotated, "the month to close was checked to be earlier than the next one");
    report.closed = rotated;
    // Step 4: the books of the new month start clean (the full E-M1/E-M2 check is check_invariants).
#if PAT_ASSERTS_ENABLED
    PAT_ASSERT(books_start_clean(world), "after the rotation every account must start the month clean");
#endif
    return report;
}

bool can_afford(const World& world, AccountRef account, Kopecks cost_kop) noexcept {
    const auto row = world.resolve_account(account);
    if (!row) {
        return false;
    }
    return cost_kop <= 0 || world.economy().accounts.treasury(*row) >= cost_kop;
}

}  // namespace pat::sim::economy
