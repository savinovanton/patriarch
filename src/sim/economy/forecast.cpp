#include "pat/sim/economy/forecast.hpp"

#include "pat/sim/economy/taxes.hpp"
#include "pat/sim/world.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace pat::sim::economy {

namespace {

void add_line(TreasuryForecast& forecast, LedgerCode code, Kopecks amount) noexcept {
    const auto index = ledger_index(code);
    if (index && amount != 0) {
        forecast.monthly_lines[*index] = saturating_add(forecast.monthly_lines[*index], amount);
    }
}

}  // namespace

TreasuryForecast forecast_treasury(const World& world,
                                   const EconomyRules& rules,
                                   const PriceSource& prices,
                                   const ExpenseSources& sources,
                                   AccountRef account) {
    TreasuryForecast out;
    const auto row = world.resolve_account(account);
    if (!row) {
        return out;
    }
    out.treasury = world.economy().accounts.treasury(*row);

    // Income: the same split as post_province_income, province by province. A province matters to this
    // account when it is the one that gets its income, or, for the crown, when it is a city of a dynasty
    // (the crown's share of the taxes of the townsfolk).
    const bool is_crown_row = world.resolve_account(AccountRef::crown()) == row;
    const ProvinceEconomy& pe = world.economy().provinces;
    const auto count = static_cast<std::uint32_t>(world.provinces().size());
    for (std::uint32_t index = 0; index < count; ++index) {
        const ProvinceId province(index);
        const auto payee = income_recipient(world, province);
        const bool is_owner = payee && world.resolve_account(*payee) == row;
        const bool gives_crown_share = is_crown_row &&
                                       static_cast<ProvinceKind>(pe.kind[index]) == ProvinceKind::City &&
                                       static_cast<OwnerKind>(pe.owner_kind[index]) == OwnerKind::Dynasty;
        if (!is_owner && !gives_crown_share) {
            continue;
        }
        const ProvinceProduction production =
            compute_province_production(rules, production_input(world, prices, province));
        const ProvinceIncome income =
            compute_province_income(rules, tax_input(world, rules, province), production.holding_income_kop);
        for (std::size_t g = 0; g < kGroupCount; ++g) {
            const GroupTax& tax = income.group[g];
            const LedgerCode code = tax_code(static_cast<Group>(g));
            if (is_owner) {
                add_line(out, code, tax.gross - tax.crown);
                add_line(out, LedgerCode::Embezzlement, -tax.leak);
            }
            if (is_crown_row && tax.crown > 0) {
                add_line(out, code, tax.crown);
            }
        }
        if (is_owner) {
            add_line(out, LedgerCode::HoldingIncome, income.holding);
        }
    }

    // Expenses: what the settlement would post for the next month.
    const std::vector<Kopecks> upkeep = building_upkeep_by_account(world, rules.buildings);
    add_line(out, LedgerCode::BuildingUpkeep, -upkeep[*row]);
    const Date next_month = world.today().first_of_next_month();
    std::vector<ExpenseLine> lines;
    const auto ask = [&](AccountRef who) {
        lines.clear();
        for (const auto& source : sources) {
            if (source) {
                source->collect(world, who, next_month, lines);
            }
        }
        for (const ExpenseLine& line : lines) {
            if (line.amount_kop > 0 && is_expense_source_code(line.code)) {
                add_line(out, line.code, -line.amount_kop);
            }
        }
    };
    // In the "one treasury" mode row 0 is dead and the holder's row also carries the crown's expenses.
    const DynastyId holder = world.economy().meta.crown_dynasty;
    if (*row == 0) {
        ask(AccountRef::crown());
    } else {
        ask(AccountRef::of(DynastyId(static_cast<std::uint32_t>(*row - 1))));
        if (holder.valid() && *row == 1 + holder.index()) {
            ask(AccountRef::crown());
        }
    }

    for (const Kopecks line : out.monthly_lines) {
        out.monthly_net = saturating_add(out.monthly_net, line);
    }
    Kopecks balance = out.treasury;
    for (std::size_t month = 0; month < kForecastMonths; ++month) {
        balance = std::clamp(saturating_add(balance, out.monthly_net), -kMaxTreasuryKop, kMaxTreasuryKop);
        out.balance[month] = balance;
    }
    return out;
}

}  // namespace pat::sim::economy
