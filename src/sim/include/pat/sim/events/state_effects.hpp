#pragma once

#include "pat/sim/events/catalog.hpp"
#include "pat/sim/events/effects.hpp"
#include "pat/sim/events/event_set.hpp"
#include "pat/sim/events/state.hpp"
#include "pat/sim/events/vm.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

// The operations over the state of the events (docs/dev/event-engine.md sections 8.3 "T11", 11.1, 11.3;
// E11-T11): flags with an end date, variables, modifiers, chains, global flags, `trigger_event` (a timer or
// an immediate start) and `notify`. They are not commands: the machine leaves them in RunResult::state_ops,
// and the caller (the dispatcher at a launch, the resolution of an option in T12) commits them through
// apply_state_ops() when the run succeeded, at the end of the run, so the next run sees them at once (section
// 7.6). Everything goes into the tables of the World (rule 3 of AGENTS.md), so saves and replays hold it.
//
// `save_scope_as` (code 2201) is a keyword of the compiler (SCOPE_SAVE), not an entry of the catalog. The
// saved scopes travel with a chain: a timer or an immediate `trigger_event` keeps them by name
// (saved_name_key) and the started event finds them under the same names (its `chain_scopes`, docs
// section 4.2).

namespace pat::sim {
class World;
}

namespace pat::sim::events {

// Codes (section 8.3; stable, never reused).
inline constexpr std::uint16_t kStSaveScopeAs = 2201;  // the compiler keyword, listed for the codes only
inline constexpr std::uint16_t kStTriggerEvent = 2202;
inline constexpr std::uint16_t kStSetFlag = 2203;
inline constexpr std::uint16_t kStRemoveFlag = 2204;
inline constexpr std::uint16_t kStSetVariable = 2205;
inline constexpr std::uint16_t kStChangeVariable = 2206;
inline constexpr std::uint16_t kStAddModifier = 2207;
inline constexpr std::uint16_t kStRemoveModifier = 2208;
inline constexpr std::uint16_t kStStartChain = 2209;
inline constexpr std::uint16_t kStSetChainStep = 2210;
inline constexpr std::uint16_t kStEndChain = 2211;
inline constexpr std::uint16_t kStSetGlobalFlag = 2212;
inline constexpr std::uint16_t kStRemoveGlobalFlag = 2213;
inline constexpr std::uint16_t kStNotify = 2214;

/// The longest delay or duration an operation accepts, in days (longer values are clamped).
inline constexpr std::int64_t kMaxStateDays = 200LL * 366;

/// `detail` of a history row HistoryOutcome::Aborted written for a `trigger_event` whose scope does not fit
/// the started event (the abort reasons of the machine use the values of AbortReason, 1...9).
inline constexpr std::uint16_t kAbortScopeMismatch = 100;

/// Registers the entries of E11-T11 in `catalog` (after Catalog::engine()).
void register_state_effects(Catalog& catalog);

/// The key of a saved scope name in a timer or a queue row: the low 32 bits of event_key(name), never 0.
[[nodiscard]] std::uint32_t saved_name_key(std::string_view name) noexcept;

using SavedSlots = std::array<Scope, limits::kMaxSavedScopes>;

/// The filled slots of an event as rows by name (packed, at most kMaxSavedScopes); returns their count.
std::uint8_t pack_saved(const EventDef& event, const SavedSlots& slots, SavedScopes& out);
/// The rows put back into the slots of `event` by name; a name the event does not know is dropped.
[[nodiscard]] SavedSlots unpack_saved(const EventDef& event, const SavedScopes& rows, std::uint8_t count);

/// An immediate `trigger_event` (no delay): the dispatcher starts it in the same pass (section 11.1).
struct ChainStart {
    EventKey event = 0;
    Scope scope;
    bool force = false;             // the trigger of the started event is not checked
    bool respect_cooldown = false;  // the cooldown and `once` are checked (a chain normally skips them)

    [[nodiscard]] friend constexpr bool operator==(const ChainStart&, const ChainStart&) noexcept = default;
};

/// Where the operations of one run go.
struct StateOpContext {
    World* world = nullptr;
    const EventSet* set = nullptr;
    const EventDef* event = nullptr;  // the running event (the names of its saved scopes)
    std::uint8_t depth = 0;           // the chain depth of the running instance (timers get depth + 1)
    SavedSlots saved{};               // the saved scopes at the end of the run
};

/// What the commit did besides the tables.
struct StateOpReport {
    std::vector<ChainStart> chained;  // immediate starts, in program order
    std::vector<EffectIssue> issues;  // operations that changed nothing, and why
    std::uint32_t applied = 0;
    bool state_full = false;  // a table at its bound refused a row (the dispatcher records StateFull)
};

/// Commits one operation. BadScope: the target is not a scope the tables accept; BadArgument: a zero key,
/// an unknown event or modifier, a refused row.
EffectStatus apply_state_op(const StateOpContext& context,
                            std::uint16_t code,
                            Scope target,
                            std::span<const Word> args,
                            StateOpReport& report);

/// Commits every operation of a run (RunResult::state_ops) in program order.
void apply_state_ops(const StateOpContext& context, const OpBuffer& ops, StateOpReport& report);

}  // namespace pat::sim::events
