#include "pat/sim/command.hpp"

#include "pat/sim/command_log.hpp"

namespace pat::sim {

namespace {

using core::Result;

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
        if (c.father.valid()) {
            if (!world.is_alive(c.father)) {
                return reject(Rejection::UnknownEntity, "father does not exist");
            }
            if (world.characters().sex[c.father.index()] != Sex::Male) {
                return reject(Rejection::InvalidArgument, "father must be male");
            }
        }
        if (c.mother.valid()) {
            if (!world.is_alive(c.mother)) {
                return reject(Rejection::UnknownEntity, "mother does not exist");
            }
            if (world.characters().sex[c.mother.index()] != Sex::Female) {
                return reject(Rejection::InvalidArgument, "mother must be female");
            }
        }
        return accept();
    }

    Result<void, CommandError> operator()(const cmd::KillCharacter& c) const {
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
};

}  // namespace

CommandType type_of(const Command& command) noexcept {
    // The variant alternatives are declared in the same order as the (1-based) CommandType values.
    static_assert(std::variant_size_v<CommandPayload> == 5, "update CommandType when adding commands");
    return static_cast<CommandType>(command.payload.index() + 1);
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
