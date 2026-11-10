#pragma once

#include "pat/sim/economy/production.hpp"
#include "pat/sim/economy/rules.hpp"
#include "pat/sim/economy/settlement.hpp"
#include "pat/sim/simulation.hpp"
#include "pat/sim/system.hpp"

#include <memory>
#include <string_view>

// The systems of the economy (E9-T06/T07, docs/dev/economy-model.md section 7).
//
// The economy is NOT part of the demo or the dynasty scenarios: their Simulation never gets these
// systems (their goldens stay as they are). A game or a tool that has a World with the economy on
// calls install_economy_systems() once, before the first tick, and passes the rules (content, not
// state: the systems keep their own copy, the World does not know them) and, optionally, the source of
// the prices (default: the static prices of the rules; E19 plugs the prices of trade nodes in).
//
// Order of registration is the order of execution (spec section 7.1):
//   1. economy.settlement   Monthly  closing of the month (E9-T09)
//   2. economy.construction Daily    finished buildings (E9-T16)           - not yet
//   3. economy.provinces    Daily    the monthly pass of a province on its slot day  - E9-T06, T07, T08
// E9-T16 adds 2 between the others inside install_economy_systems, so callers do not change.

namespace pat::sim::economy {

inline constexpr std::string_view kSettlementSystemName = "economy.settlement";
inline constexpr std::string_view kProvincesSystemName = "economy.provinces";

/// The system `economy.settlement` (E9-T09): on the 1st of every month, before anything else of the
/// day, closes the month that ended (settle_month: flat expenses of the buildings and of the
/// `expenses` sources, rotation of the ring of the statement). It runs BEFORE `economy.provinces`
/// (registration order), so the postings of the slots of the 1st go to the new month. Does nothing in
/// a world without economy. `expenses` are the sources of flat expenses of other epics (none in M1).
/// No random numbers; the system keeps no state of its own.
[[nodiscard]] std::unique_ptr<System> make_settlement_system(const EconomyRules& rules,
                                                             ExpenseSources expenses = {});

/// The system `economy.provinces`: every day it takes the provinces whose slot is today
/// (`is_monthly_slot(province_index, date)`, index % 28 == day - 1, days 1..28; days 29..31 take
/// nobody), so every province is processed exactly once in a month of any length. For each one it
/// runs the steps of section 7.3 in their fixed order; so far: step 2 (growth of the population, E9-T06),
/// step 3 (production, consumption, grain stock and wealth, E9-T07), step 4 (taxes and holding income
/// posted to the owner, E9-T08) and step 8 (`last_slot_date`; the
/// population cache is refreshed by the World). `prices` (shared, immutable; null = the static prices
/// of `rules.production`) is asked for the price of every good a province makes.
/// Does nothing in a world without economy. No random numbers.
[[nodiscard]] std::unique_ptr<System>
make_provinces_system(const EconomyRules& rules, std::shared_ptr<const PriceSource> prices = nullptr);

/// Registers the systems of the economy in the order of the spec. Call before the first tick.
/// `expenses`: the flat-expense sources of other epics for the settlement (E9-T09; none in M1).
void install_economy_systems(Simulation& simulation,
                             const EconomyRules& rules,
                             std::shared_ptr<const PriceSource> prices = nullptr,
                             ExpenseSources expenses = {});

}  // namespace pat::sim::economy
