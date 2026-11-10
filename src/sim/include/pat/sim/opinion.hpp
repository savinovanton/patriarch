#pragma once

#include "pat/sim/system.hpp"

#include <memory>
#include <string_view>

namespace pat::sim {

inline constexpr std::string_view kOpinionSystemName = "dynasty.opinions";

[[nodiscard]] std::unique_ptr<System> make_opinion_system();

}  // namespace pat::sim
