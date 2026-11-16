#include "pat/core/bytes.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <limits>
#include <vector>

using namespace pat;

TEST_CASE("ByteReader::can_hold: boundary and overflow") {
    const std::vector<std::uint8_t> data(10, 0);
    core::ByteReader r(data);
    CHECK(r.can_hold(0, 4));
    CHECK(r.can_hold(2, 5));
    CHECK_FALSE(r.can_hold(3, 5));
    CHECK(r.can_hold(10, 1));
    CHECK_FALSE(r.can_hold(11, 1));
    CHECK(r.can_hold(std::numeric_limits<std::uint64_t>::max(), 0));
    CHECK_FALSE(r.can_hold(std::numeric_limits<std::uint64_t>::max(), 2));  // product would overflow
    CHECK_FALSE(r.can_hold(std::numeric_limits<std::uint32_t>::max(), 24));

    (void)r.u32();
    CHECK(r.can_hold(6, 1));
    CHECK_FALSE(r.can_hold(7, 1));
}

TEST_CASE("ByteReader::can_hold: false once the reader has failed") {
    const std::vector<std::uint8_t> data(4, 0);
    core::ByteReader r(data);
    (void)r.u64();  // reads past the end
    REQUIRE_FALSE(r.ok());
    CHECK_FALSE(r.can_hold(0, 1));
}
