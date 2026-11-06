#include "pat/sim/sim.hpp"

#include <doctest/doctest.h>

TEST_CASE("sim: one tick is one day") {
    CHECK(pat::sim::days_for_ticks(0) == 0);
    CHECK(pat::sim::days_for_ticks(365) == 365);
}
