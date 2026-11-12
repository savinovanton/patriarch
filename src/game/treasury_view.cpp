#include "pat/game/economy_view.hpp"
#include "pat/game/tooltip_view.hpp"
#include "pat/game/world_view.hpp"
#include "pat/sim/economy/forecast.hpp"
#include "pat/sim/economy/ledger.hpp"

#include <algorithm>
#include <array>
#include <span>
#include <string>

namespace pat::game {

namespace {

using sim::economy::AccountTable;
using sim::economy::Kopecks;

ui::EconomyNumber money(const loc::Localizer& loc, Kopecks value, std::string_view label_key) {
    return recorded_economy_value(loc, ui::EconomyUnit::Kopecks, value, label_key);
}

ui::Explanation money_term(const loc::Localizer& loc, Kopecks value, std::string_view label_key) {
    return {loc.format(label_key), format_money(loc, value, true), "", {}};
}

std::vector<ui::TreasuryLineSnapshot> statement_lines(const loc::Localizer& loc,
                                                      std::span<const Kopecks> amounts) {
    std::vector<ui::TreasuryLineSnapshot> lines;
    lines.reserve(sim::economy::kLedgerLineCount);
    for (std::size_t i = 0; i < sim::economy::kLedgerLineCount; ++i) {
        const auto code = static_cast<std::uint16_t>(sim::economy::kLedgerCodes[i]);
        const std::string suffix = "code" + std::to_string(code);
        const std::string key = "ui.treasury.line." + suffix;
        lines.push_back({code, loc.format(key), money(loc, amounts[i], key)});
    }
    return lines;
}

ui::EconomyNumber statement_net(const loc::Localizer& loc,
                                Kopecks value,
                                const std::vector<ui::TreasuryLineSnapshot>& lines,
                                std::string_view label_key) {
    auto out = money(loc, value, label_key);
    out.explanation.terms.clear();
    for (const auto& line : lines) {
        out.explanation.terms.push_back({line.name, format_money(loc, line.amount.value, true), "", {}});
    }
    return out;
}

ui::TreasuryMonthSnapshot statement_month(const loc::Localizer& loc,
                                          bool available,
                                          sim::Date start,
                                          Kopecks opening,
                                          std::span<const Kopecks> amounts) {
    ui::TreasuryMonthSnapshot out;
    out.available = available;
    if (available) {
        out.date = format_date(loc, start);
    }
    out.opening = money(loc, opening, "ui.treasury.opening");
    out.lines = statement_lines(loc, amounts);
    const Kopecks net = AccountTable::sum(amounts);
    out.net = statement_net(loc, net, out.lines, "ui.treasury.net");
    out.closing = money(loc, sim::economy::saturating_add(opening, net), "ui.treasury.closing");
    out.closing.explanation.terms = {money_term(loc, opening, "ui.treasury.opening"), out.net.explanation};
    return out;
}

std::string
account_name(const loc::Localizer& loc, sim::economy::AccountRef account, const EconomyViewOptions& options) {
    if (account.kind == sim::economy::AccountKind::Crown) {
        return loc.format("ui.treasury.owner.crown");
    }
    if (options.dynasty_name) {
        const std::string name = options.dynasty_name(account.dynasty);
        if (!name.empty()) {
            return name;
        }
    }
    return loc.format("ui.treasury.owner.dynasty", {loc::Arg("id", account.dynasty.index() + 1)});
}

}  // namespace

ui::TreasurySnapshot treasury_snapshot(const loc::Localizer& loc,
                                       const sim::World& world,
                                       sim::economy::AccountRef account,
                                       const sim::economy::EconomyRules& rules,
                                       const EconomyViewOptions& options) {
    ui::TreasurySnapshot out;
    const auto row = world.resolve_account(account);
    if (!row) {
        return out;
    }
    const auto& economy = world.economy();
    const AccountTable& accounts = economy.accounts;
    out.available = true;
    out.owner = account_name(loc, account, options);
    out.current = statement_month(
        loc, true, economy.meta.open_month_start, accounts.opening(*row), accounts.open_lines(*row));
    out.treasury = money(loc, accounts.treasury(*row), "ui.treasury.balance");
    out.treasury.explanation.terms = out.current.closing.explanation.terms;

    const std::array<Kopecks, sim::economy::kLedgerLineCount> zero_lines{};
    const std::size_t closed = std::min<std::size_t>(economy.meta.closed_months, out.history.size());
    for (std::size_t age = 0; age < out.history.size(); ++age) {
        out.history[age] = age < closed ? statement_month(loc,
                                                          true,
                                                          accounts.ring_month_start(age),
                                                          accounts.ring_opening(*row, age),
                                                          accounts.ring_lines(*row, age))
                                        : statement_month(loc, false, {}, 0, zero_lines);
    }

    const sim::economy::StaticPriceSource static_prices(rules.production);
    const sim::economy::PriceSource& prices = options.prices != nullptr ? *options.prices : static_prices;
    const sim::economy::ExpenseSources no_expenses;
    const sim::economy::ExpenseSources& expenses =
        options.expenses != nullptr ? *options.expenses : no_expenses;
    const auto forecast = sim::economy::forecast_treasury(world, rules, prices, expenses, account);
    out.projected_lines = statement_lines(loc, forecast.monthly_lines);
    out.projected_monthly_net =
        statement_net(loc, forecast.monthly_net, out.projected_lines, "ui.treasury.projected_net");
    Kopecks previous = forecast.treasury;
    for (std::size_t month = 0; month < out.projected_balances.size(); ++month) {
        const Kopecks next = forecast.balance[month];
        const Kopecks delta = sim::economy::saturating_sub(next, previous);
        auto& balance = out.projected_balances[month];
        balance = money(loc, next, "ui.treasury.projected_balance");
        balance.explanation.terms = {money_term(loc, previous, "ui.treasury.previous_balance"),
                                     money_term(loc, delta, "ui.treasury.projected_change")};
        previous = next;
    }
    return out;
}

}  // namespace pat::game
