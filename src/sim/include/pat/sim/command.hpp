#pragma once

#include "pat/core/result.hpp"
#include "pat/sim/world.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

// Commands are the ONLY way UI, AI and scenarios change the simulation state (ADR-0001).
// A command is validated against the current world and then applied at a tick boundary; both the
// outcome and the command itself go to the command log, which makes replays possible.

namespace pat::sim {

struct TraitCatalog;

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
    Die = 6,                           // E7-T04
    Marry = 7,                         // E7-T03
    EndMarriage = 8,                   // E7-T03
    CreatePerson = 9,                  // E7-T02
    GrantTrait = 14,                   // E7-T08
    RevokeTrait = 15,                  // E7-T08
    PlanDeath = 16,                    // E7-T10
    AdjustUnity = 17,                  // E7-T14
    RecordChronicle = 18,              // E7-T16
    AdjustOpinion = 19,                // E7-T17
    AdjustPrestige = 20,               // E7-T14
    AdjustHonorWithReason = 21,        // E7-T14
    SetTaxPolicy = 32,                 // E9-T12
    SetRecruitPolicy = 33,             // E9-T12
    ConscriptRecruits = 34,            // E9-T13
    AssignProvinceAdministrator = 35,  // E9-T10
};

/// Why a marriage is ended by cmd::EndMarriage. Death is not a reason: Die ends the marriage itself.
enum class MarriageEnd : std::uint8_t { Annulment, Tonsure };
inline constexpr std::uint8_t kMarriageEndCount = 2;

namespace cmd {

struct GrantTrait {
    CharacterId character;
    std::uint16_t trait = 0;
    [[nodiscard]] bool operator==(const GrantTrait&) const = default;
};

struct RevokeTrait {
    CharacterId character;
    std::uint16_t trait = 0;
    [[nodiscard]] bool operator==(const RevokeTrait&) const = default;
};

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

struct AdjustUnity {
    DynastyId dynasty;
    core::Fixed32 delta;
    StandingReason reason = kStandingReasonUnspecified;
    [[nodiscard]] bool operator==(const AdjustUnity&) const = default;
};

struct AdjustPrestige {
    DynastyId dynasty;
    core::Fixed32 delta;
    StandingReason reason = kStandingReasonUnspecified;
    [[nodiscard]] bool operator==(const AdjustPrestige&) const = default;
};

/// Dynasty-mode honor change with a reason. The legacy AdjustHonor tag keeps its original layout.
struct AdjustHonorWithReason {
    DynastyId dynasty;
    std::int32_t delta = 0;
    StandingReason reason = kStandingReasonUnspecified;
    [[nodiscard]] bool operator==(const AdjustHonorWithReason&) const = default;
};

/// Full character creation (scenarios, births): every column of CharacterInit. Unlike
/// World::create_character, out-of-range values are rejected by validate() instead of clamped.
/// System only.
struct CreatePerson {
    CharacterInit init;
    [[nodiscard]] bool operator==(const CreatePerson&) const = default;
};

/// A character dies (E7-T04): the row moves to the archive, see World::die. System only (the player
/// "kills" through the commands of intrigue, execution and war, which end in Die issued by the
/// system), dynasty mode only. In E1 mode KillCharacter is the way, and it is rejected in dynasty mode.
///
/// Contract for the epics that keep references to people (E8 offices, E10 schemes, E14 armies, E17
/// regencies and treaties, E9 administrators): a Die that was applied is in the command journal of the
/// tick (`CommandLog`, outcome None). React to it there, from your own system, and drop or hand over
/// whatever refers to `character`; the archive record (`World::archived_person`) tells who died, when
/// and of what, and the member list and head slot of the dynasty are already clean.
struct Die {
    CharacterId character;
    CauseOfDeath cause = CauseOfDeath::OldAge;
    [[nodiscard]] bool operator==(const Die&) const = default;
};

/// Schedules a death or cancels it with kNoDate. System only, dynasty mode only.
struct PlanDeath {
    CharacterId character;
    Date date = kNoDate;
    [[nodiscard]] bool operator==(const PlanDeath&) const = default;
};

/// Adds an editorial note to a dynasty's chronicle. Factual entries come from world operations.
struct RecordChronicle {
    DynastyId dynasty;
    ChronicleKind kind = ChronicleKind::Note;
    CharacterId a;
    CharacterId b;
    std::int32_t param = 0;

    [[nodiscard]] bool operator==(const RecordChronicle&) const = default;
};

/// Adjusts one living character's opinion of another. System only.
struct AdjustOpinion {
    CharacterId from;
    CharacterId to;
    core::Fixed32 delta;
    OpinionReason reason = 0;

    [[nodiscard]] bool operator==(const AdjustOpinion&) const = default;
};

/// Appoints a character as the administrator of a province, or dismisses the present one when
/// `administrator` is invalid (E9-T10, GDD 03 section 3.2). Needs an economy. A dynasty may appoint
/// a living member of its own house to a province it owns; the system (invalid actor) may appoint any
/// living character to any province ("appointed lawfully": the crown, an office, a grant). Control
/// and corruption do not jump: they move toward the targets of the new administrator over the next
/// months (economy.provinces, step 1).
struct AssignProvinceAdministrator {
    ProvinceId province;
    CharacterId administrator;  // invalid = no administrator
    [[nodiscard]] bool operator==(const AssignProvinceAdministrator&) const = default;
};

/// The tax policy of a dynasty for all its provinces (E9-T12, GDD 03 section 9): a level index, 0 = low
/// ... 3 = excessive in M1 (the number of levels comes from the rules, see
/// World::set_economy_policy_levels). Needs an economy. A dynasty sets its own policy (`actor ==
/// dynasty`), the system (invalid actor) may set any. Changes only the choice of the dynasty: the
/// level takes effect in the next monthly pass of each of its provinces (economy::effective_tax_level).
struct SetTaxPolicy {
    DynastyId dynasty;
    std::uint8_t level = 0;
    [[nodiscard]] bool operator==(const SetTaxPolicy&) const = default;
};

/// The recruit policy of a dynasty (E9-T12): 0 = voluntary ... 2 = intensified in M1. Same rules as
/// SetTaxPolicy.
struct SetRecruitPolicy {
    DynastyId dynasty;
    std::uint8_t level = 0;
    [[nodiscard]] bool operator==(const SetRecruitPolicy&) const = default;
};

/// Conscription (E9-T13, GDD 03 section 10): takes `count` people out of the recruit pool of a province
/// and out of its peasants, cossacks and townsfolk (proportionally to their sizes); the manpower reserve
/// of the owner dynasty grows by exactly `count` (the reserve is spent by E16). Needs an economy. The
/// province must be held by a dynasty: that dynasty (`actor`) or the system (invalid actor) may do it
/// (NotAuthorized for anyone else); `count` is 1 or more and not above the pool (and the people of
/// the recruit groups), so a conscription never takes more than the potential. The sizes fall at
/// once, so output and potential fall with them; the mood answers through the recruit policy (F8).
struct ConscriptRecruits {
    ProvinceId province;
    std::uint32_t count = 0;
    [[nodiscard]] bool operator==(const ConscriptRecruits&) const = default;
};

/// Marries two living, unmarried characters (E7-T03). System only, dynasty mode only: the rules of
/// marriage (consent, age, blood relationship, the Church) are checked by E8 before it emits this.
struct Marry {
    CharacterId a;
    CharacterId b;
    [[nodiscard]] bool operator==(const Marry&) const = default;
};
/// Ends the marriage of `a` (annulment, tonsure; widowhood is part of Die). System only.
struct EndMarriage {
    CharacterId a;
    MarriageEnd reason = MarriageEnd::Annulment;
    [[nodiscard]] bool operator==(const EndMarriage&) const = default;
};

}  // namespace cmd

using CommandPayload = std::variant<cmd::SetProvinceOwner,
                                    cmd::CreateCharacter,
                                    cmd::KillCharacter,
                                    cmd::SetDynastyHead,
                                    cmd::AdjustHonor,
                                    cmd::CreatePerson,
                                    cmd::Die,
                                    cmd::Marry,
                                    cmd::EndMarriage,
                                    cmd::PlanDeath,
                                    cmd::RecordChronicle,
                                    cmd::AdjustOpinion,
                                    cmd::AdjustUnity,
                                    cmd::AdjustPrestige,
                                    cmd::AdjustHonorWithReason,
                                    cmd::AssignProvinceAdministrator,
                                    cmd::SetTaxPolicy,
                                    cmd::SetRecruitPolicy,
                                    cmd::GrantTrait,
                                    cmd::RevokeTrait,
                                    cmd::ConscriptRecruits>;

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
[[nodiscard]] core::Result<void, CommandError>
validate(const World& world, const Command& command, const TraitCatalog* traits = nullptr);

/// Applies a command that passed validate() against the same world state.
void apply(World& world, const Command& command, const TraitCatalog* traits = nullptr);

struct ApplyStats {
    std::size_t applied = 0;
    std::size_t rejected = 0;
};

class CommandLog;

struct DepartedHead {
    DynastyId dynasty;
    CharacterId predecessor;
};

/// Validates and applies the commands in order, recording every command with its outcome.
/// If supplied, departed_heads receives the last incumbent killed by an accepted Die per dynasty,
/// sorted by dynasty id.
ApplyStats process_commands(World& world,
                            std::span<const Command> commands,
                            CommandSource source,
                            CommandLog& log,
                            std::vector<DepartedHead>* departed_heads = nullptr,
                            const TraitCatalog* traits = nullptr);

}  // namespace pat::sim
