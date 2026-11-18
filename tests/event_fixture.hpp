#pragma once

// Shared fixture of the event compiler tests (E11-T06): a small catalog of triggers, values, effects
// and state operations (the real ones arrive with E11-T07 .. T11), and a host for the machine that
// reads a tiny hand-made world. Codes follow docs/dev/event-engine.md section 8.

#include "pat/core/rng.hpp"
#include "pat/sim/events/catalog.hpp"
#include "pat/sim/events/vm.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace pat::event_fixture {

namespace ev = pat::sim::events;

// predicates
inline constexpr std::uint16_t kIsAdult = 1001;
inline constexpr std::uint16_t kHasTrait = 1009;
inline constexpr std::uint16_t kIsAlive = 1012;
inline constexpr std::uint16_t kHasFlag = 1050;
inline constexpr std::uint16_t kHasModifier = 1051;
// values
inline constexpr std::uint16_t kAge = 1021;
inline constexpr std::uint16_t kSkill = 1022;
inline constexpr std::uint16_t kOpinionOf = 1026;
inline constexpr std::uint16_t kHonor = 1031;
inline constexpr std::uint16_t kCorruption = 1041;
inline constexpr std::uint16_t kControl = 1042;
inline constexpr std::uint16_t kDate = 1090;
// effects
inline constexpr std::uint16_t kChangeHonor = 2001;
inline constexpr std::uint16_t kChangeOpinion = 2004;
inline constexpr std::uint16_t kAddTrait = 2005;
inline constexpr std::uint16_t kDie = 2007;
inline constexpr std::uint16_t kPay = 2101;
inline constexpr std::uint16_t kChangeControl = 2105;
// state operations
inline constexpr std::uint16_t kSetFlag = 2201;
inline constexpr std::uint16_t kAddModifier = 2205;
inline constexpr std::uint16_t kTriggerEvent = 2210;

inline ev::CatalogEntry entry(ev::NameSpace space,
                              std::uint16_t code,
                              std::string name,
                              ev::ScopeMask from,
                              std::vector<ev::ArgSpec> args = {}) {
    ev::CatalogEntry e;
    e.space = space;
    e.code = code;
    e.name = std::move(name);
    e.from = from;
    e.args = std::move(args);
    e.result = space == ev::NameSpace::Predicate ? ev::ValueType::Bool : ev::ValueType::Number;
    return e;
}

inline ev::Catalog test_catalog() {
    using ev::ArgKind;
    using ev::NameSpace;
    using ev::ScopeKind;
    ev::Catalog c = ev::Catalog::engine();
    const ev::ScopeMask character = ev::scope_bit(ScopeKind::Character);
    const ev::ScopeMask dynasty = ev::scope_bit(ScopeKind::Dynasty);
    const ev::ScopeMask province = ev::scope_bit(ScopeKind::Province);

    c.add(entry(NameSpace::Predicate, kIsAdult, "is_adult", character));
    c.add(entry(NameSpace::Predicate, kHasTrait, "has_trait", character, {ev::arg_ref("trait", "trait")}));
    c.add(entry(NameSpace::Predicate, kIsAlive, "is_alive", character));
    c.add(entry(NameSpace::Predicate, kHasFlag, "has_flag", 0, {ev::arg_name("name")}));
    c.add(entry(
        NameSpace::Predicate, kHasModifier, "has_modifier", 0, {ev::arg_name("name", "event_modifier")}));

    c.add(entry(NameSpace::Value, kAge, "age", character));
    {
        ev::CatalogEntry skill = entry(NameSpace::Value, kSkill, "skill", character);
        skill.suffixes = {"stewardship", "diplomacy", "martial", "intrigue", "learning"};
        c.add(std::move(skill));
    }
    c.add(entry(NameSpace::Value,
                kOpinionOf,
                "opinion_of",
                character,
                {ev::arg_scope("target", ScopeKind::Character)}));
    c.add(entry(NameSpace::Value, kHonor, "honor", dynasty));
    c.add(entry(NameSpace::Value, kCorruption, "corruption", province));
    c.add(entry(NameSpace::Value, kControl, "control", province));
    c.add(entry(NameSpace::Value, kDate, "date", 0));
    {
        ev::CatalogEntry retired = entry(NameSpace::Value, 1099, "prestige_old", dynasty);
        retired.retired = true;
        retired.retired_hint = "use honor";
        c.add(std::move(retired));
    }

    c.add(entry(NameSpace::Effect, kChangeHonor, "change_honor", dynasty, {ev::arg_number("value")}));
    c.add(entry(NameSpace::Effect,
                kChangeOpinion,
                "change_opinion",
                character,
                {ev::arg_scope("target", ScopeKind::Character), ev::arg_number("value")}));
    c.add(entry(NameSpace::Effect, kAddTrait, "add_trait", character, {ev::arg_ref("trait", "trait")}));
    c.add(entry(NameSpace::Effect,
                kDie,
                "die",
                character,
                {ev::arg_enum("cause", {"natural", "murder", "battle"}, false)}));
    c.add(entry(NameSpace::Effect, kPay, "pay", dynasty, {ev::arg_number("amount")}));
    c.add(entry(NameSpace::Effect, kChangeControl, "change_control", province, {ev::arg_number("value")}));

    c.add(entry(
        NameSpace::StateOp, kSetFlag, "set_flag", 0, {ev::arg_name("name"), ev::arg_number("days", false)}));
    c.add(entry(NameSpace::StateOp,
                kAddModifier,
                "add_modifier",
                0,
                {ev::arg_name("name", "event_modifier"), ev::arg_number("months", false)}));
    c.add(entry(NameSpace::StateOp,
                kTriggerEvent,
                "trigger_event",
                0,
                {ev::arg_name("id", "event"), ev::arg_number("months", false)}));
    return c;
}

/// A host over a tiny hand-made world: set the maps, run programs.
class EventTestHost final : public ev::VmHost {
public:
    explicit EventTestHost(std::uint64_t seed = 1) : rng_(seed, 7) {}

    ev::Scope transition(ev::Scope from, std::uint16_t code) override {
        const auto it = transitions.find({from.word(), code});
        return it == transitions.end() ? ev::Scope{} : it->second;
    }
    ev::Word load(ev::Scope scope, std::uint16_t code, std::span<const ev::Word> args) override {
        last_args.assign(args.begin(), args.end());
        const auto it = values.find({scope.word(), code});
        return it == values.end() ? 0 : it->second;
    }
    ev::Word load_var(ev::Scope scope, bool global, std::uint64_t key) override {
        const auto it = vars.find({global ? -1 : scope.word(), key});
        return it == vars.end() ? 0 : it->second;
    }
    bool predicate(ev::Scope scope, std::uint16_t code, std::span<const ev::Word> args) override {
        last_args.assign(args.begin(), args.end());
        const auto it = preds.find({scope.word(), code});
        return it != preds.end() && it->second;
    }
    bool
    list(ev::Scope scope, std::uint16_t code, std::uint32_t limit, std::vector<ev::Scope>& out) override {
        const auto it = lists.find({scope.word(), code});
        if (it == lists.end()) {
            return false;
        }
        const std::size_t n = std::min<std::size_t>(limit, it->second.size());
        out.assign(it->second.begin(), it->second.begin() + static_cast<std::ptrdiff_t>(n));
        return it->second.size() > limit;
    }
    core::Pcg32& rng() override { return rng_; }

    std::map<std::pair<ev::Word, std::uint16_t>, ev::Scope> transitions;
    std::map<std::pair<ev::Word, std::uint16_t>, ev::Word> values;
    std::map<std::pair<ev::Word, std::uint16_t>, bool> preds;
    std::map<std::pair<ev::Word, std::uint16_t>, std::vector<ev::Scope>> lists;
    std::map<std::pair<ev::Word, std::uint64_t>, ev::Word> vars;
    std::vector<ev::Word> last_args;

private:
    core::Pcg32 rng_;
};

inline ev::Scope character(std::uint32_t id) {
    return ev::Scope::make(ev::ScopeKind::Character, id);
}
inline ev::Scope dynasty(std::uint32_t id) {
    return ev::Scope::make(ev::ScopeKind::Dynasty, id);
}
inline ev::Scope province(std::uint32_t id) {
    return ev::Scope::make(ev::ScopeKind::Province, id);
}
inline ev::Word whole(std::int32_t v) {
    return ev::word_from_int(v);
}

}  // namespace pat::event_fixture
