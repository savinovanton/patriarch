#pragma once

#include "pat/sim/events/catalog.hpp"
#include "pat/sim/events/program.hpp"
#include "pat/sim/traits.hpp"
#include "pat/sim/world.hpp"

#include <cstdint>
#include <optional>
#include <span>

// Triggers and values of the scopes `character` and `dynasty` (docs/dev/event-engine.md section 8.2,
// E11-T07): the catalog entries (names, codes, signatures, tooltip keys, examples) and the evaluators
// that read a World. The host of the machine (VmHost::predicate / load) forwards a code to
// eval_character_predicate / eval_character_value; a code that does not belong to this block gives
// std::nullopt so that the next block (T08, E10 ...) can be asked.
//
// An entry whose system does not exist yet (CatalogEntry::stub, today `has_office`) is registered, so
// that event files may already name it, and evaluates to "no data": value 0 and has_data == false. The
// machine sees 0 / false; Trace and the reference can tell the difference.
//
// Nothing here changes the World, uses floating point or keeps state.

namespace pat::sim::events {

// Predicates (section 8.2; the codes are stable).
inline constexpr std::uint16_t kTrigIsAdult = 1001;
inline constexpr std::uint16_t kTrigIsFemale = 1002;
inline constexpr std::uint16_t kTrigIsMarried = 1003;
inline constexpr std::uint16_t kTrigIsPregnant = 1004;
inline constexpr std::uint16_t kTrigIsHeadOfDynasty = 1005;
inline constexpr std::uint16_t kTrigIsHeir = 1006;
inline constexpr std::uint16_t kTrigIsTonsured = 1007;
inline constexpr std::uint16_t kTrigIsWidowed = 1008;
inline constexpr std::uint16_t kTrigHasTrait = 1009;
inline constexpr std::uint16_t kTrigHasEducation = 1010;
inline constexpr std::uint16_t kTrigSameDynasty = 1011;
inline constexpr std::uint16_t kTrigIsAlive = 1012;
inline constexpr std::uint16_t kTrigHasLivingChild = 1013;
inline constexpr std::uint16_t kTrigIsRegent = 1014;
inline constexpr std::uint16_t kTrigIsBetrothed = 1015;
inline constexpr std::uint16_t kTrigIsDisowned = 1016;
inline constexpr std::uint16_t kTrigHasSuccessionLaw = 1035;
inline constexpr std::uint16_t kTrigHasOffice = 1200;  // E10 owns the block 1200..1249; a stub until E10-T06

// Values.
inline constexpr std::uint16_t kValAge = 1021;
inline constexpr std::uint16_t kValSkill = 1022;
inline constexpr std::uint16_t kValHealth = 1023;
inline constexpr std::uint16_t kValStress = 1024;
inline constexpr std::uint16_t kValPiety = 1025;
inline constexpr std::uint16_t kValOpinionOf = 1026;
inline constexpr std::uint16_t kValKinshipDegree = 1027;
inline constexpr std::uint16_t kValFertility = 1028;
inline constexpr std::uint16_t kValAmbition = 1029;
inline constexpr std::uint16_t kValLoyaltyHead = 1030;
inline constexpr std::uint16_t kValHonor = 1031;
inline constexpr std::uint16_t kValUnity = 1032;
inline constexpr std::uint16_t kValPrestige = 1033;
inline constexpr std::uint16_t kValTreasury = 1034;
inline constexpr std::uint16_t kValChildCount = 1036;

/// `kinship_degree` of persons that are not (blood) relatives or whose degree is not known.
inline constexpr std::int32_t kNoKinship = 255;

/// What the evaluators read. The World must stay unchanged while an evaluator runs.
struct TriggerEnv {
    const World* world = nullptr;
    /// Traits turn the stored skills, health, stress and piety into the effective values (E7-T05).
    /// Without a catalogue the stored columns are used.
    const TraitCatalog* traits = nullptr;
    /// The argument of `has_trait` is the index of the object in the content registry (what the compiler
    /// pushes); this table gives the slot stored in the characters (TraitDef::id). Empty: the argument is
    /// the slot itself.
    std::span<const std::uint16_t> trait_slots;
};

struct TriggerResult {
    /// A predicate: 0 / 1; a value: Fixed64 raw (the unit of the entry).
    Word value = 0;
    /// False for a stub: the system behind the entry does not exist yet.
    bool has_data = true;
};

/// Registers the entries of E11-T07 in `catalog` (the language entries of Catalog::engine() are
/// expected to be there already). Codes and names must not be registered twice.
void register_character_triggers(Catalog& catalog);

/// The predicate with this code over `scope`, or nullopt when the code is not one of this block.
/// A scope of the wrong kind, an unknown or dead person: false.
[[nodiscard]] std::optional<TriggerResult>
eval_character_predicate(const TriggerEnv& env, Scope scope, std::uint16_t code, std::span<const Word> args);
/// The value with this code over `scope`, or nullopt when the code is not one of this block. A scope of
/// the wrong kind or an unknown person: 0. `args` are as the machine passes them (the suffix index first
/// for `skill`).
[[nodiscard]] std::optional<TriggerResult>
eval_character_value(const TriggerEnv& env, Scope scope, std::uint16_t code, std::span<const Word> args);

}  // namespace pat::sim::events
