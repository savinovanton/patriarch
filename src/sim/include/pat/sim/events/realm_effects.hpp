#pragma once

#include "pat/sim/economy/money.hpp"
#include "pat/sim/events/catalog.hpp"
#include "pat/sim/events/effects.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// Effects of the scopes `province` and `dynasty` over the realm and its money (docs/dev/event-engine.md
// section 8.3, E11-T10): the catalog entries and their translation into commands, with the system as the
// actor. The table is in catalog_realm_effects.cpp, the translation in realm_effects.cpp, the Russian
// tooltip templates in data/loc/ru/events/tooltip_effects_realm.pd.
//
// The commands are those of E9 where one exists (SetProvinceOwner, AssignProvinceAdministrator,
// StartConstruction, ConscriptRecruits) and the commands of the block of E11 otherwise: PostEventMoney
// (103) for `pay` / `receive`, AdjustProvinceUnrest / AdjustProvinceControl / AdjustGroupMood (107...109)
// for one-off shifts of the columns. An effect never writes the World: the economy sees a payment of an
// event as one transfer of the pair EventExpense -> EventIncome between the house and the crown (no money
// is made), and a shift as a change of the column that the month then moves toward its target (F7...F9).
//
// The constructions of the flow (`if` / `else_if` / `else`, `random_list`, `random`, `every_`, `random_`,
// `ordered_`, `hidden_effect`; codes 2150...2156 of section 8.3) are words of the language: the compiler
// (E11-T06) turns them into jumps, draws and iterators of the machine (E11-T04), so they have no entry and no
// translation here; their effects arrive as the effects of the blocks.

namespace pat::sim::events {

// Codes (section 8.3; stable, never reused).
inline constexpr std::uint16_t kEffSetProvinceOwner = 2101;
inline constexpr std::uint16_t kEffAssignAdministrator = 2102;
inline constexpr std::uint16_t kEffChangeUnrest = 2103;
inline constexpr std::uint16_t kEffChangeControl = 2104;
inline constexpr std::uint16_t kEffChangeGroupMood = 2105;
inline constexpr std::uint16_t kEffPay = 2106;
inline constexpr std::uint16_t kEffReceive = 2107;
inline constexpr std::uint16_t kEffStartConstruction = 2108;
inline constexpr std::uint16_t kEffConscript = 2109;

/// Registers the entries of E11-T10 in `catalog` (after Catalog::engine()).
void register_realm_effects(Catalog& catalog);

/// The commands of an effect of this block, or nullopt when the code is not one of it.
[[nodiscard]] std::optional<EffectStatus> realm_effect_commands(const EffectEnv& env,
                                                                std::uint16_t code,
                                                                Scope target,
                                                                std::span<const Word> args,
                                                                std::vector<Command>& out);

/// The kopecks `house` lacks for the event payments among `commands` (PostEventMoney of the house, in
/// order: what it receives first pays what follows), 0 when it can make them all. The implicit condition
/// of an option with `pay` (section 4.3, "Не хватает N ₽"): the owner of the option list (E11-T12/T13)
/// asks it over the commands of the Preview of the option.
[[nodiscard]] economy::Kopecks
payment_shortfall(const World& world, DynastyId house, std::span<const Command> commands);

}  // namespace pat::sim::events
