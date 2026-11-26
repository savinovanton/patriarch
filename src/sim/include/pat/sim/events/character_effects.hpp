#pragma once

#include "pat/sim/events/catalog.hpp"
#include "pat/sim/events/effects.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// Effects of the scopes `character` and `dynasty` (docs/dev/event-engine.md section 8.3, E11-T09): the
// catalog entries (names, codes, signatures, tooltip keys, examples) and their translation into commands.
// The table is in catalog_character_effects.cpp, the translation in character_effects.cpp, the Russian
// tooltip templates in data/loc/ru/events/tooltip_effects_character.pd.
//
// The effects of E8 whose commands do not exist yet (grant_land, assign_regent, dismiss_regent,
// resolve_succession_dispute) are registered as stubs ("E8"): event files may name them,
// they compile and issue nothing (EffectStatus::Stub) until their E8 task adds the command.

namespace pat::sim::events {

// Codes (section 8.3; stable, never reused).
inline constexpr std::uint16_t kEffChangeHonor = 2001;
inline constexpr std::uint16_t kEffChangeUnity = 2002;
inline constexpr std::uint16_t kEffChangePrestige = 2003;
inline constexpr std::uint16_t kEffChangeOpinion = 2004;
inline constexpr std::uint16_t kEffAddTrait = 2005;
inline constexpr std::uint16_t kEffRemoveTrait = 2006;
inline constexpr std::uint16_t kEffDie = 2007;
inline constexpr std::uint16_t kEffPlanDeath = 2008;
inline constexpr std::uint16_t kEffSetEducation = 2009;
inline constexpr std::uint16_t kEffConceive = 2010;
inline constexpr std::uint16_t kEffCreatePerson = 2011;
inline constexpr std::uint16_t kEffEndMarriage = 2012;
inline constexpr std::uint16_t kEffChangeSkill = 2013;
inline constexpr std::uint16_t kEffChangeHealth = 2014;
inline constexpr std::uint16_t kEffChangeStress = 2015;
inline constexpr std::uint16_t kEffRecordChronicle = 2016;
inline constexpr std::uint16_t kEffBetroth = 2017;
inline constexpr std::uint16_t kEffTonsure = 2018;
inline constexpr std::uint16_t kEffSetHeir = 2019;
inline constexpr std::uint16_t kEffDisown = 2020;
inline constexpr std::uint16_t kEffGrantLand = 2021;                 // stub until E8-T18
inline constexpr std::uint16_t kEffAssignRegent = 2022;              // stub until E8-T17
inline constexpr std::uint16_t kEffDismissRegent = 2023;             // stub until E8-T17
inline constexpr std::uint16_t kEffResolveSuccessionDispute = 2024;  // stub until E8-T16

/// The reason of the honor, unity and prestige changes made by events (the block 500...599 of the
/// standing and opinion reasons belongs to E11; E9 has 1...3, E7 10...79, E8 101...299, E10 300...399,
/// E12 400...499).
inline constexpr StandingReason kStandingReasonEvent = 500;
/// The reason of an opinion changed by an event: kOpinionReasonEvent + `reason` of `change_opinion`
/// (0...kMaxEventOpinionReason), so that an event can keep its sources apart.
inline constexpr OpinionReason kOpinionReasonEvent = 500;
inline constexpr std::int32_t kMaxEventOpinionReason = 99;

/// The `param` of a chronicle note written by an event (section 10.6): bit 30 marks the origin E11, the
/// low 30 bits are those of the 64-bit key of the template name (`record_chronicle = { key = ... }`).
inline constexpr std::int32_t kChronicleEventBit = 0x40000000;
[[nodiscard]] constexpr std::int32_t chronicle_event_param(std::uint64_t key) noexcept {
    return kChronicleEventBit | static_cast<std::int32_t>(key & 0x3FFFFFFFU);
}

/// Registers the entries of E11-T09 in `catalog` (after Catalog::engine()).
void register_character_effects(Catalog& catalog);

/// The commands of an effect of this block, or nullopt when the code is not one of it.
[[nodiscard]] std::optional<EffectStatus> character_effect_commands(const EffectEnv& env,
                                                                    std::uint16_t code,
                                                                    Scope target,
                                                                    std::span<const Word> args,
                                                                    std::vector<Command>& out);

}  // namespace pat::sim::events
