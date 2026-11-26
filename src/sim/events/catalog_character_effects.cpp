#include "pat/sim/events/character_effects.hpp"

#include <string>
#include <utility>
#include <vector>

// The table of the effects of E11-T09 (docs/dev/event-engine.md section 8.3). The translation into
// commands is in character_effects.cpp; the Russian templates of the tooltips are in
// data/loc/ru/events/tooltip_effects_character.pd (tests/event_effects_character_test.cpp checks that every
// entry has one). The order of the enum values is the order of the enums of the commands.

namespace pat::sim::events {

namespace {

constexpr ScopeMask kC = scope_bit(ScopeKind::Character);
constexpr ScopeMask kD = scope_bit(ScopeKind::Dynasty);

std::vector<std::string> skills() {
    return {"stewardship", "diplomacy", "martial", "intrigue", "learning"};  // Skill
}

}  // namespace

void register_character_effects(Catalog& c) {
    // ---- the standing of a house ----
    add_effect(c, kEffChangeHonor, "change_honor", kD, "change_honor = -5", {arg_number("value")});
    add_effect(c, kEffChangeUnity, "change_unity", kD, "change_unity = 2.5", {arg_number("value")});
    add_effect(c, kEffChangePrestige, "change_prestige", kD, "change_prestige = 10", {arg_number("value")});

    // ---- a person ----
    add_effect(c,
               kEffChangeOpinion,
               "change_opinion",
               kC,
               "change_opinion = { target = scope:rival  value = -20 }",
               {arg_scope("target", ScopeKind::Character), arg_number("value"), arg_number("reason", false)});
    add_effect(c, kEffAddTrait, "add_trait", kC, "add_trait = greedy", {arg_ref("trait", "trait")});
    add_effect(c, kEffRemoveTrait, "remove_trait", kC, "remove_trait = greedy", {arg_ref("trait", "trait")});
    add_effect(c,
               kEffDie,
               "die",
               kC,
               "die = illness",
               {arg_enum("cause",
                         {"old_age",
                          "illness",
                          "childbirth",
                          "war",
                          "execution",
                          "murder",
                          "accident",
                          "wounds",
                          "grief"})});  // CauseOfDeath 1...9
    add_effect(c,
               kEffPlanDeath,
               "plan_death",
               kC,
               "plan_death = { years = 2 }",
               {arg_date("date", false), arg_number("years", false, core::Fixed64::from_int(-1))});
    add_effect(c,
               kEffSetEducation,
               "set_education",
               kC,
               "set_education = { focus = martial  tutor = scope:tutor }",
               {arg_enum("focus", skills()), arg_scope("tutor", ScopeKind::Character, false)});
    add_effect(c,
               kEffConceive,
               "conceive",
               kC,
               "conceive = { father = scope:lover }",
               {arg_scope("father", ScopeKind::Character)});
    add_effect(c,
               kEffCreatePerson,
               "create_person",
               kD,
               "create_person = { sex = female  age = 16 }",
               {arg_enum("sex", {"male", "female"}),
                arg_number("age"),
                arg_scope("father", ScopeKind::Character, false),
                arg_scope("mother", ScopeKind::Character, false)});
    add_effect(c,
               kEffEndMarriage,
               "end_marriage",
               kC,
               "end_marriage = annulment",
               {arg_enum("reason", {"annulment", "tonsure"})});  // MarriageEnd
    add_effect(c,
               kEffChangeSkill,
               "change_skill",
               kC,
               "change_skill = { skill = martial  delta = 2 }",
               {arg_enum("skill", skills()), arg_number("delta")});
    add_effect(c, kEffChangeHealth, "change_health", kC, "change_health = -10", {arg_number("delta")});
    add_effect(c, kEffChangeStress, "change_stress", kC, "change_stress = 15", {arg_number("delta")});
    add_effect(c,
               kEffRecordChronicle,
               "record_chronicle",
               kD,
               "record_chronicle = { key = duel  a = root  b = scope:rival }",
               {arg_name("key"),
                arg_scope("a", ScopeKind::Character, false),
                arg_scope("b", ScopeKind::Character, false)});

    // ---- the family commands of E8 ----
    add_effect(c,
               kEffBetroth,
               "betroth",
               kD,
               "betroth = { groom = scope:groom  bride = scope:bride  dowry = 300 }",
               {arg_scope("groom", ScopeKind::Character),
                arg_scope("bride", ScopeKind::Character),
                arg_number("dowry", false),
                arg_bool("dispensation")});
    add_effect(c, kEffTonsure, "tonsure", kC, "tonsure = yes");
    add_effect(
        c, kEffSetHeir, "set_heir", kD, "set_heir = scope:son", {arg_scope("heir", ScopeKind::Character)});
    add_effect(c, kEffDisown, "disown", kC, "disown = yes");
    add_effect(c,
               kEffGrantLand,
               "grant_land",
               kD,
               "grant_land = { character = scope:son  province = scope:estate }",
               {arg_scope("character", ScopeKind::Character), arg_scope("province", ScopeKind::Province)},
               "E8");
    add_effect(c,
               kEffAssignRegent,
               "assign_regent",
               kD,
               "assign_regent = { regent = scope:uncle  kind = uncle }",
               {arg_scope("regent", ScopeKind::Character),
                arg_enum("kind", {"mother", "guardian", "uncle", "council", "imperial"})},  // RegencyKind
               "E8");
    add_effect(c,
               kEffDismissRegent,
               "dismiss_regent",
               kD,
               "dismiss_regent = dismissed",
               {arg_enum("reason", {"majority", "dismissed", "regent_died"})},
               "E8");
    add_effect(c,
               kEffResolveSuccessionDispute,
               "resolve_succession_dispute",
               kD,
               "resolve_succession_dispute = { winner = scope:claimant }",
               {arg_scope("winner", ScopeKind::Character)},
               "E8");
}

}  // namespace pat::sim::events
