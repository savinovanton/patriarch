#include "pat/sim/command.hpp"

#include "pat/sim/command_log.hpp"

#include <array>

namespace pat::sim {

namespace {

using core::Result;

constexpr core::Fixed32 kZeroFixed = core::Fixed32::zero();

[[nodiscard]] Result<void, CommandError> reject(Rejection reason, const char* detail) {
    return Result<void, CommandError>::err(CommandError{reason, detail});
}
[[nodiscard]] Result<void, CommandError> accept() {
    return Result<void, CommandError>::ok();
}

/// The system (invalid actor) may do anything that is otherwise valid; a dynasty may act only for itself.
[[nodiscard]] bool may_act_for(DynastyId actor, DynastyId owner) noexcept {
    return !actor.valid() || actor == owner;
}

struct Validator {
    const World& world;
    DynastyId actor;

    Result<void, CommandError> operator()(const cmd::SetProvinceOwner& c) const {
        if (!world.exists(c.province)) {
            return reject(Rejection::UnknownEntity, "unknown province");
        }
        if (c.new_owner.valid() && !world.exists(c.new_owner)) {
            return reject(Rejection::UnknownEntity, "unknown new owner");
        }
        if (!c.new_owner.valid() && actor.valid()) {
            return reject(Rejection::NotAuthorized, "only the system may leave a province unowned");
        }
        const DynastyId current = world.provinces().owner[c.province.index()];
        if (actor.valid() && actor != current) {
            return reject(Rejection::NotAuthorized, "the actor does not own the province");
        }
        if (current == c.new_owner) {
            return reject(Rejection::InvalidState, "the province already has this owner");
        }
        return accept();
    }

    Result<void, CommandError> operator()(const cmd::CreateCharacter& c) const {
        if (!world.exists(c.dynasty)) {
            return reject(Rejection::UnknownEntity, "unknown dynasty");
        }
        if (!may_act_for(actor, c.dynasty)) {
            return reject(Rejection::NotAuthorized, "the actor may not create characters of another dynasty");
        }
        if (c.birth_date > world.today()) {
            return reject(Rejection::InvalidArgument, "birth date is in the future");
        }
        if (!world.can_create_character()) {
            return reject(Rejection::InvalidState, "the character table is full");
        }
        return check_parents(c.father, c.mother);
    }

    Result<void, CommandError> check_parents(CharacterId father, CharacterId mother) const {
        if (father.valid()) {
            if (!world.is_alive(father)) {
                return reject(Rejection::UnknownEntity, "father does not exist");
            }
            if (world.characters().sex[father.index()] != Sex::Male) {
                return reject(Rejection::InvalidArgument, "father must be male");
            }
        }
        if (mother.valid()) {
            if (!world.is_alive(mother)) {
                return reject(Rejection::UnknownEntity, "mother does not exist");
            }
            if (world.characters().sex[mother.index()] != Sex::Female) {
                return reject(Rejection::InvalidArgument, "mother must be female");
            }
        }
        return accept();
    }

    Result<void, CommandError> operator()(const cmd::CreatePerson& c) const {
        const CharacterInit& init = c.init;
        if (actor.valid()) {
            return reject(Rejection::NotAuthorized, "only the system may create persons directly");
        }
        if (!world.exists(init.dynasty)) {
            return reject(Rejection::UnknownEntity, "unknown dynasty");
        }
        if (!is_sane_date(init.birth_date) || init.birth_date > world.today()) {
            return reject(Rejection::InvalidArgument, "birth date is in the future or implausible");
        }
        if (!world.can_create_character()) {
            return reject(Rejection::InvalidState, "the character table is full");
        }
        if (init.sex != Sex::Male && init.sex != Sex::Female) {
            return reject(Rejection::InvalidArgument, "unknown sex");
        }
        if (static_cast<std::uint8_t>(init.legitimacy) >= kLegitimacyCount) {
            return reject(Rejection::InvalidArgument, "unknown legitimacy");
        }
        if (init.health < kZeroFixed || init.health > kScaleMax) {
            return reject(Rejection::InvalidArgument, "health must be within 0..100");
        }
        for (const core::Fixed32 skill : init.skills) {
            if (skill < kZeroFixed || skill > kSkillMax) {
                return reject(Rejection::InvalidArgument, "skills must be within 0..20");
            }
        }
        for (const core::Fixed32 scale : {init.fertility, init.stress, init.piety}) {
            if (scale < kZeroFixed || scale > kScaleMax) {
                return reject(Rejection::InvalidArgument,
                              "fertility, stress and piety must be within 0..100");
            }
        }
        for (const core::Fixed32 value : {init.ambition, init.loyalty_head, init.loyalty_emperor}) {
            if (value < -kSignedScaleMax || value > kSignedScaleMax) {
                return reject(Rejection::InvalidArgument, "ambition and loyalties must be within -100..100");
            }
        }
        if (!traits_are_canonical(init.traits)) {
            return reject(Rejection::InvalidArgument, "traits must be sorted, unique and packed");
        }
        if (init.education.focus > kEducationFocusCount || init.education.level > kMaxEducationLevel) {
            return reject(Rejection::InvalidArgument, "education focus or level out of range");
        }
        if (init.education.tutor.valid() && !world.is_alive(init.education.tutor)) {
            return reject(Rejection::UnknownEntity, "the tutor does not exist");
        }
        if (init.branch.valid() &&
            (!world.exists(init.branch) || world.branches().dynasty[init.branch.index()] != init.dynasty)) {
            return reject(Rejection::InvalidArgument, "the branch does not belong to the dynasty");
        }
        if (!planned_death_is_valid(init.birth_date, init.planned_death)) {
            return reject(Rejection::InvalidArgument, "planned death lies outside the lifetime");
        }
        return check_parents(init.father, init.mother);
    }

    Result<void, CommandError> operator()(const cmd::KillCharacter& c) const {
        if (world.dynasty_mode()) {
            return reject(Rejection::InvalidState, "characters do not disappear in dynasty mode (use Die)");
        }
        if (!world.is_alive(c.character)) {
            return reject(Rejection::UnknownEntity, "unknown or already dead character");
        }
        if (!may_act_for(actor, world.characters().dynasty[c.character.index()])) {
            return reject(Rejection::NotAuthorized, "the actor does not control this character");
        }
        return accept();
    }

    Result<void, CommandError> operator()(const cmd::SetDynastyHead& c) const {
        if (!world.exists(c.dynasty)) {
            return reject(Rejection::UnknownEntity, "unknown dynasty");
        }
        if (!may_act_for(actor, c.dynasty)) {
            return reject(Rejection::NotAuthorized, "the actor may not change another dynasty's head");
        }
        if (c.head.valid()) {
            if (!world.is_alive(c.head)) {
                return reject(Rejection::UnknownEntity, "unknown or dead character");
            }
            if (world.characters().dynasty[c.head.index()] != c.dynasty) {
                return reject(Rejection::InvalidState, "the new head is not a member of the dynasty");
            }
        }
        return accept();
    }

    Result<void, CommandError> operator()(const cmd::AdjustHonor& c) const {
        if (actor.valid()) {
            return reject(Rejection::NotAuthorized, "only the system may adjust honor directly");
        }
        if (!world.exists(c.dynasty)) {
            return reject(Rejection::UnknownEntity, "unknown dynasty");
        }
        return accept();
    }
};

struct Applier {
    World& world;

    void operator()(const cmd::SetProvinceOwner& c) const {
        world.set_province_owner(c.province, c.new_owner);
    }
    void operator()(const cmd::CreateCharacter& c) const {
        CharacterInit init;
        init.dynasty = c.dynasty;
        init.birth_date = c.birth_date;
        init.sex = c.sex;
        init.father = c.father;
        init.mother = c.mother;
        (void)world.create_character(init);
    }
    void operator()(const cmd::KillCharacter& c) const { (void)world.destroy_character(c.character); }
    void operator()(const cmd::SetDynastyHead& c) const { (void)world.set_dynasty_head(c.dynasty, c.head); }
    void operator()(const cmd::AdjustHonor& c) const { world.add_honor(c.dynasty, c.delta); }
    void operator()(const cmd::CreatePerson& c) const { (void)world.create_character(c.init); }
};

// ---- stable tags ------------------------------------------------------------------------------
// One specialisation per payload type. A new command without one does not compile; the tag of an
// existing command must never change (it is written to replays and saves).

template <class T>
struct CommandTag;  // intentionally undefined for unknown payload types

template <>
struct CommandTag<cmd::SetProvinceOwner> {
    static constexpr CommandType value = CommandType::SetProvinceOwner;
};
template <>
struct CommandTag<cmd::CreateCharacter> {
    static constexpr CommandType value = CommandType::CreateCharacter;
};
template <>
struct CommandTag<cmd::KillCharacter> {
    static constexpr CommandType value = CommandType::KillCharacter;
};
template <>
struct CommandTag<cmd::SetDynastyHead> {
    static constexpr CommandType value = CommandType::SetDynastyHead;
};
template <>
struct CommandTag<cmd::AdjustHonor> {
    static constexpr CommandType value = CommandType::AdjustHonor;
};
template <>
struct CommandTag<cmd::CreatePerson> {
    static constexpr CommandType value = CommandType::CreatePerson;
};

template <class Variant>
struct TagTable;
template <class... Alternatives>
struct TagTable<std::variant<Alternatives...>> {
    static constexpr std::array<CommandType, sizeof...(Alternatives)> value = {
        CommandTag<Alternatives>::value...};
};

constexpr auto kTagByAlternative = TagTable<CommandPayload>::value;

constexpr bool tags_are_valid_and_unique() {
    for (std::size_t i = 0; i < kTagByAlternative.size(); ++i) {
        if (static_cast<std::uint8_t>(kTagByAlternative[i]) == 0) {
            return false;
        }
        for (std::size_t j = i + 1; j < kTagByAlternative.size(); ++j) {
            if (kTagByAlternative[i] == kTagByAlternative[j]) {
                return false;
            }
        }
    }
    return true;
}
static_assert(tags_are_valid_and_unique(), "every command needs its own non-zero tag");

}  // namespace

CommandType type_of(const Command& command) noexcept {
    return kTagByAlternative[command.payload.index()];  // the payload is never valueless
}

std::string_view to_string(CommandType type) noexcept {
    switch (type) {
        case CommandType::SetProvinceOwner:
            return "SetProvinceOwner";
        case CommandType::CreateCharacter:
            return "CreateCharacter";
        case CommandType::KillCharacter:
            return "KillCharacter";
        case CommandType::SetDynastyHead:
            return "SetDynastyHead";
        case CommandType::AdjustHonor:
            return "AdjustHonor";
        case CommandType::CreatePerson:
            return "CreatePerson";
    }
    return "?";
}

std::string_view to_string(Rejection reason) noexcept {
    switch (reason) {
        case Rejection::None:
            return "accepted";
        case Rejection::UnknownEntity:
            return "unknown entity";
        case Rejection::NotAuthorized:
            return "not authorized";
        case Rejection::InvalidArgument:
            return "invalid argument";
        case Rejection::InvalidState:
            return "invalid state";
    }
    return "?";
}

Result<void, CommandError> validate(const World& world, const Command& command) {
    if (command.actor.valid() && !world.exists(command.actor)) {
        return reject(Rejection::UnknownEntity, "unknown actor");
    }
    return std::visit(Validator{world, command.actor}, command.payload);
}

void apply(World& world, const Command& command) {
    std::visit(Applier{world}, command.payload);
}

ApplyStats
process_commands(World& world, std::span<const Command> commands, CommandSource source, CommandLog& log) {
    ApplyStats stats;
    for (const Command& command : commands) {
        const Result<void, CommandError> verdict = validate(world, command);
        LoggedCommand entry;
        entry.date = world.today();
        entry.source = source;
        entry.command = command;
        if (verdict.has_value()) {
            apply(world, command);
            ++stats.applied;
        } else {
            entry.outcome = verdict.error().reason;
            ++stats.rejected;
        }
        log.append(entry);
    }
    return stats;
}

}  // namespace pat::sim
