#pragma once

#include "pat/sim/events/catalog.hpp"
#include "pat/sim/events/character_triggers.hpp"
#include "pat/sim/events/program.hpp"
#include "pat/sim/world.hpp"

#include <cstdint>
#include <optional>
#include <span>

// Triggers and values of the scopes `province` and `realm`, of time, of the state of events (flags,
// variables, modifiers, cooldowns, chains) and of chance (docs/dev/event-engine.md section 8.2, E11-T08):
// the catalog entries (names, codes, signatures, tooltip keys, examples) and the evaluators that read a
// World. The host of the machine asks eval_realm_predicate / eval_realm_value after the character block
// (E11-T07); a code that does not belong to this block gives std::nullopt.
//
// Three entries are words of the language that the compiler turns into instructions itself: `variable.<name>`
// and `global_variable.<name>` (LOAD_VAR) and `random_chance` (RAND_PERMILLE, a draw from the stream of the
// event). They are registered for the reference and the tooltips; the evaluators answer the two variables
// by key and `random_chance` with "no data" (there is no stream here: the host never asks for it).
//
// An entry that needs the economy (unrest, control, corruption, recruits, buildings, groups, debt, quotas)
// answers "no data" (value 0, has_data == false) in a world without economy; `unrest_state` also needs
// TriggerEnv::economy (the thresholds).
//
// Nothing here changes the World, uses floating point or keeps state.

namespace pat::sim::events {

// Predicates (section 8.2; the codes are stable).
inline constexpr std::uint16_t kTrigHasFlag = 1101;
inline constexpr std::uint16_t kTrigHasGlobalFlag = 1102;
inline constexpr std::uint16_t kTrigHasModifier = 1103;
inline constexpr std::uint16_t kTrigHasFired = 1104;
inline constexpr std::uint16_t kTrigRandomChance = 1108;
inline constexpr std::uint16_t kTrigHasBuilding = 1117;
inline constexpr std::uint16_t kTrigIsPlayerControlled = 1120;
inline constexpr std::uint16_t kTrigHasCooldown = 1121;
inline constexpr std::uint16_t kTrigHasQuota = 1125;

// Values.
inline constexpr std::uint16_t kValChainStep = 1105;
inline constexpr std::uint16_t kValVariable = 1106;
inline constexpr std::uint16_t kValGlobalVariable = 1107;
inline constexpr std::uint16_t kValCurrentYear = 1109;
inline constexpr std::uint16_t kValCurrentMonth = 1110;
inline constexpr std::uint16_t kValDate = 1111;
inline constexpr std::uint16_t kValUnrest = 1112;
inline constexpr std::uint16_t kValControl = 1113;
inline constexpr std::uint16_t kValCorruption = 1114;
inline constexpr std::uint16_t kValPopulation = 1115;
inline constexpr std::uint16_t kValRecruitPool = 1116;
inline constexpr std::uint16_t kValUnrestState = 1118;
inline constexpr std::uint16_t kValDevelopment = 1119;
inline constexpr std::uint16_t kValGroupSize = 1122;
inline constexpr std::uint16_t kValGroupMood = 1123;
inline constexpr std::uint16_t kValDebt = 1124;

/// Registers the entries of E11-T08 in `catalog` (after Catalog::engine()). Codes and names must not be
/// registered twice.
void register_realm_triggers(Catalog& catalog);

/// The predicate with this code over `scope`, or nullopt when the code is not one of this block. Arguments
/// are as the machine passes them: a name is its 64-bit key, a reference or an enum its index.
[[nodiscard]] std::optional<TriggerResult>
eval_realm_predicate(const TriggerEnv& env, Scope scope, std::uint16_t code, std::span<const Word> args);
/// The value with this code over `scope`, or nullopt when the code is not one of this block (the suffix
/// index first for `group_size` / `group_mood`).
[[nodiscard]] std::optional<TriggerResult>
eval_realm_value(const TriggerEnv& env, Scope scope, std::uint16_t code, std::span<const Word> args);

/// The building type (BuildingDef::slot, what ProvinceEconomy::slot_building holds) of a `building`
/// argument: the index of the content registry translated by TriggerEnv::building_slots, or the type itself
/// when the table is empty; 0 for an unknown index (`has_building`, `start_construction` of E11-T10).
[[nodiscard]] std::uint8_t building_slot_of(const TriggerEnv& env, std::int32_t index);

}  // namespace pat::sim::events
