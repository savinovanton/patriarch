#include "pat/sim/simulation.hpp"

#include "pat/core/assert.hpp"
#include "pat/core/profile.hpp"
#include "pat/sim/succession.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <utility>

namespace pat::sim {

Simulation::Simulation(World world, SimulationConfig config)
    : world_(std::move(world)), config_(config), executor_(std::make_unique<SerialExecutor>()),
      external_(std::make_unique<core::SpscQueue<Command>>(config.external_queue_capacity)),
      scratch_(config.scratch_block_bytes) {}

void Simulation::add_system(std::unique_ptr<System> system) {
    PAT_ASSERT(system != nullptr, "add_system(nullptr)");
    PAT_ASSERT(tick_count() == 0, "systems must be registered before the first tick");
    for (const auto& existing : systems_) {
        PAT_ASSERT(existing->name() != system->name(), "duplicate system name: {}", system->name());
    }
    systems_.push_back(std::move(system));
    disable_telemetry();  // Registration changes the observer's metric layout, never game state.
}

core::Result<void> Simulation::enable_telemetry(std::size_t window) {
    using R = core::Result<void>;
    if (telemetry_ && telemetry_->active())
        return R::err({1, "enable telemetry between ticks"});
    auto created = core::Telemetry::create(window, std::max<std::size_t>(64, systems_.size() + 7));
    if (!created)
        return R::err(created.error());
    auto next = std::move(created).value();
    TelemetryMetrics ids;
    std::array<core::MetricId*, 7> targets{
        &ids.tick, &ids.weekly, &ids.monthly, &ids.quarterly, &ids.yearly, &ids.internal, &ids.external};
    constexpr std::array<std::string_view, 7> names{"sim.tick",
                                                    "sim.tick.weekly",
                                                    "sim.tick.monthly",
                                                    "sim.tick.quarterly",
                                                    "sim.tick.yearly",
                                                    "sim.commands.internal",
                                                    "sim.commands.external"};
    for (std::size_t i = 0; i < names.size(); ++i) {
        auto registered = next->register_metric(names[i]);
        if (!registered)
            return R::err(registered.error());
        *targets[i] = registered.value();
    }
    std::vector<core::MetricId> system_ids;
    for (const auto& system : systems_) {
        auto registered = next->register_metric("system." + std::string(system->name()));
        if (!registered)
            return R::err(registered.error());
        system_ids.push_back(registered.value());
    }
    telemetry_ = std::move(next);
    telemetry_metrics_ = ids;
    system_metrics_ = std::move(system_ids);
    return R::ok();
}

void Simulation::disable_telemetry() {
    if (telemetry_ && telemetry_->active())
        return;
    telemetry_.reset();
    system_metrics_.clear();
}

std::shared_ptr<const core::TelemetrySnapshot> Simulation::telemetry_snapshot() const {
    return telemetry_ ? std::make_shared<const core::TelemetrySnapshot>(telemetry_->snapshot()) : nullptr;
}

void Simulation::set_executor(std::unique_ptr<TaskExecutor> executor) {
    PAT_ASSERT(executor != nullptr, "set_executor(nullptr)");
    executor_ = std::move(executor);
}

void Simulation::restore_state(World world, std::vector<Command> pending_internal) {
    world_ = std::move(world);
    // A loaded or copied world has exactly-sized columns; leave room so the first births after a
    // load do not reallocate every character column (E2 benchmark sim.telemetry_tick, #217).
    const std::size_t characters = world_.characters().ids.capacity();
    world_.reserve_character_headroom(characters / 8 + 64);
    pending_internal_ = std::move(pending_internal);
    log_.clear();
    report_ = TickReport{};
    if (telemetry_)
        telemetry_->clear();
}

const TickReport& Simulation::tick() {
    PAT_PROFILE_SCOPE("sim.tick");
    core::TelemetryFrame telemetry_frame(telemetry_.get(), tick_count() + 1);
    core::ScopedTimer tick_timer(telemetry_.get(), telemetry_metrics_.tick);

    // 1. calendar
    report_.events = world_.advance_day();
    report_.date = world_.today();
    report_.tick_index = world_.calendar().tick_count();
    if (report_.events.new_week)
        tick_timer.include(telemetry_metrics_.weekly);
    if (report_.events.new_month)
        tick_timer.include(telemetry_metrics_.monthly);
    if (report_.events.new_quarter)
        tick_timer.include(telemetry_metrics_.quarterly);
    if (report_.events.new_year)
        tick_timer.include(telemetry_metrics_.yearly);

    // Fresh per-tick resources.
    scratch_.reset();
    budget_.reset(config_.wu_per_tick);
    std::vector<DepartedHead> departed_heads;
    auto* vacancies = world_.dynasty_mode() ? &departed_heads : nullptr;

    // 2. commands emitted by systems during the previous tick
    {
        PAT_SCOPED_TIMER(telemetry_.get(), telemetry_metrics_.internal, "sim.commands.internal");
        batch_ = std::move(pending_internal_);
        pending_internal_.clear();
        report_.internal_commands =
            process_commands(world_, batch_, CommandSource::Internal, log_, vacancies, config_.traits.get());
    }

    // 3. commands submitted from outside (bounded by what is queued right now, so a fast producer
    //    cannot stall the tick)
    {
        PAT_SCOPED_TIMER(telemetry_.get(), telemetry_metrics_.external, "sim.commands.external");
        batch_.clear();
        std::size_t remaining = external_->size_approx();
        Command command;
        while (remaining > 0 && external_->try_pop(command)) {
            batch_.push_back(command);
            --remaining;
        }
        report_.external_commands =
            process_commands(world_, batch_, CommandSource::External, log_, vacancies, config_.traits.get());
    }

    // Resolve against the final membership, after deaths and explicit appointments in both batches.
    for (const auto& [dynasty, predecessor] : departed_heads) {
        if (world_.dynasties().head[dynasty.index()].valid()) {
            continue;
        }
        const CharacterId successor = fallback_successor(world_, dynasty, predecessor);
        if (successor.valid()) {
            const Command appointment{{}, cmd::SetDynastyHead{dynasty, successor}};
            const auto result = process_commands(
                world_, {&appointment, 1}, CommandSource::Internal, log_, nullptr, config_.traits.get());
            report_.internal_commands.applied += result.applied;
            report_.internal_commands.rejected += result.rejected;
        }
    }

    // 4. systems
    report_.systems.clear();
    TickContext context{world_.today(),
                        report_.events,
                        report_.tick_index,
                        scratch_,
                        budget_,
                        *executor_,
                        pending_internal_,
                        telemetry_.get()};
    std::size_t system_index = 0;
    for (const auto& system : systems_) {
        SystemRun run;
        run.name = system->name();
        run.cadence = system->cadence();
        if (is_due(run.cadence, report_.events)) {
            PAT_SCOPED_TIMER(telemetry_.get(),
                             telemetry_ ? system_metrics_[system_index] : core::MetricId(),
                             "sim.system");
            const std::uint64_t before = budget_.used();
            system->run(world_, context);
            run.ran = true;
            run.wu_used = budget_.used() - before;
        }
        report_.systems.push_back(run);
        ++system_index;
    }

    report_.wu_quota = budget_.quota();
    report_.wu_used = budget_.used();
    report_.over_budget = budget_.over_budget();
    PAT_PROFILE_FRAME_NAMED("sim.tick");
    return report_;
}

void Simulation::run_days(int days) {
    for (int i = 0; i < days; ++i) {
        (void)tick();
    }
}

}  // namespace pat::sim
