#include "pat/core/spsc_queue.hpp"

#include <doctest/doctest.h>

#include <atomic>
#include <cstdint>
#include <thread>

using pat::core::SpscQueue;

TEST_CASE("spsc: capacity is rounded up to a power of two") {
    CHECK(SpscQueue<int>(1).capacity() == 2);
    CHECK(SpscQueue<int>(5).capacity() == 8);
    CHECK(SpscQueue<int>(8).capacity() == 8);
    CHECK(SpscQueue<int>(1000).capacity() == 1024);
}

TEST_CASE("spsc: push and pop preserve order; full and empty are reported") {
    SpscQueue<int> q(4);
    int out = -1;
    CHECK_FALSE(q.try_pop(out));

    for (int i = 0; i < 4; ++i) {
        CHECK(q.try_push(i));
    }
    CHECK_FALSE(q.try_push(99));  // full
    CHECK(q.size_approx() == 4);

    for (int i = 0; i < 4; ++i) {
        REQUIRE(q.try_pop(out));
        CHECK(out == i);
    }
    CHECK_FALSE(q.try_pop(out));
    CHECK(q.size_approx() == 0);
}

TEST_CASE("spsc: indices wrap around many times") {
    SpscQueue<std::uint64_t> q(8);
    std::uint64_t next_in = 0;
    std::uint64_t next_out = 0;
    for (int round = 0; round < 10000; ++round) {
        const int burst = 1 + round % 7;
        for (int i = 0; i < burst; ++i) {
            REQUIRE(q.try_push(next_in++));
        }
        for (int i = 0; i < burst; ++i) {
            std::uint64_t v = 0;
            REQUIRE(q.try_pop(v));
            REQUIRE(v == next_out++);
        }
    }
}

TEST_CASE("spsc: one producer thread and one consumer thread deliver everything in order") {
    constexpr std::uint64_t kCount = 300000;
    SpscQueue<std::uint64_t> q(256);

    std::atomic<bool> in_order{true};
    std::atomic<std::uint64_t> received{0};

    std::thread consumer([&] {
        std::uint64_t expected = 0;
        while (expected < kCount) {
            std::uint64_t v = 0;
            if (q.try_pop(v)) {
                if (v != expected) {
                    in_order.store(false);
                    return;
                }
                ++expected;
            } else {
                std::this_thread::yield();
            }
        }
        received.store(expected);
    });

    for (std::uint64_t i = 0; i < kCount; ++i) {
        while (!q.try_push(i)) {
            std::this_thread::yield();
        }
    }
    consumer.join();

    CHECK(in_order.load());
    CHECK(received.load() == kCount);
    CHECK(q.size_approx() == 0);
}
