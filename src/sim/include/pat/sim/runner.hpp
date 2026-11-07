#pragma once

#include "pat/sim/command.hpp"
#include "pat/sim/simulation.hpp"
#include "pat/sim/world.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

namespace pat::sim {

/// Game speed. Paused stops the clock (explicit step() calls still run); Unlimited ticks as fast as
/// the machine allows (tools, benchmarks, fast-forward) - days are never skipped at any speed.
enum class Speed : std::uint8_t { Paused = 0, X1, X2, X3, X4, X5, Unlimited };

inline constexpr std::size_t kSpeedCount = 7;

struct RunnerConfig {
    /// Target days per second for each speed (Paused is ignored; 0 means "no pacing" and is used
    /// by Unlimited). NFR-PERF-2: speed 5 must reach at least 20 days/s on the target hardware.
    std::array<double, kSpeedCount> days_per_second = {0.0, 1.0, 3.0, 8.0, 20.0, 40.0, 0.0};
    /// Upper bound for published snapshots (the UI does not need more than its frame rate).
    double max_snapshots_per_second = 60.0;
};

/// Runs a Simulation on its own thread and publishes immutable snapshots for the UI (ADR-0001/0002).
///
/// Threads:
///   * the simulation thread (owned by the runner) is the only one that ticks the simulation;
///   * any single "UI" thread may call submit(), set_speed(), step(), latest_snapshot() and the
///     read-only counters; latest_snapshot() is safe from any number of threads.
/// Readers never wait for a tick: they receive the last published `shared_ptr<const World>`.
class SimulationRunner {
public:
    using Clock = std::chrono::steady_clock;

    explicit SimulationRunner(Simulation simulation, RunnerConfig config = {});
    ~SimulationRunner();

    SimulationRunner(const SimulationRunner&) = delete;
    SimulationRunner& operator=(const SimulationRunner&) = delete;

    /// Starts the simulation thread (initially Paused). Call once.
    void start();
    /// Stops the thread after the current tick and publishes a final snapshot. Idempotent.
    void stop();

    void set_speed(Speed speed);
    [[nodiscard]] Speed speed() const noexcept { return speed_.load(); }
    /// Requests `ticks` extra ticks that run regardless of the speed (useful while paused).
    void step(int ticks);

    /// Queues a command for the next tick boundary. Returns false if the command queue is full.
    [[nodiscard]] bool submit(const Command& command) { return simulation_.submit(command); }

    /// The most recently published state; never null. Cheap to call every frame.
    [[nodiscard]] std::shared_ptr<const World> latest_snapshot() const { return snapshot_.load(); }
    /// Latest detached telemetry summary, or null when disabled. Safe for overlay readers.
    [[nodiscard]] std::shared_ptr<const core::TelemetrySnapshot> latest_telemetry() const {
        return telemetry_.load();
    }
    /// Increases every time a new snapshot is published.
    [[nodiscard]] std::uint64_t snapshot_version() const noexcept { return version_.load(); }
    [[nodiscard]] std::int64_t ticks_done() const noexcept { return ticks_done_.load(); }
    [[nodiscard]] bool running() const noexcept { return running_.load(); }

    /// Blocks until `target` ticks have been executed or the timeout passes. Returns whether the
    /// target was reached.
    [[nodiscard]] bool wait_for_ticks(std::int64_t target, std::chrono::milliseconds timeout) const;

    /// Access to the simulation for inspection after stop() (the thread must not be running).
    [[nodiscard]] Simulation& simulation_when_stopped();

private:
    void run();
    bool take_step() noexcept;
    void tick_once();
    void publish(bool force);
    void wait_for_change(Clock::time_point deadline, bool has_deadline, std::uint64_t seen_generation);

    Simulation simulation_;
    RunnerConfig config_;

    std::thread thread_;
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> running_{false};
    std::atomic<Speed> speed_{Speed::Paused};
    std::atomic<int> pending_steps_{0};
    std::atomic<std::uint64_t> generation_{0};  // bumped by every control change to wake the thread

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    mutable std::condition_variable progress_;

    std::atomic<std::int64_t> ticks_done_{0};
    std::atomic<std::shared_ptr<const World>> snapshot_;
    std::atomic<std::shared_ptr<const core::TelemetrySnapshot>> telemetry_;
    std::atomic<std::uint64_t> version_{0};
    std::int64_t last_published_tick_ = -1;  // simulation thread only
    Clock::time_point last_publish_;         // simulation thread only
};

}  // namespace pat::sim
