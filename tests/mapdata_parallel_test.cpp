#include <doctest/doctest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "parallel.hpp"

using namespace pat::mapdata::parallel;

TEST_CASE("resolve_threads: explicit count wins and zero means hardware") {
    CHECK(resolve_threads(3) == 3);
    CHECK(resolve_threads(1) == 1);
    CHECK(resolve_threads(0) >= 1);
}

TEST_CASE("parallel_for: every index is visited once") {
    for (const unsigned threads : {1U, 2U, 3U, 8U}) {
        for (const std::size_t count : {std::size_t{0}, std::size_t{1}, std::size_t{7}, std::size_t{100}}) {
            std::vector<std::atomic<int>> hits(count);
            parallel_for(count, threads, [&](std::size_t i) { hits[i].fetch_add(1); });
            for (std::size_t i = 0; i < count; ++i) {
                CHECK(hits[i].load() == 1);
            }
        }
    }
}

TEST_CASE("parallel_rows: the ranges partition the row range") {
    for (const unsigned threads : {1U, 2U, 5U, 64U}) {
        for (const std::uint32_t rows : {1U, 2U, 7U, 48U}) {
            std::vector<std::atomic<int>> hits(rows);
            parallel_rows(rows, threads, [&](std::uint32_t begin, std::uint32_t end) {
                CHECK(begin < end);
                for (std::uint32_t y = begin; y < end; ++y) {
                    hits[y].fetch_add(1);
                }
            });
            for (std::uint32_t y = 0; y < rows; ++y) {
                CHECK(hits[y].load() == 1);
            }
        }
    }
}
