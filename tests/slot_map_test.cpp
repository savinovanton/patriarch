#include "pat/core/rng.hpp"
#include "pat/core/slot_map.hpp"

#include <doctest/doctest.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace pat::core;

TEST_CASE("slot_map: insert, get and erase") {
    SlotMap<std::string> map;
    const auto a = map.insert("alpha");
    const auto b = map.insert("beta");
    CHECK(map.size() == 2);
    REQUIRE(map.get(a) != nullptr);
    CHECK(*map.get(a) == "alpha");
    CHECK(map.at(b) == "beta");

    CHECK(map.erase(a));
    CHECK(map.size() == 1);
    CHECK(map.get(a) == nullptr);
    CHECK_FALSE(map.contains(a));
    CHECK_FALSE(map.erase(a));  // already erased
    CHECK(map.contains(b));
}

TEST_CASE("slot_map: stale handles never match a reused slot") {
    SlotMap<int> map;
    const auto first = map.insert(10);
    CHECK(map.erase(first));
    const auto second = map.insert(20);

    CHECK(second.index == first.index);            // slot is reused ...
    CHECK(second.generation != first.generation);  // ... under a new generation
    CHECK(map.get(first) == nullptr);
    REQUIRE(map.get(second) != nullptr);
    CHECK(*map.get(second) == 20);
    CHECK_FALSE(map.erase(first));
    CHECK(map.size() == 1);
}

TEST_CASE("slot_map: invalid and out-of-range handles are rejected") {
    SlotMap<int> map;
    (void)map.insert(1);
    CHECK(map.get(Handle<int>{}) == nullptr);
    CHECK(map.get(Handle<int>{999, 1}) == nullptr);
    CHECK_FALSE(map.erase(Handle<int>{}));
}

TEST_CASE("slot_map: iteration is in slot order and skips holes") {
    SlotMap<int> map;
    const auto h0 = map.insert(0);
    const auto h1 = map.insert(1);
    const auto h2 = map.insert(2);
    const auto h3 = map.insert(3);
    (void)h0;
    (void)h3;
    map.erase(h1);
    map.erase(h2);

    std::vector<int> seen;
    for (const auto entry : map) {
        seen.push_back(entry.value);
        CHECK(map.get(entry.handle) != nullptr);
    }
    CHECK(seen == std::vector<int>{0, 3});

    // The most recently freed slot (2) is reused first (LIFO), the layout is a pure function of operations.
    const auto n = map.insert(99);
    CHECK(n.index == h2.index);
}

TEST_CASE("slot_map: move-only values and emplace") {
    SlotMap<std::unique_ptr<int>> map;
    const auto h = map.emplace(std::make_unique<int>(5));
    REQUIRE(map.get(h) != nullptr);
    CHECK(**map.get(h) == 5);
}

TEST_CASE("slot_map: clear makes all handles stale") {
    SlotMap<int> map;
    const auto a = map.insert(1);
    const auto b = map.insert(2);
    map.clear();
    CHECK(map.empty());
    CHECK(map.get(a) == nullptr);
    CHECK(map.get(b) == nullptr);
    const auto c = map.insert(3);
    CHECK(c.index == 0);  // lowest slot reused first
    CHECK(map.get(a) == nullptr);
    CHECK(*map.get(c) == 3);
}

TEST_CASE("slot_map: random operations agree with a reference model") {
    SlotMap<int> map;
    std::map<std::pair<std::uint32_t, std::uint32_t>, int> model;  // (index, generation) -> value
    std::vector<Handle<int>> issued;
    Pcg32 rng(2025, 3);

    for (int step = 0; step < 20000; ++step) {
        const std::uint32_t op = rng.bounded(100);
        if (op < 50 || issued.empty()) {
            const int value = static_cast<int>(rng.bounded(1000000));
            const auto h = map.insert(value);
            model[{h.index, h.generation}] = value;
            issued.push_back(h);
        } else if (op < 80) {
            const auto h = issued[rng.bounded(static_cast<std::uint32_t>(issued.size()))];
            const bool live = model.contains({h.index, h.generation});
            CHECK(map.erase(h) == live);
            model.erase({h.index, h.generation});
        } else {
            const auto h = issued[rng.bounded(static_cast<std::uint32_t>(issued.size()))];
            const auto it = model.find({h.index, h.generation});
            const int* value = map.get(h);
            if (it == model.end()) {
                CHECK(value == nullptr);
            } else {
                REQUIRE(value != nullptr);
                CHECK(*value == it->second);
            }
        }
        REQUIRE(map.size() == model.size());
    }
}
