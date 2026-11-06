#pragma once

#include <cstdint>

namespace pat::sim {

/// Per-tick allowance of abstract "work units" (WU) - a deterministic stand-in for CPU time
/// (ADR-0002 P6). Systems estimate their cost in WU (entities visited, candidates scored, ...) and
/// charge it here, so decisions such as "stop planning for this tick" never depend on wall-clock
/// time or machine speed.
///
/// The simulation loop does not enforce the quota (E1 only accounts for it); budget-aware systems
/// such as the AI use try_spend() to decide how much work to do.
class WorkBudget {
public:
    void reset(std::uint64_t quota) noexcept {
        quota_ = quota;
        used_ = 0;
    }

    /// Charges `wu` if it fits into the remaining allowance. Returns false (and charges nothing) otherwise.
    [[nodiscard]] bool try_spend(std::uint64_t wu) noexcept {
        if (wu > remaining()) {
            return false;
        }
        used_ += wu;
        return true;
    }

    /// Charges `wu` unconditionally; the quota may be exceeded (visible through over_budget()).
    void spend(std::uint64_t wu) noexcept { used_ += wu; }

    [[nodiscard]] std::uint64_t quota() const noexcept { return quota_; }
    [[nodiscard]] std::uint64_t used() const noexcept { return used_; }
    [[nodiscard]] std::uint64_t remaining() const noexcept { return used_ >= quota_ ? 0 : quota_ - used_; }
    [[nodiscard]] bool over_budget() const noexcept { return used_ > quota_; }

private:
    std::uint64_t quota_ = 0;
    std::uint64_t used_ = 0;
};

}  // namespace pat::sim
