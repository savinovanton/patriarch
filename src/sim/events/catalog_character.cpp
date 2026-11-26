#include "pat/sim/events/character_triggers.hpp"

#include <string>
#include <utility>
#include <vector>

// The table of the triggers and values of E11-T07 (docs/dev/event-engine.md section 8.2). The
// evaluators are in character_triggers.cpp; the Russian templates of the tooltips are in
// data/loc/ru/events/tooltip_character.pd (tests/event_triggers_character_test.cpp checks that every
// entry has them).

namespace pat::sim::events {

namespace {

constexpr ScopeMask kC = scope_bit(ScopeKind::Character);
constexpr ScopeMask kD = scope_bit(ScopeKind::Dynasty);

}  // namespace

void register_character_triggers(Catalog& c) {
    // ---- predicates over a character ----
    add_predicate(c, kTrigIsAdult, "is_adult", kC, "is_adult = yes");
    add_predicate(c, kTrigIsFemale, "is_female", kC, "is_female = no");
    add_predicate(c, kTrigIsMarried, "is_married", kC, "is_married = yes");
    add_predicate(c, kTrigIsPregnant, "is_pregnant", kC, "is_pregnant = yes");
    add_predicate(c, kTrigIsHeadOfDynasty, "is_head_of_dynasty", kC, "is_head_of_dynasty = yes");
    add_predicate(c, kTrigIsHeir, "is_heir", kC, "is_heir = yes");
    add_predicate(c, kTrigIsTonsured, "is_tonsured", kC, "is_tonsured = no");
    add_predicate(c, kTrigIsWidowed, "is_widowed", kC, "is_widowed = yes");
    add_predicate(c, kTrigHasTrait, "has_trait", kC, "has_trait = greedy", {arg_ref("trait", "trait")});
    add_predicate(c,
                  kTrigHasEducation,
                  "has_education",
                  kC,
                  "has_education = martial",
                  {arg_enum("focus", {"stewardship", "diplomacy", "martial", "intrigue", "learning"})});
    add_predicate(
        c, kTrigSameDynasty, "same_dynasty", kC | kD, "same_dynasty = scope:rival", {arg_scope("target")});
    add_predicate(c, kTrigIsAlive, "is_alive", kC, "is_alive = yes");
    add_predicate(c, kTrigHasLivingChild, "has_living_child", kC, "has_living_child = yes");
    add_predicate(c, kTrigIsRegent, "is_regent", kC, "is_regent = yes");
    add_predicate(c, kTrigIsBetrothed, "is_betrothed", kC, "is_betrothed = yes");
    add_predicate(c, kTrigIsDisowned, "is_disowned", kC, "is_disowned = no");
    // The block 1200..1249 belongs to E10; the evaluator arrives with E10-T06 (court-model.md section 10.2).
    add_predicate(
        c, kTrigHasOffice, "has_office", kC, "has_office = yes", {arg_ref("type", "office", false)}, "E10");

    // ---- values of a character ----
    add_value(c, kValAge, "age", kC, "age >= 16");
    add_value(c,
              kValSkill,
              "skill",
              kC,
              "skill.martial >= 8",
              {},
              {"stewardship", "diplomacy", "martial", "intrigue", "learning"});
    add_value(c, kValHealth, "health", kC, "health < 30");
    add_value(c, kValStress, "stress", kC, "stress >= 60");
    add_value(c, kValPiety, "piety", kC, "piety >= 50");
    add_value(c,
              kValOpinionOf,
              "opinion_of",
              kC,
              "opinion_of = { target = scope:governor  value >= 10 }",
              {arg_scope("target", ScopeKind::Character)});
    add_value(c,
              kValKinshipDegree,
              "kinship_degree",
              kC,
              "kinship_degree = { target = scope:rival  value <= 3 }",
              {arg_scope("target", ScopeKind::Character)});
    add_value(c, kValFertility, "fertility", kC, "fertility >= 40");
    add_value(c, kValAmbition, "ambition", kC, "ambition >= 30");
    add_value(c, kValLoyaltyHead, "loyalty_head", kC, "loyalty_head < 0");
    add_value(c, kValChildCount, "child_count", kC, "child_count >= 3");

    // ---- dynasty ----
    add_value(c, kValHonor, "honor", kD, "honor >= 30");
    add_value(c, kValUnity, "unity", kD, "unity < 40");
    add_value(c, kValPrestige, "prestige", kD, "prestige >= 50");
    add_value(c, kValTreasury, "treasury", kD, "treasury >= 500");
    add_predicate(c,
                  kTrigHasSuccessionLaw,
                  "has_succession_law",
                  kD,
                  "has_succession_law = majorat",
                  {arg_enum("law", {"majorat", "minorat", "partition", "family_council", "appointment"})});
}

}  // namespace pat::sim::events
