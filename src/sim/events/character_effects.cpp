#include "pat/sim/events/character_effects.hpp"

#include "pat/core/fixed.hpp"
#include "pat/sim/conception.hpp"
#include "pat/sim/economy/money.hpp"

#include <cstddef>

namespace pat::sim::events {

namespace {

// ---- reading the arguments --------------------------------------------------------------------------

/// A reference or an enum: the whole number the compiler pushes (its index).
[[nodiscard]] std::int32_t index_arg(std::span<const Word> args, std::size_t at) {
    return int_from_word(effect_arg(args, at));
}

/// An optional person argument: an empty scope is "none" (invalid id); a scope of another kind is an error.
[[nodiscard]] std::optional<CharacterId> person_arg(std::span<const Word> args, std::size_t at) {
    const std::optional<Scope> s = effect_scope_arg(args, at, ScopeKind::Character);
    if (!s) {
        return std::nullopt;
    }
    return s->empty() ? CharacterId() : CharacterId{s->id};
}

[[nodiscard]] bool is_character(Scope s) noexcept {
    return s.kind == ScopeKind::Character;
}

/// The house of a dynasty scope (or of a person, for safety), or nullopt.
[[nodiscard]] std::optional<DynastyId> house_of(const World& world, Scope target) {
    const DynastyId d = dynasty_of_scope(world, target);
    return d.valid() ? std::optional<DynastyId>(d) : std::nullopt;
}

[[nodiscard]] const DemographyRules& rules_of(const EffectEnv& env) {
    return env.demography != nullptr ? *env.demography : kDefaultDemography;
}

// ---- the effects that need more than one line --------------------------------------------------------

EffectStatus
plan_death(const World& world, CharacterId who, std::span<const Word> args, std::vector<Command>& out) {
    // `years` wins when it is given (it defaults to -1); otherwise `date` (day 0 = absent).
    const std::int32_t years = int_from_word(effect_arg(args, 1));
    Date when = kNoDate;
    if (years >= 0) {
        if (years > 150) {
            return EffectStatus::BadArgument;
        }
        when = date_plus_months(world.today(), 12 * years);
    } else {
        const std::int32_t day = int_from_word(effect_arg(args, 0));
        if (day == 0) {
            return EffectStatus::BadArgument;
        }
        when = Date::from_days(day);
    }
    out.push_back(Command{DynastyId(), cmd::PlanDeath{who, when}});
    return EffectStatus::Issued;
}

EffectStatus
conceive(const EffectEnv& env, CharacterId mother, std::span<const Word> args, std::vector<Command>& out) {
    const World& world = *env.triggers.world;
    const std::optional<CharacterId> father = person_arg(args, 0);
    if (!father || !father->valid()) {
        return EffectStatus::BadArgument;
    }
    // The terms of E7-T12: a pregnancy at term after `pregnancy_months`; an event decides no miscarriage. A
    // child of the husband is legitimate, any other is not.
    const bool husband = world.is_alive(mother) && world.characters().spouse[mother.index()] == *father;
    const Date today = world.today();
    out.push_back(Command{DynastyId(),
                          cmd::Conceive{mother,
                                        *father,
                                        today,
                                        date_plus_months(today, rules_of(env).pregnancy_months),
                                        PregnancyEnd::AtTerm,
                                        husband ? Legitimacy::Legitimate : Legitimacy::Illegitimate}});
    return EffectStatus::Issued;
}

EffectStatus
create_person(const EffectEnv& env, DynastyId house, std::span<const Word> args, std::vector<Command>& out) {
    const World& world = *env.triggers.world;
    const std::int32_t sex = index_arg(args, 0);
    const std::int32_t age = int_from_word(effect_arg(args, 1));
    const std::optional<CharacterId> father = person_arg(args, 2);
    const std::optional<CharacterId> mother = person_arg(args, 3);
    if ((sex != 0 && sex != 1) || age < 0 || age > 120 || !father || !mother) {
        return EffectStatus::BadArgument;
    }
    CharacterInit init;
    init.dynasty = house;
    init.sex = sex == 0 ? Sex::Male : Sex::Female;
    init.birth_date = date_plus_months(world.today(), -12 * age);
    init.father = *father;
    init.mother = *mother;
    // The name and the portrait of E7-T09 (two draws of the run, in every branch); skills and the other
    // columns keep the defaults of CharacterInit (an event that wants more says so with change_skill ...).
    if (env.names != nullptr && env.rng != nullptr &&
        env.given_name_ids.size() == env.names->given_names.size()) {
        const std::size_t name = choose_given_name(*env.names, *env.rng, NamingContext(init.sex));
        init.portrait = choose_portrait(*env.names, *env.rng, init.sex, life_stage(rules_of(env), age));
        if (name != kNoName) {
            init.name = env.given_name_ids[name];
        }
    }
    out.push_back(Command{DynastyId(), cmd::CreatePerson{init}});
    return EffectStatus::Issued;
}

EffectStatus betroth(DynastyId house, std::span<const Word> args, std::vector<Command>& out) {
    const std::optional<CharacterId> groom = person_arg(args, 0);
    const std::optional<CharacterId> bride = person_arg(args, 1);
    const core::Fixed64 rubles = fixed_from_word(effect_arg(args, 2));
    if (!groom || !bride || !groom->valid() || !bride->valid() || rubles.is_negative()) {
        return EffectStatus::BadArgument;
    }
    const economy::Kopecks dowry = economy::roubles_to_kopecks(rubles);
    out.push_back(Command{DynastyId(), cmd::Betroth{*groom, *bride, dowry, house, effect_arg(args, 3) != 0}});
    return EffectStatus::Issued;
}

}  // namespace

std::optional<EffectStatus> character_effect_commands(const EffectEnv& env,
                                                      std::uint16_t code,
                                                      Scope target,
                                                      std::span<const Word> args,
                                                      std::vector<Command>& out) {
    const World& world = *env.triggers.world;
    const auto issue = [&](CommandPayload payload) {
        out.push_back(Command{DynastyId(), std::move(payload)});
        return EffectStatus::Issued;
    };
    const CharacterId who{target.id};
    switch (code) {
        case kEffChangeHonor:
        case kEffChangeUnity:
        case kEffChangePrestige:
        case kEffCreatePerson:
        case kEffRecordChronicle:
        case kEffBetroth:
        case kEffSetHeir: {
            const std::optional<DynastyId> house = house_of(world, target);
            if (!house) {
                return EffectStatus::BadScope;
            }
            switch (code) {
                case kEffChangeHonor:
                    return issue(cmd::AdjustHonorWithReason{
                        *house, int_from_word(effect_arg(args, 0)), kStandingReasonEvent});
                case kEffChangeUnity:
                    return issue(cmd::AdjustUnity{
                        *house, fixed32_from_word(effect_arg(args, 0)), kStandingReasonEvent});
                case kEffChangePrestige:
                    return issue(cmd::AdjustPrestige{
                        *house, fixed32_from_word(effect_arg(args, 0)), kStandingReasonEvent});
                case kEffCreatePerson:
                    return create_person(env, *house, args, out);
                case kEffRecordChronicle: {
                    const std::optional<CharacterId> a = person_arg(args, 1);
                    const std::optional<CharacterId> b = person_arg(args, 2);
                    if (!a || !b) {
                        return EffectStatus::BadArgument;
                    }
                    const auto key = static_cast<std::uint64_t>(effect_arg(args, 0));
                    return issue(cmd::RecordChronicle{
                        *house, ChronicleKind::Note, *a, *b, chronicle_event_param(key)});
                }
                case kEffSetHeir: {  // the house names its heir (SetHeir acts on its actor)
                    const std::optional<CharacterId> heir = person_arg(args, 0);
                    if (!heir || !heir->valid()) {
                        return EffectStatus::BadArgument;
                    }
                    out.push_back(Command{*house, cmd::SetHeir{*heir, CharacterId()}});
                    return EffectStatus::Issued;
                }
                default:  // kEffBetroth
                    return betroth(*house, args, out);
            }
        }
        case kEffChangeOpinion:
        case kEffAddTrait:
        case kEffRemoveTrait:
        case kEffDie:
        case kEffPlanDeath:
        case kEffSetEducation:
        case kEffConceive:
        case kEffEndMarriage:
        case kEffChangeSkill:
        case kEffChangeHealth:
        case kEffChangeStress:
        case kEffDisown:
        case kEffTonsure: {
            // A dead or unknown person is not refused here: the command says so in the CommandLog.
            if (!is_character(target)) {
                return EffectStatus::BadScope;
            }
            switch (code) {
                case kEffChangeOpinion: {
                    const std::optional<CharacterId> to = person_arg(args, 0);
                    const std::int32_t reason = int_from_word(effect_arg(args, 2));
                    if (!to || !to->valid() || reason < 0 || reason > kMaxEventOpinionReason) {
                        return EffectStatus::BadArgument;
                    }
                    return issue(
                        cmd::AdjustOpinion{who,
                                           *to,
                                           fixed32_from_word(effect_arg(args, 1)),
                                           static_cast<OpinionReason>(kOpinionReasonEvent + reason)});
                }
                case kEffAddTrait:
                case kEffRemoveTrait: {
                    const std::uint16_t slot = trait_slot_of(env.triggers, index_arg(args, 0));
                    if (slot == 0) {
                        return EffectStatus::BadArgument;
                    }
                    if (code == kEffAddTrait) {
                        return issue(cmd::GrantTrait{who, slot});
                    }
                    return issue(cmd::RevokeTrait{who, slot});
                }
                case kEffDie: {
                    const std::int32_t cause = index_arg(args, 0) + 1;
                    if (cause < 1 || cause >= kCauseOfDeathCount) {
                        return EffectStatus::BadArgument;
                    }
                    return issue(cmd::Die{who, static_cast<CauseOfDeath>(cause)});
                }
                case kEffPlanDeath:
                    return plan_death(world, who, args, out);
                case kEffSetEducation: {
                    const std::int32_t focus = index_arg(args, 0);
                    const std::optional<CharacterId> tutor = person_arg(args, 1);
                    if (focus < 0 || focus >= static_cast<std::int32_t>(kSkillCount) || !tutor) {
                        return EffectStatus::BadArgument;
                    }
                    return issue(cmd::SetEducation{who, static_cast<std::uint8_t>(focus + 1), *tutor});
                }
                case kEffConceive:
                    return conceive(env, who, args, out);
                case kEffEndMarriage: {
                    const std::int32_t reason = index_arg(args, 0);
                    if (reason < 0 || reason >= kMarriageEndCount) {
                        return EffectStatus::BadArgument;
                    }
                    return issue(cmd::EndMarriage{who, static_cast<MarriageEnd>(reason)});
                }
                case kEffChangeSkill: {
                    const std::int32_t skill = index_arg(args, 0);
                    if (skill < 0 || skill >= static_cast<std::int32_t>(kSkillCount)) {
                        return EffectStatus::BadArgument;
                    }
                    return issue(cmd::AdjustSkill{
                        who, static_cast<std::uint8_t>(skill), effect_delta(effect_arg(args, 1))});
                }
                case kEffChangeHealth:
                    return issue(cmd::AdjustHealth{who, effect_delta(effect_arg(args, 0))});
                case kEffChangeStress:
                    return issue(cmd::AdjustStress{who, effect_delta(effect_arg(args, 0))});
                default: {  // tonsure, disown: the house decides (the commands refuse the system as the
                            // actor)
                    const DynastyId house = dynasty_of_scope(world, target);
                    if (!house.valid()) {
                        return EffectStatus::BadScope;
                    }
                    if (code == kEffDisown) {
                        out.push_back(Command{house, cmd::Disown{who}});
                    } else {
                        out.push_back(Command{house, cmd::Tonsure{who}});
                    }
                    return EffectStatus::Issued;
                }
            }
        }
        case kEffGrantLand:
        case kEffAssignRegent:
        case kEffDismissRegent:
        case kEffResolveSuccessionDispute:
            return EffectStatus::Stub;  // the commands of E8-T16...T18 do not exist yet
        default:
            return std::nullopt;
    }
}

}  // namespace pat::sim::events
