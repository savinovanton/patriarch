#include "pat/core/ids.hpp"
#include "pat/core/rng.hpp"
#include "pat/core/sparse_set.hpp"

#include <doctest/doctest.h>

#include <set>
#include <vector>

using namespace pat::core;

TEST_CASE("sparse_set: insert, contains, erase") {
    SparseSet<> set;
    CHECK(set.insert(5));
    CHECK_FALSE(set.insert(5));
    CHECK(set.insert(100));
    CHECK(set.size() == 2);
    CHECK(set.contains(5));
    CHECK(set.contains(100));
    CHECK_FALSE(set.contains(6));
    CHECK_FALSE(set.contains(100000));  // beyond the sparse array

    CHECK(set.erase(5));
    CHECK_FALSE(set.erase(5));
    CHECK_FALSE(set.contains(5));
    CHECK(set.size() == 1);
}

TEST_CASE("sparse_set: dense order is insertion order with swap-remove on erase") {
    SparseSet<> set;
    for (std::uint32_t v : {10U, 20U, 30U, 40U}) {
        (void)set.insert(v);
    }
    CHECK(std::vector<std::uint32_t>(set.begin(), set.end()) == std::vector<std::uint32_t>{10, 20, 30, 40});

    (void)set.erase(20);  // last element (40) moves into its place
    CHECK(std::vector<std::uint32_t>(set.begin(), set.end()) == std::vector<std::uint32_t>{10, 40, 30});
    CHECK(set.contains(40));
    CHECK(set.contains(30));
}

TEST_CASE("sparse_set: works with strong ids and clear()") {
    using Id = StrongId<struct SparseSetTag>;
    SparseSet<Id> set;
    CHECK(set.insert(Id(3)));
    CHECK(set.insert(Id(8)));
    CHECK(set.contains(Id(8)));
    set.clear();
    CHECK(set.empty());
    CHECK_FALSE(set.contains(Id(3)));
    CHECK(set.insert(Id(3)));
}

TEST_CASE("sparse_set: random operations agree with std::set and are reproducible") {
    auto run = [](std::uint64_t seed) {
        SparseSet<> set;
        std::set<std::uint32_t> model;
        Pcg32 rng(seed, 1);
        for (int step = 0; step < 20000; ++step) {
            const std::uint32_t key = rng.bounded(300);
            if (rng.chance_permille(550)) {
                CHECK(set.insert(key) == model.insert(key).second);
            } else {
                CHECK(set.erase(key) == (model.erase(key) > 0));
            }
            REQUIRE(set.size() == model.size());
        }
        for (const std::uint32_t key : model) {
            CHECK(set.contains(key));
        }
        return std::vector<std::uint32_t>(set.begin(), set.end());
    };

    const auto first = run(7);
    const auto second = run(7);
    CHECK(first == second);  // same operation sequence -> same dense order
}
