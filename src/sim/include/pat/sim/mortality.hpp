#pragma once

#include "pat/sim/demography.hpp"
#include "pat/sim/system.hpp"

#include <memory>
#include <string_view>

namespace pat::sim {

inline constexpr std::string_view kMortalitySystemName = "dynasty.mortality";

/// Processes life events daily. Rules must pass demography validation and are copied into the system.
[[nodiscard]] std::unique_ptr<System> make_mortality_system(const DemographyRules& rules);

}  // namespace pat::sim
