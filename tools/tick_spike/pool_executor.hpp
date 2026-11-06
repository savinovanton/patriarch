#pragma once

#include "pat/sim/task_executor.hpp"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace pat::spike {

// Experiment only: persistent workers, caller participation, synchronous batches. One caller,
// no nested batches, nonthrowing bodies. Small batches with one chunk stay on the caller.
class PoolExecutor final : public sim::TaskExecutor {
public:
    explicit PoolExecutor(std::size_t threads);
    ~PoolExecutor() override;
    PoolExecutor(const PoolExecutor&) = delete;
    PoolExecutor& operator=(const PoolExecutor&) = delete;

    void parallel_for(std::size_t count,
                      std::size_t grain,
                      const std::function<void(std::size_t, std::size_t)>& body) override;
    [[nodiscard]] std::size_t concurrency() const noexcept override { return workers_.size() + 1; }

private:
    void consume();
    void worker();

    std::vector<std::thread> workers_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::condition_variable done_;
    std::atomic<std::size_t> next_{0};
    // Keep the contended cursor away from batch metadata read by every worker.
    [[maybe_unused]] std::array<std::byte, 64> cursor_padding_{};
    const std::function<void(std::size_t, std::size_t)>* body_ = nullptr;
    std::size_t count_ = 0;
    std::size_t grain_ = 1;
    std::size_t remaining_ = 0;
    std::uint64_t generation_ = 0;
    bool stop_ = false;
};

}  // namespace pat::spike
