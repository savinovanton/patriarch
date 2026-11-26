#include "pat/sim/events/state_effects.hpp"

#include <vector>

// The table of the operations of E11-T11 (docs/dev/event-engine.md section 8.3). The commit is in
// state_effects.cpp; the Russian templates of the tooltips are in data/loc/ru/events/tooltip_effects_state.pd
// (tests/event_state_logic_test.cpp checks that every entry has one). The order of the arguments is the order
// in which apply_state_op() reads them.

namespace pat::sim::events {

namespace {

constexpr ScopeMask kAny = 0;
constexpr ScopeMask kHouse = scope_mask({ScopeKind::Character, ScopeKind::Dynasty, ScopeKind::Province});

/// `days`, `months`, `years`: optional whole numbers, summed into days (months and years in the lengths of
/// the settings, as `cooldown` and `mean_time_to_happen`).
void add_duration(std::vector<ArgSpec>& args) {
    args.push_back(arg_number("days", false));
    args.push_back(arg_number("months", false));
    args.push_back(arg_number("years", false));
}

std::vector<ArgSpec> named_with_duration(ArgSpec name) {
    std::vector<ArgSpec> args{std::move(name)};
    add_duration(args);
    return args;
}

}  // namespace

void register_state_effects(Catalog& c) {
    // ---- chains of events ----
    {
        std::vector<ArgSpec> args{arg_name("id", "event")};
        add_duration(args);
        args.push_back(arg_scope("scope", ScopeKind::None, false));
        args.push_back(arg_bool("force"));
        args.push_back(arg_bool("respect_cooldown"));
        add_state_op(c,
                     kStTriggerEvent,
                     "trigger_event",
                     kAny,
                     "trigger_event = { id = estate.audit_result  months = 3 }",
                     std::move(args));
    }
    add_state_op(c, kStStartChain, "start_chain", kAny, "start_chain = theft_story", {arg_name("name")});
    add_state_op(c,
                 kStSetChainStep,
                 "set_chain_step",
                 kAny,
                 "set_chain_step = { name = theft_story  step = 2 }",
                 {arg_name("name"), arg_number("step")});
    add_state_op(c, kStEndChain, "end_chain", kAny, "end_chain = theft_story", {arg_name("name")});

    // ---- flags, variables, modifiers ----
    add_state_op(c,
                 kStSetFlag,
                 "set_flag",
                 kAny,
                 "set_flag = { name = audit_pending  months = 6 }",
                 named_with_duration(arg_name("name")));
    add_state_op(c, kStRemoveFlag, "remove_flag", kAny, "remove_flag = audit_pending", {arg_name("name")});
    add_state_op(c,
                 kStSetVariable,
                 "set_variable",
                 kAny,
                 "set_variable = { name = debt_count  value = 0 }",
                 {arg_name("name"), arg_number("value")});
    add_state_op(c,
                 kStChangeVariable,
                 "change_variable",
                 kAny,
                 "change_variable = { name = debt_count  add = 1 }",
                 {arg_name("name"),
                  arg_number("add", false),
                  arg_number("multiply", false, core::Fixed64::one()),
                  arg_number("min", false, core::Fixed64::max()),
                  arg_number("max", false, core::Fixed64::min())});
    add_state_op(c,
                 kStAddModifier,
                 "add_modifier",
                 kAny,
                 "add_modifier = { name = estate.ignored_theft  months = 12 }",
                 named_with_duration(arg_name("name", "event_modifier")));
    add_state_op(c,
                 kStRemoveModifier,
                 "remove_modifier",
                 kAny,
                 "remove_modifier = estate.ignored_theft",
                 {arg_name("name", "event_modifier")});
    add_state_op(
        c, kStSetGlobalFlag, "set_global_flag", kAny, "set_global_flag = poltava_won", {arg_name("name")});
    add_state_op(c,
                 kStRemoveGlobalFlag,
                 "remove_global_flag",
                 kAny,
                 "remove_global_flag = poltava_won",
                 {arg_name("name")});

    // ---- the journal of notifications ----
    add_state_op(c,
                 kStNotify,
                 "notify",
                 kHouse,
                 "notify = { key = harvest_failed  a = root  urgency = important }",
                 {arg_name("key"),
                  arg_scope("a", ScopeKind::None, false),
                  arg_scope("b", ScopeKind::None, false),
                  arg_enum("urgency", {"critical", "important", "info"}, false, 2)});  // events::Urgency
}

}  // namespace pat::sim::events
