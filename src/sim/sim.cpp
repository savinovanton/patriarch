#include "pat/sim/sim.hpp"

namespace pat::sim {

// One tick equals one in-game day (TZ FR-TIME-1).
std::int64_t days_for_ticks(std::int64_t ticks) noexcept {
    return ticks;
}

}  // namespace pat::sim
