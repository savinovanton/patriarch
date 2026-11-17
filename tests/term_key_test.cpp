#include "pat/sim/economy/breakdown.hpp"

#include <doctest/doctest.h>

#include <array>

using namespace pat::sim::economy;

TEST_CASE("find_term_key: binary search over a sorted table") {
    constexpr std::array table{
        TermKey{2, "a.two"},
        TermKey{5, "a.five"},
        TermKey{9, "a.nine"},
        TermKey{10000, "a.big"},
    };
    static_assert(is_sorted_term_table(table));
    for (const TermKey& row : table) {
        CHECK(find_term_key(table, row.code) == row.key);
    }
    CHECK(find_term_key(table, 0).empty());
    CHECK(find_term_key(table, 3).empty());
    CHECK(find_term_key(table, 10).empty());
    CHECK(find_term_key(table, 65535).empty());
    CHECK(find_term_key({}, 2).empty());
}

TEST_CASE("is_sorted_term_table: rejects unsorted and duplicate codes") {
    constexpr std::array unsorted{TermKey{5, "x"}, TermKey{2, "y"}};
    constexpr std::array duplicate{TermKey{2, "x"}, TermKey{2, "y"}};
    CHECK_FALSE(is_sorted_term_table(unsorted));
    CHECK_FALSE(is_sorted_term_table(duplicate));
    CHECK(is_sorted_term_table({}));
}
