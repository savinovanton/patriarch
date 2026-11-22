#include "pat/sim/events/reconcile.hpp"

#include "pat/sim/events/event_set.hpp"
#include "pat/sim/world.hpp"

#include <cstdint>
#include <vector>

namespace pat::sim::events {

namespace {

void note(World& world, EventKey key, Scope scope, HistoryOutcome outcome, std::uint8_t option) {
    HistoryEntry entry;
    entry.date = world.today();
    entry.event_key = key;
    entry.outcome = outcome;
    entry.scope = scope;
    entry.option = option;
    (void)world.append_event_history(entry);  // a full ring drops its oldest entry; a refusal is harmless
}

/// The queue row refers to a window that the loaded event cannot show any more.
bool instance_is_stale(const Instance& row, const EventDef* def) {
    if (def == nullptr) {
        return true;
    }
    const std::size_t options = def->options.size();
    if (row.option != kNoOption && row.option >= options) {
        return true;
    }
    return options < 8 && (row.avail_mask >> options) != 0;
}

}  // namespace

ReconcileResult reconcile(World& world, const EventSet& set) {
    ReconcileResult result;
    const State& state = world.event_state();

    std::vector<Timer> stale_timers;
    for (const Timer& row : state.timers) {
        if (set.find(row.event_key) == nullptr) {
            stale_timers.push_back(row);
        }
    }
    for (const Timer& row : stale_timers) {
        if (world.cancel_event_timer(row.seq)) {
            note(world, row.event_key, row.scope, HistoryOutcome::Disabled, kNoOption);
            ++result.timers;
        }
    }

    std::vector<Cooldown> stale_cooldowns;
    for (const Cooldown& row : state.cooldowns) {
        if ((row.key & kCooldownGroupBit) == 0 && set.find(row.key) == nullptr) {
            stale_cooldowns.push_back(row);
        }
    }
    for (const Cooldown& row : stale_cooldowns) {
        if (world.clear_event_cooldown(row.scope, row.key)) {
            ++result.cooldowns;
        }
    }

    std::vector<Instance> stale_instances;
    for (const Instance& row : state.queue) {
        if (instance_is_stale(row, set.find(row.event_key))) {
            stale_instances.push_back(row);
        }
    }
    for (const Instance& row : stale_instances) {
        if (world.remove_event_instance(row.instance)) {
            note(world, row.event_key, row.scope, HistoryOutcome::Cancelled, row.option);
            ++result.instances;
        }
    }
    return result;
}

}  // namespace pat::sim::events
