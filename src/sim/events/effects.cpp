#include "pat/sim/events/effects.hpp"

#include "pat/sim/events/character_effects.hpp"
#include "pat/sim/events/realm_effects.hpp"

#include <optional>

namespace pat::sim::events {

const char* effect_status_name(EffectStatus status) noexcept {
    switch (status) {
        case EffectStatus::Issued:
            return "issued";
        case EffectStatus::Stub:
            return "stub";
        case EffectStatus::BadScope:
            return "bad_scope";
        case EffectStatus::BadArgument:
            return "bad_argument";
        case EffectStatus::Unknown:
            return "unknown";
    }
    return "unknown";
}

std::optional<Scope> effect_scope_arg(std::span<const Word> args, std::size_t at, ScopeKind kind) {
    const Scope s = Scope::from_word(effect_arg(args, at));
    if (!s.empty() && s.kind != kind) {
        return std::nullopt;
    }
    return s;
}

core::Fixed32 effect_delta(Word w) {
    return core::clamp(fixed32_from_word(w), -cmd::kMaxVitalDelta, cmd::kMaxVitalDelta);
}

EffectStatus effect_commands(const EffectEnv& env,
                             std::uint16_t code,
                             Scope target,
                             std::span<const Word> args,
                             std::vector<Command>& out) {
    if (env.triggers.world == nullptr) {
        return EffectStatus::Unknown;
    }
    // The blocks in turn (later epics add theirs here).
    if (const std::optional<EffectStatus> s = character_effect_commands(env, code, target, args, out)) {
        return *s;
    }
    if (const std::optional<EffectStatus> s = realm_effect_commands(env, code, target, args, out)) {
        return *s;
    }
    return EffectStatus::Unknown;
}

std::size_t effects_to_commands(const EffectEnv& env,
                                const OpBuffer& effects,
                                std::vector<Command>& out,
                                std::vector<EffectIssue>* issues) {
    const std::size_t before = out.size();
    for (std::size_t i = 0; i < effects.size(); ++i) {
        const OpBuffer::Entry& e = effects.entry(i);
        const EffectStatus status = effect_commands(env, e.code, e.target, effects.args_of(i), out);
        if (status != EffectStatus::Issued && issues != nullptr) {
            issues->push_back(EffectIssue{static_cast<std::uint32_t>(i), e.code, status});
        }
    }
    return out.size() - before;
}

}  // namespace pat::sim::events
