#pragma once

#include <cstddef>

// Reconciliation of the event state of a world with a set of events (E11-T03, docs/dev/event-engine.md
// section 9.4). A save names events by EventKey only; after a load made with another set of events (the
// developer mode, a hot reload) some keys may no longer exist. reconcile() removes such rows
// deterministically and records them in the history ring; it never fails and never crashes on a key it
// does not know.

namespace pat::sim {
class World;
}

namespace pat::sim::events {

class EventSet;

struct ReconcileResult {
    std::size_t timers = 0;     ///< timers of unknown events cancelled (history: Disabled)
    std::size_t cooldowns = 0;  ///< cooldowns of unknown events dropped
    std::size_t instances = 0;  ///< queue rows of unknown events or with an option that no longer exists
                                ///< cancelled (history: Cancelled)

    [[nodiscard]] std::size_t total() const noexcept { return timers + cooldowns + instances; }
};

/// Flags, variables, chains, modifiers, group cooldowns and the history ring are kept: their keys are
/// literals that cannot be told from "needed" ones without a set of keys. With a matching set the call
/// changes nothing (`total() == 0`).
ReconcileResult reconcile(World& world, const EventSet& set);

}  // namespace pat::sim::events
