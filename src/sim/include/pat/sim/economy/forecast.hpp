#pragma once

#include "pat/sim/economy/ledger.hpp"
#include "pat/sim/economy/money.hpp"
#include "pat/sim/economy/production.hpp"
#include "pat/sim/economy/rules.hpp"
#include "pat/sim/economy/settlement.hpp"
#include "pat/sim/economy/tables.hpp"

#include <array>
#include <cstddef>

// Forecast of the treasury for the next 12 months (E9-T09, docs/dev/economy-model.md decision D12,
// GDD 03 section 7.3).
//
// The forecast is a DERIVED value: a pure function of the World and the rules, computed on demand
// (for the window of the treasury and the top bar). It is never stored: it is not in the World, the
// save, the state hash or `operator==`; two worlds that are equal have equal forecasts.
//
// It is the derivative of the CURRENT rates: the month that the provinces would give if nothing
// changed (population, wealth, control, corruption, policies and prices as they are now), by the same
// functions the simulation uses (compute_province_production, compute_province_income) and the same
// split between the owner and the crown (post_province_income), plus the flat expenses of the
// settlement (upkeep of the buildings, the ExpenseSources) and interest on outstanding loans. The
// recurring month repeats twelve times; loan carry advances each month using the settlement formula.
// Balances stay within the bounds of an account. Growth, harvests, riots, new buildings, new loans
// and voluntary repayments are not predicted (they are not "current rates").
//
// The forecast reads the whole province table once: O(provinces), a few microseconds per province
// that belongs to the account; it is meant to be asked a few times per frame at most, never per tick.

namespace pat::sim {
class World;
}

namespace pat::sim::economy {

/// How many months ahead the forecast looks.
inline constexpr std::size_t kForecastMonths = kLedgerRingMonths;

/// The treasury of one account in the next kForecastMonths months.
struct TreasuryForecast {
    /// The balance now.
    Kopecks treasury = 0;
    /// The expected lines of the next month, by the dense index of the ledger code (income positive,
    /// expense negative) - the same layout as a month of the statement.
    std::array<Kopecks, kLedgerLineCount> monthly_lines{};
    /// The sum of `monthly_lines`: what the balance changes by in the next month.
    Kopecks monthly_net = 0;
    /// The balance at the end of month 1, 2, ..., kForecastMonths from now.
    std::array<Kopecks, kForecastMonths> balance{};

    [[nodiscard]] bool operator==(const TreasuryForecast&) const = default;
};

/// The forecast of `account`. Zeroes (all lines and balances) for an account that does not exist or
/// in a world without economy. `prices` is the source the simulation uses (the static prices of the
/// rules unless E19 plugs in the trade nodes); `sources` the expense sources of the settlement.
[[nodiscard]] TreasuryForecast forecast_treasury(const World& world,
                                                 const EconomyRules& rules,
                                                 const PriceSource& prices,
                                                 const ExpenseSources& sources,
                                                 AccountRef account);

}  // namespace pat::sim::economy
