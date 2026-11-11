#pragma once

#include "pat/sim/system.hpp"

#include <memory>
#include <string_view>

namespace pat::sim {

inline constexpr std::string_view kDynastyStandingSystemName = "dynasty.standing";

[[nodiscard]] std::unique_ptr<System> make_dynasty_standing_system();

}  // namespace pat::sim
