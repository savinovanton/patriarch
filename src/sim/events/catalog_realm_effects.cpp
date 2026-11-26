#include "pat/sim/events/realm_effects.hpp"

#include <string>
#include <vector>

// The table of the effects of E11-T10 (docs/dev/event-engine.md section 8.3). The translation into
// commands is in realm_effects.cpp; the Russian templates of the tooltips are in
// data/loc/ru/events/tooltip_effects_realm.pd (tests/event_effects_realm_test.cpp checks that every entry
// has one). The group names are those of economy::Group, in its order.

namespace pat::sim::events {

namespace {

constexpr ScopeMask kD = scope_bit(ScopeKind::Dynasty);
constexpr ScopeMask kP = scope_bit(ScopeKind::Province);

std::vector<std::string> group_names() {
    return {"nobility", "clergy", "townsfolk", "peasants", "cossacks", "natives"};  // economy::Group
}

}  // namespace

void register_realm_effects(Catalog& c) {
    // ---- the province ----
    add_effect(c,
               kEffSetProvinceOwner,
               "set_province_owner",
               kP,
               "set_province_owner = scope:rival_house",
               {arg_scope("owner", ScopeKind::Dynasty)});
    add_effect(c,
               kEffAssignAdministrator,
               "assign_administrator",
               kP,
               "assign_administrator = scope:steward",
               {arg_scope("administrator", ScopeKind::Character, false), arg_bool("dismiss")});
    add_effect(c, kEffChangeUnrest, "change_unrest", kP, "change_unrest = 10", {arg_number("value")});
    add_effect(c, kEffChangeControl, "change_control", kP, "change_control = -5", {arg_number("value")});
    add_effect(c,
               kEffChangeGroupMood,
               "change_group_mood",
               kP,
               "change_group_mood = { group = peasants  delta = -8 }",
               {arg_enum("group", group_names()), arg_number("delta")});

    // ---- money (rubles; one transfer between the house and the crown) ----
    add_effect(c, kEffPay, "pay", kD, "pay = 120", {arg_number("value")});
    add_effect(c, kEffReceive, "receive", kD, "receive = 50.5", {arg_number("value")});

    // ---- the economy of a province (commands of E9) ----
    add_effect(c,
               kEffStartConstruction,
               "start_construction",
               kP,
               "start_construction = mill",
               {arg_ref("building", "building")});
    add_effect(c, kEffConscript, "conscript", kP, "conscript = 200", {arg_number("count")});
}

}  // namespace pat::sim::events
