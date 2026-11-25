#pragma once

#include "pat/sim/events/dispatch.hpp"
#include "pat/sim/system.hpp"

#include <memory>

// The systems of the event engine (E11-T05, docs/dev/event-engine.md section 12.3). They are registered by
// their own function and are not part of dynasty.v1: a simulation without install_event_systems() behaves
// and hashes exactly as before (the golden values of the demo, dynasty.v1 and the economy do not move).

namespace pat::sim {

class Simulation;

/// `events.dispatch`: a daily system that runs one pass of the Dispatcher over the commands of the tick.
/// The dispatcher stays reachable for the tools and the tests (its launches, its counters).
[[nodiscard]] std::unique_ptr<System> make_events_dispatch_system(std::shared_ptr<events::Dispatcher> dispatcher);

/// Registers `events.dispatch` after every system already registered (it must be the last one: it reads
/// what the others did on this tick) and baselines the dispatcher on the world of `simulation`. Call before
/// the first tick. Returns the dispatcher.
std::shared_ptr<events::Dispatcher> install_event_systems(Simulation& simulation,
                                                          std::shared_ptr<const events::EventSet> set,
                                                          events::Catalog catalog,
                                                          events::DispatchConfig config = {});

}  // namespace pat::sim
