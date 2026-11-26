#include "pat/sim/events/character_triggers.hpp"

#include "pat/core/fixed.hpp"
#include "pat/sim/economy/tables.hpp"
#include "pat/sim/family/tables.hpp"
#include "pat/sim/kinship.hpp"

#include <algorithm>
#include <cstddef>

namespace pat::sim::events {

namespace {

// ---- reading helpers ----------------------------------------------------------------------------------

/// The facts of a person that exist for the living and for the archived dead.
struct Person {
    CharacterId id;
    bool alive = false;
    DynastyId dynasty;
    Date birth;
    Date death = kNoDate;
    Sex sex = Sex::Male;
};

[[nodiscard]] std::int32_t int_of(Word w) noexcept {
    return int_from_word(w);
}

[[nodiscard]] std::optional<Person> person_of(const World& world, Scope scope) {
    if (scope.kind != ScopeKind::Character) {
        return std::nullopt;
    }
    const CharacterId id{scope.id};
    if (world.is_alive(id)) {
        const CharacterTable& t = world.characters();
        const std::size_t i = id.index();
        return Person{id, true, t.dynasty[i], t.birth_date[i], kNoDate, t.sex[i]};
    }
    if (const std::optional<ArchivedPerson> dead = world.archived_person(id)) {
        return Person{id, false, dead->dynasty, dead->birth_date, dead->death_date, dead->sex};
    }
    return std::nullopt;
}

[[nodiscard]] int age_of(const World& world, const Person& p) {
    return age_years(p.birth, p.alive ? world.today() : p.death);
}

/// The first argument of a Ref / Enum entry: an index (the compiler pushes a whole number).
[[nodiscard]] std::int32_t index_arg(std::span<const Word> args, std::size_t at) {
    return at < args.size() ? int_of(args[at]) : 0;
}

[[nodiscard]] Scope scope_arg(std::span<const Word> args, std::size_t at) {
    return at < args.size() ? Scope::from_word(args[at]) : Scope{};
}

/// The effective value of an attribute (traits included), the stored column without a catalogue.
[[nodiscard]] core::Fixed32 attribute_of(const TriggerEnv& env,
                                         const Person& p,
                                         CharacterAttribute attribute,
                                         const std::vector<core::Fixed32>& stored) {
    if (env.traits != nullptr) {
        return attribute_breakdown(*env.world, *env.traits, p.id, attribute).total;
    }
    return stored[p.id.index()];
}

[[nodiscard]] bool has_status(const World& world, CharacterId id, std::uint8_t flag) {
    const family::PersonStatus* status = world.family_status(id);
    return status != nullptr && (status->flags & flag) != 0;
}

[[nodiscard]] bool is_widowed(const World& world, const Person& p) {
    if (!p.alive) {
        return false;
    }
    const CharacterTable& t = world.characters();
    const std::size_t i = p.id.index();
    if (t.spouse[i].valid()) {
        return false;
    }
    if (const family::PersonStatus* status = world.family_status(p.id);
        status != nullptr && status->widowed_on != kNoDate) {
        return true;
    }
    CharacterId last;
    for (const CharacterId ex : t.ex_spouses[i]) {
        if (ex.valid()) {
            last = ex;
        }
    }
    return last.valid() && !world.is_alive(last);
}

[[nodiscard]] bool has_living_child(const World& world, CharacterId id) {
    const std::vector<CharacterId> children = kin::children_of(world, id);
    return std::ranges::any_of(children, [&](CharacterId c) { return world.is_alive(c); });
}

[[nodiscard]] TriggerResult yes(bool v) {
    return TriggerResult{v ? Word{1} : Word{0}, true};
}

}  // namespace

std::uint16_t trait_slot_of(const TriggerEnv& env, std::int32_t index) {
    if (env.trait_slots.empty()) {
        return index > 0 && index <= 0xFFFF ? static_cast<std::uint16_t>(index) : std::uint16_t{0};
    }
    return index >= 0 && static_cast<std::size_t>(index) < env.trait_slots.size()
               ? env.trait_slots[static_cast<std::size_t>(index)]
               : std::uint16_t{0};
}

DynastyId dynasty_of_scope(const World& world, Scope scope) {
    if (scope.kind == ScopeKind::Dynasty) {
        const DynastyId d{scope.id};
        return world.exists(d) ? d : DynastyId{};
    }
    const std::optional<Person> p = person_of(world, scope);
    return p ? p->dynasty : DynastyId{};
}

std::optional<TriggerResult>
eval_character_predicate(const TriggerEnv& env, Scope scope, std::uint16_t code, std::span<const Word> args) {
    const World& world = *env.world;
    const std::optional<Person> person = person_of(world, scope);
    const CharacterTable& t = world.characters();
    const bool alive = person && person->alive;
    const std::size_t i = person ? person->id.index() : 0;

    switch (code) {
        case kTrigIsAdult:
            return yes(alive && age_of(world, *person) >= kComingOfAgeYears);
        case kTrigIsFemale:
            return yes(person && person->sex == Sex::Female);
        case kTrigIsMarried:
            return yes(alive && t.spouse[i].valid());
        case kTrigIsPregnant:
            return yes(alive && world.is_pregnant(person->id));
        case kTrigIsHeadOfDynasty:
            return yes(alive && person->dynasty.valid() &&
                       world.dynasties().head[person->dynasty.index()] == person->id);
        case kTrigIsHeir: {
            const family::SuccessionState* s = alive ? world.family_succession(person->dynasty) : nullptr;
            return yes(s != nullptr && s->designated_heir == person->id);
        }
        case kTrigIsTonsured:
            return yes(person && has_status(world, person->id, family::kPersonTonsured));
        case kTrigIsWidowed:
            return yes(person && is_widowed(world, *person));
        case kTrigHasTrait: {
            const std::uint16_t slot = trait_slot_of(env, index_arg(args, 0));
            return yes(alive && slot != 0 && std::ranges::find(t.traits[i], slot) != t.traits[i].end());
        }
        case kTrigHasEducation:
            return yes(alive && t.education_focus[i] == index_arg(args, 0) + 1);
        case kTrigSameDynasty: {
            const DynastyId mine = dynasty_of_scope(world, scope);
            const DynastyId theirs = dynasty_of_scope(world, scope_arg(args, 0));
            return yes(mine.valid() && mine == theirs);
        }
        case kTrigIsAlive:
            return yes(alive);
        case kTrigHasLivingChild:
            return yes(person && has_living_child(world, person->id));
        case kTrigIsRegent: {
            const family::Regency* r = alive ? world.family_regency(person->dynasty) : nullptr;
            return yes(r != nullptr && r->regent == person->id);
        }
        case kTrigIsBetrothed:
            return yes(alive && world.family_betrothal_of(person->id) != nullptr);
        case kTrigIsDisowned:
            return yes(person && has_status(world, person->id, family::kPersonDisowned));
        case kTrigHasSuccessionLaw: {
            const DynastyId d =
                scope.kind == ScopeKind::Dynasty ? dynasty_of_scope(world, scope) : DynastyId{};
            return yes(d.valid() &&
                       static_cast<int>(world.dynasties().succession_law[d.index()]) == index_arg(args, 0));
        }
        case kTrigHasOffice:
            return TriggerResult{0, false};  // E10: there are no offices in the World yet
        default:
            return std::nullopt;
    }
}

std::optional<TriggerResult>
eval_character_value(const TriggerEnv& env, Scope scope, std::uint16_t code, std::span<const Word> args) {
    const World& world = *env.world;
    const CharacterTable& t = world.characters();
    const std::optional<Person> person = person_of(world, scope);
    const bool alive = person && person->alive;
    const std::size_t i = person ? person->id.index() : 0;
    const DynastyId dynasty = scope.kind == ScopeKind::Dynasty ? dynasty_of_scope(world, scope) : DynastyId{};
    const auto number = [](Word w) { return TriggerResult{w, true}; };

    switch (code) {
        case kValAge:
            return number(person ? word_from_whole(age_of(world, *person)) : 0);
        case kValSkill: {
            const std::int32_t index = index_arg(args, 0);
            if (!alive || index < 0 || static_cast<std::size_t>(index) >= kSkillCount) {
                return number(0);
            }
            const auto skill = static_cast<Skill>(index);
            return number(word_from_fixed32(env.traits != nullptr
                                                ? skill_breakdown(world, *env.traits, person->id, skill).total
                                                : t.skills[i][static_cast<std::size_t>(index)]));
        }
        case kValHealth:
            return number(
                alive ? word_from_fixed32(attribute_of(env, *person, CharacterAttribute::Health, t.health))
                      : 0);
        case kValStress:
            return number(
                alive ? word_from_fixed32(attribute_of(env, *person, CharacterAttribute::Stress, t.stress))
                      : 0);
        case kValPiety:
            return number(
                alive ? word_from_fixed32(attribute_of(env, *person, CharacterAttribute::Piety, t.piety))
                      : 0);
        case kValFertility:
            return number(alive ? word_from_fixed32(
                                      attribute_of(env, *person, CharacterAttribute::Fertility, t.fertility))
                                : 0);
        case kValAmbition:
            return number(alive ? word_from_fixed32(
                                      attribute_of(env, *person, CharacterAttribute::Ambition, t.ambition))
                                : 0);
        case kValLoyaltyHead:
            return number(alive ? word_from_fixed32(attribute_of(
                                      env, *person, CharacterAttribute::LoyaltyHead, t.loyalty_head))
                                : 0);
        case kValOpinionOf: {
            const Scope target = scope_arg(args, 0);
            if (!alive || target.kind != ScopeKind::Character || !world.exists(CharacterId{target.id})) {
                return number(0);
            }
            return number(word_from_fixed32(world.get_opinion(person->id, CharacterId{target.id})));
        }
        case kValKinshipDegree: {
            const Scope target = scope_arg(args, 0);
            if (!person || target.kind != ScopeKind::Character) {
                return number(word_from_whole(kNoKinship));
            }
            const std::uint8_t degree = kin::blood_degree(world, person->id, CharacterId{target.id});
            return number(word_from_whole(degree == 0 ? kNoKinship : degree));
        }
        case kValChildCount:
            return number(
                person ? word_from_whole(static_cast<std::int64_t>(kin::child_count(world, person->id))) : 0);
        case kValHonor:
            return number(dynasty.valid() ? word_from_whole(world.dynasties().honor[dynasty.index()]) : 0);
        case kValUnity:
            return number(dynasty.valid() ? word_from_fixed32(world.dynasties().unity[dynasty.index()]) : 0);
        case kValPrestige:
            return number(dynasty.valid() ? word_from_fixed32(world.dynasties().prestige[dynasty.index()])
                                          : 0);
        case kValTreasury: {
            if (!dynasty.valid() || !world.economy_present()) {
                return TriggerResult{0, false};  // no economy in this world: no data
            }
            const economy::Kopecks kop = world.treasury(economy::AccountRef::of(dynasty));
            return number(word_from_fixed(core::Fixed64::from_ratio(kop, 100)));
        }
        default:
            return std::nullopt;
    }
}

}  // namespace pat::sim::events
