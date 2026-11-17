#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

// Thread helpers of the map pipeline (PMD2 tiles, map generator). Private to src/mapdata: the
// simulation core never spawns threads outside its scheduler, these are for offline tools and loading.

namespace pat::mapdata::parallel {

/// `requested` workers, or the hardware concurrency (at least 1) when `requested` is 0.
inline unsigned resolve_threads(unsigned requested) {
    if (requested != 0) {
        return requested;
    }
    return std::max(1U, std::thread::hardware_concurrency());
}

/// Runs fn(i) for i in [0, count) on up to `threads` workers (i % workers == worker). The work items
/// must write to disjoint memory; with one worker (or one item) everything runs on the caller.
template <class Fn>
void parallel_for(std::size_t count, unsigned threads, Fn&& fn) {
    const std::size_t workers = std::min<std::size_t>(resolve_threads(threads), count);
    if (workers <= 1) {
        for (std::size_t i = 0; i < count; ++i) {
            fn(i);
        }
        return;
    }
    const auto run = [&](std::size_t first) {
        for (std::size_t i = first; i < count; i += workers) {
            fn(i);
        }
    };
    std::vector<std::thread> pool;
    pool.reserve(workers - 1);
    for (std::size_t w = 1; w < workers; ++w) {
        pool.emplace_back(run, w);
    }
    run(0);
    for (std::thread& t : pool) {
        t.join();
    }
}

/// Runs fn(row_begin, row_end) over contiguous row ranges on `threads` workers (the ranges partition
/// [0, rows)). `threads` is used as given (at least 1, at most `rows`): pass resolve_threads() first.
template <class Fn>
void parallel_rows(std::uint32_t rows, unsigned threads, Fn&& fn) {
    threads = std::max(1U, std::min<unsigned>(threads, rows));
    if (threads == 1) {
        fn(0U, rows);
        return;
    }
    std::vector<std::thread> pool;
    pool.reserve(threads);
    for (unsigned t = 0; t < threads; ++t) {
        const std::uint32_t begin =
            static_cast<std::uint32_t>(static_cast<std::uint64_t>(rows) * t / threads);
        const std::uint32_t end =
            static_cast<std::uint32_t>(static_cast<std::uint64_t>(rows) * (t + 1) / threads);
        pool.emplace_back([&fn, begin, end] { fn(begin, end); });
    }
    for (std::thread& th : pool) {
        th.join();
    }
}

}  // namespace pat::mapdata::parallel
