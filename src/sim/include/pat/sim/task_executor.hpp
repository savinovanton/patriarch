#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>

namespace pat::sim {

/// Runs data-parallel loops for systems. The interface is the seam for a future thread pool
/// (E2-T03); results must not depend on how chunks are scheduled, so a system may only write to
/// disjoint parts of the state inside the body and must merge any shared results deterministically.
class TaskExecutor {
public:
    virtual ~TaskExecutor() = default;

    /// Calls body(begin, end) for consecutive chunks covering [0, count), each at most `grain`
    /// elements long (grain 0 is treated as 1). Returns when all chunks are done.
    virtual void parallel_for(std::size_t count,
                              std::size_t grain,
                              const std::function<void(std::size_t begin, std::size_t end)>& body) = 0;

    /// Number of chunks that may run at the same time.
    [[nodiscard]] virtual std::size_t concurrency() const noexcept = 0;
};

/// Executes chunks one after another on the calling thread, in ascending order.
class SerialExecutor final : public TaskExecutor {
public:
    void parallel_for(std::size_t count,
                      std::size_t grain,
                      const std::function<void(std::size_t, std::size_t)>& body) override {
        const std::size_t step = std::max<std::size_t>(grain, 1);
        for (std::size_t begin = 0; begin < count; begin += step) {
            body(begin, std::min(begin + step, count));
        }
    }

    [[nodiscard]] std::size_t concurrency() const noexcept override { return 1; }
};

}  // namespace pat::sim
