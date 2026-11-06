#include "pat/sim/simulation.hpp"

#include "pat/core/assert.hpp"
#include "pat/core/profile.hpp"

#include <utility>

namespace pat::sim {

Simulation::Simulation(World world, SimulationConfig config)
    : world_(std::move(world)), config_(config), executor_(std::make_unique<SerialExecutor>()),
      external_(config.external_queue_capacity), scratch_(config.scratch_block_bytes) {}

void Simulation::add_system(std::unique_ptr<System> system) {
    PAT_ASSERT(system != nullptr, "add_system(nullptr)");
    PAT_ASSERT(tick_count() == 0, "systems must be registered before the first tick");
    for (const auto& existing : systems_) {
        PAT_ASSERT(existing->name() != system->name(), "duplicate system name: {}", system->name());
    }
    systems_.push_back(std::move(system));
}

void Simulation::set_executor(std::unique_ptr<TaskExecutor> executor) {
    PAT_ASSERT(executor != nullptr, "set_executor(nullptr)");
    executor_ = std::move(executor);
}

const TickReport& Simulation::tick() {
    PAT_PROFILE_SCOPE("sim.tick");

    // 1. calendar
    report_.events = world_.advance_day();
    report_.date = world_.today();
    report_.tick_index = world_.calendar().tick_count();

    // Fresh per-tick resources.
    scratch_.reset();
    budget_.reset(config_.wu_per_tick);

    // 2. commands emitted by systems during the previous tick
    {
        PAT_PROFILE_SCOPE("sim.commands.internal");
        batch_ = std::move(pending_internal_);
        pending_internal_.clear();
        report_.internal_commands = process_commands(world_, batch_, CommandSource::Internal, log_);
    }

    // 3. commands submitted from outside (bounded by what is queued right now, so a fast producer
    //    cannot stall the tick)
    {
        PAT_PROFILE_SCOPE("sim.commands.external");
        batch_.clear();
        std::size_t remaining = external_.size_approx();
        Command command;
        while (remaining > 0 && external_.try_pop(command)) {
            batch_.push_back(command);
            --remaining;
        }
        report_.external_commands = process_commands(world_, batch_, CommandSource::External, log_);
    }

    // 4. systems
    report_.systems.clear();
    TickContext context{
        world_.today(), report_.events, report_.tick_index, scratch_, budget_, *executor_, pending_internal_};
    for (const auto& system : systems_) {
        SystemRun run;
        run.name = system->name();
        run.cadence = system->cadence();
        if (is_due(run.cadence, report_.events)) {
            PAT_PROFILE_SCOPE("sim.system");
            const std::uint64_t before = budget_.used();
            system->run(world_, context);
            run.ran = true;
            run.wu_used = budget_.used() - before;
        }
        report_.systems.push_back(run);
    }

    report_.wu_quota = budget_.quota();
    report_.wu_used = budget_.used();
    report_.over_budget = budget_.over_budget();
    return report_;
}

void Simulation::run_days(int days) {
    for (int i = 0; i < days; ++i) {
        (void)tick();
    }
}

}  // namespace pat::sim
