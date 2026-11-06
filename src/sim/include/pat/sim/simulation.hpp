#pragma once

#include "pat/core/arena.hpp"
#include "pat/core/spsc_queue.hpp"
#include "pat/sim/command.hpp"
#include "pat/sim/command_log.hpp"
#include "pat/sim/system.hpp"
#include "pat/sim/task_executor.hpp"
#include "pat/sim/work_budget.hpp"
#include "pat/sim/world.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace pat::sim {

struct SimulationConfig {
    std::uint64_t wu_per_tick = 1'000'000;
    std::size_t external_queue_capacity = 4096;
    std::size_t scratch_block_bytes = 64 * 1024;
};

struct SystemRun {
    std::string_view name;
    Cadence cadence = Cadence::Daily;
    bool ran = false;
    std::uint64_t wu_used = 0;
};

/// What happened during the last tick (reused between ticks; copy it if you need to keep it).
struct TickReport {
    Date date;
    TickEvents events;
    std::int64_t tick_index = 0;
    ApplyStats internal_commands;  // emitted by systems during the previous tick
    ApplyStats external_commands;  // submitted from outside (UI)
    std::uint64_t wu_quota = 0;
    std::uint64_t wu_used = 0;
    bool over_budget = false;
    std::vector<SystemRun> systems;  // one entry per registered system, in execution order
};

/// The simulation loop: owns the world and advances it one day per tick.
///
/// One tick:
///   1. the calendar moves to the next day;
///   2. commands emitted by systems during the previous tick are validated and applied (Internal);
///   3. commands submitted from outside are validated and applied (External);
///   4. every due system runs, in registration order.
/// Only 2 and 3 change the world outside of systems, so replaying the External entries of the
/// command log at the same dates reproduces the run exactly.
///
/// Thread rules: tick(), add_system() and all accessors belong to the simulation thread.
/// submit() may be called by ONE other thread (the UI); that is the only cross-thread entry point.
class Simulation {
public:
    explicit Simulation(World world, SimulationConfig config = {});

    // Movable so that factories can return a Simulation by value. Never move one while another
    // thread may still call submit().
    Simulation(Simulation&&) noexcept = default;
    Simulation& operator=(Simulation&&) noexcept = default;
    Simulation(const Simulation&) = delete;
    Simulation& operator=(const Simulation&) = delete;

    /// Registers a system; call before the first tick. Names must be unique.
    void add_system(std::unique_ptr<System> system);
    void set_executor(std::unique_ptr<TaskExecutor> executor);

    /// Queues a command from the producer thread. Returns false if the queue is full.
    [[nodiscard]] bool submit(const Command& command) { return external_->try_push(command); }

    /// Advances the world by one day.
    const TickReport& tick();
    /// Convenience for tests and tools: runs `days` ticks.
    void run_days(int days);

    /// Replaces the world and the not-yet-applied internal commands (loading a save). The command
    /// log starts empty again. Simulation thread only.
    void restore_state(World world, std::vector<Command> pending_internal);
    /// Commands emitted by systems that will be applied at the start of the next tick.
    [[nodiscard]] std::span<const Command> pending_internal() const noexcept { return pending_internal_; }

    [[nodiscard]] const World& world() const noexcept { return world_; }
    [[nodiscard]] std::shared_ptr<const World> snapshot() const { return world_.snapshot(); }
    [[nodiscard]] const CommandLog& command_log() const noexcept { return log_; }
    [[nodiscard]] const TickReport& last_report() const noexcept { return report_; }
    [[nodiscard]] std::int64_t tick_count() const noexcept { return world_.calendar().tick_count(); }
    [[nodiscard]] std::size_t system_count() const noexcept { return systems_.size(); }

private:
    World world_;
    SimulationConfig config_;
    std::vector<std::unique_ptr<System>> systems_;
    std::unique_ptr<TaskExecutor> executor_;
    std::unique_ptr<core::SpscQueue<Command>> external_;  // heap-allocated so Simulation stays movable
    std::vector<Command> pending_internal_;
    std::vector<Command> batch_;
    CommandLog log_;
    core::Arena scratch_;
    WorkBudget budget_;
    TickReport report_;
};

}  // namespace pat::sim
