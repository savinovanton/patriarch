#pragma once

#include "pat/core/result.hpp"
#include "pat/sim/world.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <variant>

// Commands are the ONLY way UI, AI and scenarios change the simulation state (ADR-0001).
// A command is validated against the current world and then applied at a tick boundary; both the
// outcome and the command itself go to the command log, which makes replays possible.

namespace pat::sim {

/// Stable numeric tags: these values are written to replay files and must never change. They are
/// NOT the position of the command in CommandPayload: the table in command.cpp maps each payload
/// type to its tag, so that epics can reserve blocks (ADR-0006 D5):
///   1..5 E1   6..31 E7 (characters and dynasties)   32..63 E9   64..95 E8   96..255 later epics
enum class CommandType : std::uint8_t {
    SetProvinceOwner = 1,
    CreateCharacter = 2,
    KillCharacter = 3,
    SetDynastyHead = 4,
    AdjustHonor = 5,
    CreatePerson = 9,  // E7-T02
};

namespace cmd {

struct SetProvinceOwner {
    ProvinceId province;
    DynastyId new_owner;  // invalid = unowned (system only)
    [[nodiscard]] bool operator==(const SetProvinceOwner&) const = default;
};
struct CreateCharacter {
    DynastyId dynasty;
    Date birth_date;
    Sex sex = Sex::Male;
    CharacterId father;
    CharacterId mother;
    [[nodiscard]] bool operator==(const CreateCharacter&) const = default;
};
struct KillCharacter {
    CharacterId character;
    [[nodiscard]] bool operator==(const KillCharacter&) const = default;
};
struct SetDynastyHead {
    DynastyId dynasty;
    CharacterId head;
    [[nodiscard]] bool operator==(const SetDynastyHead&) const = default;
};
struct AdjustHonor {
    DynastyId dynasty;
    std::int32_t delta = 0;
    [[nodiscard]] bool operator==(const AdjustHonor&) const = default;
};

/// Full character creation (scenarios, births): every column of CharacterInit. Unlike
/// World::create_character, out-of-range values are rejected by validate() instead of clamped.
/// System only.
struct CreatePerson {
    CharacterInit init;
    [[nodiscard]] bool operator==(const CreatePerson&) const = default;
};

}  // namespace cmd

using CommandPayload = std::variant<cmd::SetProvinceOwner,
                                    cmd::CreateCharacter,
                                    cmd::KillCharacter,
                                    cmd::SetDynastyHead,
                                    cmd::AdjustHonor,
                                    cmd::CreatePerson>;

/// A command with its issuer. `actor` is the dynasty on whose behalf the command is issued; an
/// invalid actor means "the system" (scenario setup, game rules), which may do anything valid.
struct Command {
    DynastyId actor;
    CommandPayload payload;

    [[nodiscard]] bool operator==(const Command&) const = default;
};

/// Where a command came from. Only External commands are recorded for replay input; Internal ones
/// (emitted by systems/AI) regenerate themselves when the simulation is replayed.
enum class CommandSource : std::uint8_t { External = 0, Internal = 1 };

enum class Rejection : std::uint8_t {
    None = 0,
    UnknownEntity,    // a referenced entity does not exist (or is dead)
    NotAuthorized,    // the actor may not do this
    InvalidArgument,  // the arguments are inconsistent (e.g. birth date in the future)
    InvalidState,     // the current state forbids it
};

struct CommandError {
    Rejection reason = Rejection::None;
    const char* detail = "";  // static string describing the first violated rule
};

[[nodiscard]] CommandType type_of(const Command& command) noexcept;
[[nodiscard]] std::string_view to_string(CommandType type) noexcept;
[[nodiscard]] std::string_view to_string(Rejection reason) noexcept;

/// Checks the command against the world without changing it.
[[nodiscard]] core::Result<void, CommandError> validate(const World& world, const Command& command);

/// Applies a command that passed validate() against the same world state.
void apply(World& world, const Command& command);

struct ApplyStats {
    std::size_t applied = 0;
    std::size_t rejected = 0;
};

class CommandLog;

/// Validates and applies the commands in order, recording every command with its outcome.
ApplyStats
process_commands(World& world, std::span<const Command> commands, CommandSource source, CommandLog& log);

}  // namespace pat::sim
