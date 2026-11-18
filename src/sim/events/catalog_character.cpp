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

void predicate(Catalog& c,
               std::uint16_t code,
               std::string name,
               ScopeMask from,
               std::string example,
               std::vector<ArgSpec> args = {},
               std::string stub = {}) {
    CatalogEntry e;
    e.space = NameSpace::Predicate;
    e.stub = std::move(stub);
    e.code = code;
    e.name = std::move(name);
    e.from = from;
    e.result = ValueType::Bool;
    e.args = std::move(args);
    e.example = std::move(example);
    c.add(std::move(e));
}

void value(Catalog& c,
           std::uint16_t code,
           std::string name,
           ScopeMask from,
           std::string example,
           std::vector<ArgSpec> args = {},
           std::vector<std::string> suffixes = {}) {
    CatalogEntry e;
    e.space = NameSpace::Value;
    e.code = code;
    e.name = std::move(name);
    e.from = from;
    e.result = ValueType::Number;
    e.args = std::move(args);
    e.suffixes = std::move(suffixes);
    e.example = std::move(example);
    c.add(std::move(e));
}

}  // namespace

void register_character_triggers(Catalog& c) {
    // ---- predicates over a character ----
    predicate(c, kTrigIsAdult, "is_adult", kC, "is_adult = yes");
    predicate(c, kTrigIsFemale, "is_female", kC, "is_female = no");
    predicate(c, kTrigIsMarried, "is_married", kC, "is_married = yes");
    predicate(c, kTrigIsPregnant, "is_pregnant", kC, "is_pregnant = yes");
    predicate(c, kTrigIsHeadOfDynasty, "is_head_of_dynasty", kC, "is_head_of_dynasty = yes");
    predicate(c, kTrigIsHeir, "is_heir", kC, "is_heir = yes");
    predicate(c, kTrigIsTonsured, "is_tonsured", kC, "is_tonsured = no");
    predicate(c, kTrigIsWidowed, "is_widowed", kC, "is_widowed = yes");
    predicate(c, kTrigHasTrait, "has_trait", kC, "has_trait = greedy", {arg_ref("trait", "trait")});
    predicate(c,
              kTrigHasEducation,
              "has_education",
              kC,
              "has_education = martial",
              {arg_enum("focus", {"stewardship", "diplomacy", "martial", "intrigue", "learning"})});
    predicate(
        c, kTrigSameDynasty, "same_dynasty", kC | kD, "same_dynasty = scope:rival", {arg_scope("target")});
    predicate(c, kTrigIsAlive, "is_alive", kC, "is_alive = yes");
    predicate(c, kTrigHasLivingChild, "has_living_child", kC, "has_living_child = yes");
    predicate(c, kTrigIsRegent, "is_regent", kC, "is_regent = yes");
    predicate(c, kTrigIsBetrothed, "is_betrothed", kC, "is_betrothed = yes");
    predicate(c, kTrigIsDisowned, "is_disowned", kC, "is_disowned = no");
    // The block 1200..1249 belongs to E10; the evaluator arrives with E10-T06 (court-model.md section 10.2).
    predicate(
        c, kTrigHasOffice, "has_office", kC, "has_office = yes", {arg_ref("type", "office", false)}, "E10");

    // ---- values of a character ----
    value(c, kValAge, "age", kC, "age >= 16");
    value(c,
          kValSkill,
          "skill",
          kC,
          "skill.martial >= 8",
          {},
          {"stewardship", "diplomacy", "martial", "intrigue", "learning"});
    value(c, kValHealth, "health", kC, "health < 30");
    value(c, kValStress, "stress", kC, "stress >= 60");
    value(c, kValPiety, "piety", kC, "piety >= 50");
    value(c,
          kValOpinionOf,
          "opinion_of",
          kC,
          "opinion_of = { target = scope:governor  value >= 10 }",
          {arg_scope("target", ScopeKind::Character)});
    value(c,
          kValKinshipDegree,
          "kinship_degree",
          kC,
          "kinship_degree = { target = scope:rival  value <= 3 }",
          {arg_scope("target", ScopeKind::Character)});
    value(c, kValFertility, "fertility", kC, "fertility >= 40");
    value(c, kValAmbition, "ambition", kC, "ambition >= 30");
    value(c, kValLoyaltyHead, "loyalty_head", kC, "loyalty_head < 0");
    value(c, kValChildCount, "child_count", kC, "child_count >= 3");

    // ---- dynasty ----
    value(c, kValHonor, "honor", kD, "honor >= 30");
    value(c, kValUnity, "unity", kD, "unity < 40");
    value(c, kValPrestige, "prestige", kD, "prestige >= 50");
    value(c, kValTreasury, "treasury", kD, "treasury >= 500");
    predicate(c,
              kTrigHasSuccessionLaw,
              "has_succession_law",
              kD,
              "has_succession_law = majorat",
              {arg_enum("law", {"majorat", "minorat", "partition", "family_council", "appointment"})});
}

}  // namespace pat::sim::events
