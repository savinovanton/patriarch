#include <doctest/doctest.h>

#include <cstdint>
#include <limits>

#include "cli_args.hpp"

using pat::tools::parse_u64;

TEST_CASE("parse_u64: accepts plain decimal numbers up to the 64-bit limit") {
    CHECK(parse_u64("0") == 0);
    CHECK(parse_u64("7") == 7);
    CHECK(parse_u64("0042") == 42);
    CHECK(parse_u64("18446744073709551615") == std::numeric_limits<std::uint64_t>::max());
}

TEST_CASE("parse_u64: rejects garbage, signs, spaces, empty and overflow") {
    CHECK_FALSE(parse_u64("").has_value());
    CHECK_FALSE(parse_u64("12abc").has_value());
    CHECK_FALSE(parse_u64("abc").has_value());
    CHECK_FALSE(parse_u64("-1").has_value());
    CHECK_FALSE(parse_u64("+1").has_value());
    CHECK_FALSE(parse_u64(" 1").has_value());
    CHECK_FALSE(parse_u64("1 ").has_value());
    CHECK_FALSE(parse_u64("0x10").has_value());
    CHECK_FALSE(parse_u64("1.5").has_value());
    CHECK_FALSE(parse_u64("18446744073709551616").has_value());
}
