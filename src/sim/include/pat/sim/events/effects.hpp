#pragma once

#include "pat/core/rng.hpp"
#include "pat/core/string_interner.hpp"
#include "pat/sim/command.hpp"
#include "pat/sim/demography.hpp"
#include "pat/sim/events/character_triggers.hpp"
#include "pat/sim/events/program.hpp"
#include "pat/sim/events/vm.hpp"
#include "pat/sim/names.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// Effects as commands (docs/dev/event-engine.md sections 6.3, 7.6 and 8.3; E11-T09). The machine never
// changes the World: an effect program leaves its effects in RunResult::commands (Run) or reports them as
// lines to a PreviewSink (Preview), each as {code, target scope, arguments}. This file turns one such
// effect into the command(s) it stands for, with the system as the actor (an invalid DynastyId), or with the
// house where the command acts on its actor (`tonsure`, `disown`, `set_heir`: the house decides). The
// caller queues them with TickContext::emit; they are validated and applied at the next tick boundary and
// recorded in the CommandLog like any Internal command, so saves and replays reproduce them.
//
// The same function serves a Run entry and a Preview line, so "what is shown is what is applied" (section
// 13.4) holds by construction: equal {code, target, arguments} give equal commands. A command that
// validate() later refuses does not stop the other effects of the run (each command is validated on its
// own); the refusal is in the CommandLog.
//
// Each catalog block (character and dynasty: E11-T09; province, economy: T10; later epics) answers the
// codes it owns; effect_commands asks them in turn. Nothing here changes the World or keeps state; the only
// thing consumed is EffectEnv::rng (the name and portrait of `create_person`), which is null in Preview.

namespace pat::sim::events {

/// What became of one effect.
enum class EffectStatus : std::uint8_t {
    Issued = 0,       // command(s) appended (validate() may still refuse them at the tick boundary)
    Stub = 1,         // the system behind the effect does not exist yet (CatalogEntry::stub): nothing issued
    BadScope = 2,     // the effect runs over a scope of the wrong kind or an empty one
    BadArgument = 3,  // an argument is missing or out of range (no target, an unknown trait, a negative age)
    Unknown = 4,      // no block knows the code
};

[[nodiscard]] const char* effect_status_name(EffectStatus status) noexcept;

/// What the translation reads besides the effect itself.
struct EffectEnv {
    /// The World (required) and the table "registry index -> trait slot" of `add_trait` / `remove_trait`.
    TriggerEnv triggers;
    /// The length of a pregnancy (`conceive`) and the life stages of the portraits (`create_person`).
    /// Null: kDefaultDemography.
    const DemographyRules* demography = nullptr;
    /// The name pools of `create_person` and the ids of their given names (BirthContent::given_name_ids).
    /// Without pools, or with a list of another size, or without `rng`, the new person is unnamed and has
    /// no portrait.
    const NamePools* names = nullptr;
    std::span<const core::StringId> given_name_ids;
    /// The random stream of the run (events.run): two draws per `create_person` when names are given.
    /// Null in Preview.
    core::Pcg32* rng = nullptr;
};

/// The commands of one effect, appended to `out`. `args` are the arguments as the machine passes them (in
/// the order of CatalogEntry::args).
EffectStatus effect_commands(const EffectEnv& env,
                             std::uint16_t code,
                             Scope target,
                             std::span<const Word> args,
                             std::vector<Command>& out);

/// An effect that issued nothing (EffectStatus other than Issued): its position in the buffer and why.
struct EffectIssue {
    std::uint32_t index = 0;
    std::uint16_t code = 0;
    EffectStatus status = EffectStatus::Issued;

    [[nodiscard]] bool operator==(const EffectIssue&) const = default;
};

/// The commands of every effect of a Run (RunResult::commands), in program order. The effects that issued
/// nothing go to `issues` when it is given (the history of the event instance records them, T11/T16).
/// Returns the number of commands appended.
std::size_t effects_to_commands(const EffectEnv& env,
                                const OpBuffer& effects,
                                std::vector<Command>& out,
                                std::vector<EffectIssue>* issues = nullptr);

}  // namespace pat::sim::events
