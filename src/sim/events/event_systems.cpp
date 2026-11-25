#include "pat/sim/events/event_systems.hpp"

#include "pat/sim/simulation.hpp"

#include <utility>

namespace pat::sim {

namespace {

class EventsDispatchSystem final : public System {
public:
    explicit EventsDispatchSystem(std::shared_ptr<events::Dispatcher> dispatcher)
        : dispatcher_(std::move(dispatcher)) {}

    [[nodiscard]] std::string_view name() const override { return "events.dispatch"; }
    [[nodiscard]] Cadence cadence() const override { return Cadence::Daily; }
    void run(World& world, TickContext& context) override {
        dispatcher_->run(world, context.applied_commands, &context.budget);
    }
    void on_state_restored(const World& world) override { dispatcher_->rebaseline(world); }

private:
    std::shared_ptr<events::Dispatcher> dispatcher_;
};

}  // namespace

std::unique_ptr<System> make_events_dispatch_system(std::shared_ptr<events::Dispatcher> dispatcher) {
    return std::make_unique<EventsDispatchSystem>(std::move(dispatcher));
}

std::shared_ptr<events::Dispatcher> install_event_systems(Simulation& simulation,
                                                          std::shared_ptr<const events::EventSet> set,
                                                          events::Catalog catalog,
                                                          events::DispatchConfig config) {
    auto dispatcher =
        std::make_shared<events::Dispatcher>(std::move(set), std::move(catalog), std::move(config));
    dispatcher->rebaseline(simulation.world());
    simulation.add_system(make_events_dispatch_system(dispatcher));
    return dispatcher;
}

}  // namespace pat::sim
