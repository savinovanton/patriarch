#include "pat/core/arena.hpp"

#include <doctest/doctest.h>

#include <cstdint>

using namespace pat::core;

namespace {
// Exactly 32 bytes of data, so alignas(32) adds no padding (avoids warning C4324).
struct alignas(32) Wide {
    std::uint64_t a = 1;
    std::uint64_t b = 2;
    std::uint64_t c = 3;
    std::uint64_t d = 4;
};
struct Point {
    int x;
    int y;
};
}  // namespace

TEST_CASE("arena: allocations respect alignment and do not overlap") {
    Arena arena(1024);
    auto* a = static_cast<std::uint8_t*>(arena.allocate(3, 1));
    auto* b = static_cast<std::uint64_t*>(arena.allocate(8, 8));
    auto* c = arena.create<Wide>();

    CHECK(reinterpret_cast<std::uintptr_t>(b) % 8 == 0);
    CHECK(reinterpret_cast<std::uintptr_t>(c) % 32 == 0);
    CHECK(c->a == 1);
    CHECK(c->b == 2);

    *b = 0x1122334455667788ULL;
    a[0] = a[1] = a[2] = 0xAB;
    CHECK(*b == 0x1122334455667788ULL);
    CHECK(arena.bytes_used() == 3 + 8 + sizeof(Wide));
}

TEST_CASE("arena: create and allocate_array") {
    Arena arena;
    Point* p = arena.create<Point>(Point{3, 4});
    CHECK(p->x == 3);
    CHECK(p->y == 4);

    auto items = arena.allocate_array<int>(100);
    CHECK(items.size() == 100);
    for (const int v : items) {
        CHECK(v == 0);  // value-initialized
    }
    items[99] = 5;
    CHECK(items[99] == 5);
}

TEST_CASE("arena: grows by blocks and serves oversized requests") {
    Arena arena(512);
    for (int i = 0; i < 100; ++i) {
        void* p = arena.allocate(100, 8);
        REQUIRE(p != nullptr);
    }
    CHECK(arena.block_count() > 1);

    void* big = arena.allocate(10000, 16);
    REQUIRE(big != nullptr);
    CHECK(arena.bytes_reserved() >= 10000);
}

TEST_CASE("arena: reset reuses the memory instead of allocating again") {
    Arena arena(1024);
    for (int i = 0; i < 50; ++i) {
        (void)arena.allocate(100, 8);
    }
    const std::size_t reserved = arena.bytes_reserved();
    const std::size_t blocks = arena.block_count();
    CHECK(arena.bytes_used() == 5000);

    arena.reset();
    CHECK(arena.bytes_used() == 0);
    for (int i = 0; i < 50; ++i) {
        (void)arena.allocate(100, 8);
    }
    CHECK(arena.bytes_reserved() == reserved);
    CHECK(arena.block_count() == blocks);
}

TEST_CASE("arena: zero-sized requests return distinct addresses") {
    Arena arena;
    void* a = arena.allocate(0, 1);
    void* b = arena.allocate(0, 1);
    CHECK(a != b);
}
