#include "pat/sim/events/realm_triggers.hpp"

#include <string>
#include <vector>

// The table of the triggers and values of E11-T08 (docs/dev/event-engine.md section 8.2): the state of
// events, chance, time, the province and the house in the realm. The evaluators are in realm_triggers.cpp;
// the Russian templates of the tooltips are in data/loc/ru/events/tooltip_realm.pd
// (tests/event_triggers_realm_test.cpp checks that every entry has them).

namespace pat::sim::events {

namespace {

constexpr ScopeMask kAny = 0;
constexpr ScopeMask kC = scope_bit(ScopeKind::Character);
constexpr ScopeMask kD = scope_bit(ScopeKind::Dynasty);
constexpr ScopeMask kP = scope_bit(ScopeKind::Province);

std::vector<std::string> group_names() {
    return {"nobility", "clergy", "townsfolk", "peasants", "cossacks", "natives"};
}

}  // namespace

void register_realm_triggers(Catalog& c) {
    // ---- the state of events: flags, modifiers, cooldowns, chains, variables ----
    add_predicate(c, kTrigHasFlag, "has_flag", kAny, "has_flag = hushed_theft", {arg_name("flag")});
    add_predicate(
        c, kTrigHasGlobalFlag, "has_global_flag", kAny, "has_global_flag = poltava_won", {arg_name("flag")});
    add_predicate(c,
                  kTrigHasModifier,
                  "has_modifier",
                  kAny,
                  "has_modifier = estate_audit",
                  {arg_name("modifier", "event_modifier")});
    add_predicate(c, kTrigHasFired, "has_fired", kAny, "has_fired = theft_found", {arg_name("name")});
    add_predicate(
        c, kTrigHasCooldown, "has_cooldown", kAny, "has_cooldown = theft_found", {arg_name("name")});
    add_value(c,
              kValChainStep,
              "chain_step",
              kAny,
              "chain_step = { chain = theft_story  value >= 2 }",
              {arg_name("chain")});
    add_value(c, kValVariable, "variable", kAny, "variable.debt_count >= 3", {arg_name("name")});
    add_value(
        c, kValGlobalVariable, "global_variable", kAny, "global_variable.levy_year >= 1", {arg_name("name")});

    // ---- chance and time ----
    add_predicate(
        c, kTrigRandomChance, "random_chance", kAny, "random_chance = 12.5", {arg_number("percent")});
    add_value(c, kValCurrentYear, "current_year", kAny, "current_year >= 1709");
    add_value(c, kValCurrentMonth, "current_month", kAny, "current_month <= 3");
    add_value(c, kValDate, "date", kAny, "date >= 1703.05.16");

    // ---- province ----
    add_value(c, kValUnrest, "unrest", kP, "unrest >= 40");
    add_value(c, kValControl, "control", kP, "control < 30");
    add_value(c, kValCorruption, "corruption", kP, "corruption > 0.25");
    add_value(c, kValPopulation, "population", kP, "population >= 50000");
    add_value(c, kValRecruitPool, "recruit_pool", kP, "recruit_pool >= 500");
    add_predicate(
        c, kTrigHasBuilding, "has_building", kP, "has_building = mill", {arg_ref("building", "building")});
    add_value(c, kValUnrestState, "unrest_state", kP, "unrest_state >= 2");
    add_value(c, kValDevelopment, "development", kP, "development >= 3");
    add_value(c, kValGroupSize, "group_size", kP, "group_size.peasants >= 20000", {}, group_names());
    add_value(c, kValGroupMood, "group_mood", kP, "group_mood.nobility < -20", {}, group_names());

    // ---- the house in the realm ----
    add_predicate(
        c, kTrigIsPlayerControlled, "is_player_controlled", kC | kD | kP, "is_player_controlled = no");
    add_value(c, kValDebt, "debt", kD, "debt > 1000");
    add_predicate(c,
                  kTrigHasQuota,
                  "has_quota",
                  kD,
                  "has_quota = recruits",
                  {arg_enum("kind", {"any", "money", "recruits"}, false, 0)});
}

}  // namespace pat::sim::events
