#pragma once

#include "pat/sim/simulation.hpp"

#include <cstdint>
#include <memory>

namespace pat::stress {

/// Names of the economy systems (docs/dev/economy-model.md section 7.1). The telemetry metric of a
/// system is `system.<name>`, so these show up as `system.economy.settlement` and so on.
inline constexpr const char* kEconomySettlementName = "economy.settlement";
inline constexpr const char* kEconomyConstructionName = "economy.construction";
inline constexpr const char* kEconomyProvincesName = "economy.provinces";

/// What the empty systems counted. Diagnostics of the tool: it lives outside the World, so it is never
/// saved or hashed, and nothing reads it to decide anything.
struct EconomySkeletonCounters {
    std::uint64_t settlement_runs = 0;    ///< months on which `economy.settlement` ran
    std::uint64_t construction_runs = 0;  ///< days on which `economy.construction` ran
    std::uint64_t province_runs = 0;      ///< days on which `economy.provinces` ran
    std::uint64_t slots_visited = 0;      ///< provinces that fell into the stagger slot of their day
};

/// The skeleton of the economy tick of E9-T05: three systems with the names, cadences and order of
/// the spec (settlement monthly, then construction and provinces daily). Construction is still an empty
/// stub that only counts; `economy.settlement` and `economy.provinces` count and then run the real
/// systems (E9-T09: closing of the month; E9-T06..T08: growth, production, taxes; all with
/// EconomyRules::defaults()). E9-T16 replaces the construction stub with the real system under the
/// same name; the telemetry metrics (`system.economy.*`) carry over,
/// and the same loaded world is the workload.
///
/// Registers the systems (this disables telemetry, as every add_system does: enable it afterwards)
/// and returns the counters they update.
std::shared_ptr<EconomySkeletonCounters> install_economy_skeleton(sim::Simulation& simulation);

}  // namespace pat::stress
