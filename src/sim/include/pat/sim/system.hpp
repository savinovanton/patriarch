#pragma once

#include "pat/core/arena.hpp"
#include "pat/sim/calendar.hpp"
#include "pat/sim/command.hpp"
#include "pat/sim/task_executor.hpp"
#include "pat/sim/work_budget.hpp"
#include "pat/sim/world.hpp"

#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

namespace pat::core {
class Telemetry;
}

namespace pat::sim {

/// How often a system runs (ADR-0002 P2). Heavy per-entity work inside a periodic system should
/// still be spread with the stagger helpers of calendar.hpp.
enum class Cadence : std::uint8_t { Daily, Weekly, Monthly, Quarterly, Yearly };

[[nodiscard]] constexpr bool is_due(Cadence cadence, const TickEvents& events) noexcept {
    switch (cadence) {
        case Cadence::Daily:
            return true;
        case Cadence::Weekly:
            return events.new_week;
        case Cadence::Monthly:
            return events.new_month;
        case Cadence::Quarterly:
            return events.new_quarter;
        case Cadence::Yearly:
            return events.new_year;
    }
    return false;
}

/// Everything a system may use during one tick besides the world itself.
struct TickContext {
    Date date;
    TickEvents events;
    std::int64_t tick_index = 0;           // number of ticks executed so far, including this one
    core::Arena& scratch;                  // per-tick scratch memory, reset before every tick
    WorkBudget& budget;                    // abstract work accounting (see work_budget.hpp)
    TaskExecutor& executor;                // data-parallel loops
    std::vector<Command>& emitted;         // commands for the NEXT tick boundary
    core::Telemetry* telemetry = nullptr;  // Optional observer; never use timings for decisions.

    /// Queues a command (for example from AI). It is validated and applied at the start of the next
    /// tick, before any system runs, and recorded in the command log as Internal.
    void emit(Command command) { emitted.push_back(std::move(command)); }
};

/// A unit of simulation logic. Systems run in registration order every time they are due.
/// They must be deterministic: no wall-clock time, no global mutable state, randomness only via
/// world.rng() streams, and state changes preferably through commands (emit) or the World API.
class System {
public:
    virtual ~System() = default;

    [[nodiscard]] virtual std::string_view name() const = 0;
    [[nodiscard]] virtual Cadence cadence() const = 0;
    virtual void run(World& world, TickContext& context) = 0;
};

}  // namespace pat::sim
