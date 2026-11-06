#include "pat/core/build_info.hpp"

#include <doctest/doctest.h>

TEST_CASE("core: version is reported") {
    CHECK_FALSE(pat::core::version().empty());
    CHECK(pat::core::version() == "0.0.1");
}

TEST_CASE("core: build is deterministic by contract") {
    static_assert(pat::core::deterministic_build());
    CHECK(pat::core::deterministic_build());
}
