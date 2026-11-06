#include "pool_executor.hpp"

#include <algorithm>

namespace pat::spike {

PoolExecutor::PoolExecutor(std::size_t threads) {
    threads = std::max<std::size_t>(threads, 1);
    workers_.reserve(threads - 1);
    for (std::size_t i = 1; i < threads; ++i) {
        workers_.emplace_back([this] { worker(); });
    }
}

PoolExecutor::~PoolExecutor() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    ready_.notify_all();
    for (auto& thread : workers_) {
        thread.join();
    }
}

void PoolExecutor::consume() {
    for (;;) {
        const auto begin = next_.fetch_add(grain_, std::memory_order_relaxed);
        if (begin >= count_) {
            return;
        }
        (*body_)(begin, std::min(begin + grain_, count_));
    }
}

void PoolExecutor::worker() {
    std::uint64_t seen = 0;
    for (;;) {
        {
            std::unique_lock lock(mutex_);
            ready_.wait(lock, [&] { return stop_ || generation_ != seen; });
            if (stop_) {
                return;
            }
            seen = generation_;
        }
        consume();
        {
            std::lock_guard lock(mutex_);
            if (--remaining_ == 0) {
                done_.notify_one();
            }
        }
    }
}

void PoolExecutor::parallel_for(std::size_t count,
                                std::size_t grain,
                                const std::function<void(std::size_t, std::size_t)>& body) {
    grain = std::max<std::size_t>(grain, 1);
    if (workers_.empty() || count <= grain) {
        for (std::size_t begin = 0; begin < count; begin += grain) {
            body(begin, std::min(begin + grain, count));
        }
        return;
    }
    {
        std::lock_guard lock(mutex_);
        body_ = &body;
        count_ = count;
        grain_ = grain;
        next_.store(0, std::memory_order_relaxed);
        remaining_ = workers_.size();
        ++generation_;
    }
    ready_.notify_all();
    consume();
    std::unique_lock lock(mutex_);
    done_.wait(lock, [&] { return remaining_ == 0; });
    body_ = nullptr;
}

}  // namespace pat::spike
