#include "pat/sim/runner.hpp"

#include "pat/core/assert.hpp"

#include <algorithm>
#include <utility>

namespace pat::sim {

SimulationRunner::SimulationRunner(Simulation simulation, RunnerConfig config)
    : simulation_(std::move(simulation)), config_(config), snapshot_(simulation_.snapshot()),
      last_publish_(Clock::now()) {
    version_.store(1);
    ticks_done_.store(simulation_.tick_count());
    last_published_tick_ = simulation_.tick_count();
}

SimulationRunner::~SimulationRunner() {
    stop();
}

void SimulationRunner::start() {
    PAT_ASSERT(!thread_.joinable(), "SimulationRunner::start called twice");
    if (thread_.joinable()) {
        return;
    }
    running_.store(true);
    thread_ = std::thread([this] { run(); });
}

void SimulationRunner::stop() {
    if (!thread_.joinable()) {
        return;
    }
    {
        std::lock_guard lock(mutex_);
        stop_requested_.store(true);
        generation_.fetch_add(1);
    }
    wake_.notify_all();
    thread_.join();
    running_.store(false);
    progress_.notify_all();
}

void SimulationRunner::set_speed(Speed speed) {
    {
        std::lock_guard lock(mutex_);
        speed_.store(speed);
        generation_.fetch_add(1);
    }
    wake_.notify_all();
}

void SimulationRunner::step(int ticks) {
    if (ticks <= 0) {
        return;
    }
    {
        std::lock_guard lock(mutex_);
        pending_steps_.fetch_add(ticks);
        generation_.fetch_add(1);
    }
    wake_.notify_all();
}

bool SimulationRunner::wait_for_ticks(std::int64_t target, std::chrono::milliseconds timeout) const {
    std::unique_lock lock(mutex_);
    return progress_.wait_for(lock, timeout, [&] {
        return ticks_done_.load() >= target || !running_.load();
    }) && ticks_done_.load() >= target;
}

Simulation& SimulationRunner::simulation_when_stopped() {
    PAT_ASSERT(!thread_.joinable(), "the simulation thread is still running");
    return simulation_;
}

bool SimulationRunner::take_step() noexcept {
    int expected = pending_steps_.load();
    while (expected > 0) {
        if (pending_steps_.compare_exchange_weak(expected, expected - 1)) {
            return true;
        }
    }
    return false;
}

void SimulationRunner::wait_for_change(Clock::time_point deadline,
                                       bool has_deadline,
                                       std::uint64_t seen_generation) {
    std::unique_lock lock(mutex_);
    const auto changed = [&] { return stop_requested_.load() || generation_.load() != seen_generation; };
    if (has_deadline) {
        wake_.wait_until(lock, deadline, changed);
    } else {
        wake_.wait(lock, changed);
    }
}

void SimulationRunner::publish(bool force) {
    const std::int64_t tick = simulation_.tick_count();
    if (tick == last_published_tick_) {
        return;  // nothing new
    }
    const auto now = Clock::now();
    const auto min_interval =
        std::chrono::duration<double>(1.0 / std::max(config_.max_snapshots_per_second, 1.0));
    if (!force && now - last_publish_ < min_interval) {
        return;
    }
    snapshot_.store(simulation_.snapshot());
    version_.fetch_add(1);
    last_published_tick_ = tick;
    last_publish_ = now;
}

void SimulationRunner::tick_once() {
    (void)simulation_.tick();
    {
        // Updating the counter under the mutex pairs with the waiters in wait_for_ticks().
        std::lock_guard lock(mutex_);
        ticks_done_.store(simulation_.tick_count());
    }
    progress_.notify_all();
}

void SimulationRunner::run() {
    auto next_tick = Clock::now();
    std::uint64_t seen = generation_.load();

    while (!stop_requested_.load()) {
        const Speed speed = speed_.load();
        const double rate = config_.days_per_second[static_cast<std::size_t>(speed)];

        if (take_step()) {
            tick_once();
            publish(pending_steps_.load() == 0 && speed == Speed::Paused);
            continue;
        }

        if (speed == Speed::Paused) {
            publish(true);  // make sure the UI sees the final state before we sleep
            seen = generation_.load();
            if (pending_steps_.load() == 0 && !stop_requested_.load()) {
                wait_for_change({}, false, seen);
            }
            next_tick = Clock::now();
            seen = generation_.load();
            continue;
        }

        if (rate <= 0.0) {  // Unlimited
            tick_once();
            publish(false);
            continue;
        }

        const auto now = Clock::now();
        if (now < next_tick) {
            seen = generation_.load();
            wait_for_change(next_tick, true, seen);
            if (generation_.load() != seen) {
                next_tick = Clock::now();  // speed changed: restart pacing from now
            }
            continue;
        }

        tick_once();
        // Finite speeds are at most a few dozen ticks per second, so publish every tick.
        publish(rate <= config_.max_snapshots_per_second);
        const auto period =
            std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / rate));
        next_tick += period;
        if (Clock::now() - next_tick > std::chrono::milliseconds(250)) {
            next_tick =
                Clock::now();  // we fell behind (slow machine or breakpoint): do not burst to catch up
        }
    }

    publish(true);
    running_.store(false);
    progress_.notify_all();
}

}  // namespace pat::sim
