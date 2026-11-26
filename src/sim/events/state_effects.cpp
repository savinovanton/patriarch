#include "pat/sim/events/state_effects.hpp"

#include "pat/sim/events/dispatch.hpp"
#include "pat/sim/world.hpp"

#include <algorithm>
#include <limits>

namespace pat::sim::events {

namespace {

constexpr Scope kGlobal{ScopeKind::Realm, 0};

[[nodiscard]] EventKey key_arg(std::span<const Word> args, std::size_t i) noexcept {
    return i < args.size() ? static_cast<EventKey>(args[i]) : 0;
}

[[nodiscard]] core::Fixed64 number_arg(std::span<const Word> args, std::size_t i) noexcept {
    return i < args.size() ? fixed_from_word(args[i]) : core::Fixed64{};
}

[[nodiscard]] std::int64_t whole_days(core::Fixed64 v) noexcept {
    return std::clamp<std::int64_t>(v.to_int(), 0, kMaxStateDays);
}

/// `days`, `months`, `years` at args[first ...] in days (months and years in the lengths of the settings).
[[nodiscard]] std::int32_t
duration_arg(const EventSettings& settings, std::span<const Word> args, std::size_t first) noexcept {
    const std::int64_t days = whole_days(number_arg(args, first)) +
                              whole_days(number_arg(args, first + 1)) * settings.pulse_month_days +
                              whole_days(number_arg(args, first + 2)) * settings.pulse_year_days;
    return static_cast<std::int32_t>(std::clamp<std::int64_t>(days, 0, kMaxStateDays));
}

/// The end date of a row that lasts `days` (0: no end).
[[nodiscard]] Date end_after(Date today, std::int32_t days) noexcept {
    return days > 0 ? today + days : kNoDate;
}

[[nodiscard]] std::int32_t scope_id_or_none(Word word) noexcept {
    const Scope scope = Scope::from_word(word);
    return scope.empty() ? -1 : static_cast<std::int32_t>(scope.id);
}

}  // namespace

std::uint32_t saved_name_key(std::string_view name) noexcept {
    const auto key = static_cast<std::uint32_t>(event_key(name) & 0xFFFFFFFFU);
    return key != 0 ? key : 1U;
}

std::uint8_t pack_saved(const EventDef& event, const SavedSlots& slots, SavedScopes& out) {
    out = SavedScopes{};
    std::uint8_t count = 0;
    const std::size_t named = std::min(event.saved_names.size(), slots.size());
    for (std::size_t i = 0; i < named; ++i) {
        if (slots[i].empty()) {
            continue;
        }
        const std::uint32_t key = saved_name_key(event.saved_names[i]);
        const bool taken = std::any_of(
            out.begin(), out.begin() + count, [&](const SavedScope& row) { return row.name_key == key; });
        if (!taken) {
            out[count++] = SavedScope{key, slots[i]};
        }
    }
    return count;
}

SavedSlots unpack_saved(const EventDef& event, const SavedScopes& rows, std::uint8_t count) {
    SavedSlots slots{};
    const std::size_t n = std::min<std::size_t>(count, rows.size());
    const std::size_t named = std::min(event.saved_names.size(), slots.size());
    for (std::size_t j = 0; j < named; ++j) {
        const std::uint32_t key = saved_name_key(event.saved_names[j]);
        for (std::size_t i = 0; i < n; ++i) {
            if (rows[i].name_key == key) {
                slots[j] = rows[i].scope;
                break;
            }
        }
    }
    return slots;
}

EffectStatus apply_state_op(const StateOpContext& context,
                            std::uint16_t code,
                            Scope target,
                            std::span<const Word> args,
                            StateOpReport& report) {
    if (context.world == nullptr || context.set == nullptr) {
        return EffectStatus::Unknown;
    }
    World& world = *context.world;
    const State& state = world.event_state();
    const EventSettings& settings = context.set->settings;
    const Date today = world.today();

    // A refused row: a full table is reported (StateFull), anything else is a bad argument.
    const auto refused = [&](std::size_t size, std::size_t bound) {
        if (size >= bound) {
            report.state_full = true;
        }
        return EffectStatus::BadArgument;
    };
    const auto scoped = [&](Scope scope) { return world.event_scope_valid(scope); };

    switch (code) {
        case kStTriggerEvent: {
            const EventKey key = key_arg(args, 0);
            const EventDef* def = context.set->find(key);
            if (def == nullptr) {
                return EffectStatus::BadArgument;
            }
            const Word scope_word = args.size() > 4 ? args[4] : 0;
            const Scope scope = scope_word != 0 ? Scope::from_word(scope_word) : target;
            if (scope.empty()) {
                return EffectStatus::BadScope;
            }
            const std::int32_t days = duration_arg(settings, args, 1);
            if (days == 0) {
                ChainStart start;
                start.event = key;
                start.scope = scope;
                start.force = args.size() > 5 && args[5] != 0;
                start.respect_cooldown = args.size() > 6 && args[6] != 0;
                report.chained.push_back(start);
                return EffectStatus::Issued;
            }
            if (scope.kind != def->scope || !scoped(scope)) {
                return EffectStatus::BadScope;
            }
            Timer timer;
            timer.due = today + days;
            timer.event_key = key;
            timer.scope = scope;
            timer.depth = static_cast<std::uint8_t>(std::min<int>(context.depth + 1, 255));
            if (context.event != nullptr) {
                timer.saved_count = pack_saved(*context.event, context.saved, timer.saved);
            }
            if (!world.add_event_timer(timer)) {
                return refused(state.timers.size(), kMaxTimers);
            }
            return EffectStatus::Issued;
        }
        case kStSetFlag:
        case kStSetGlobalFlag: {
            const Scope scope = code == kStSetGlobalFlag ? kGlobal : target;
            if (!scoped(scope)) {
                return EffectStatus::BadScope;
            }
            const Date expires =
                code == kStSetFlag ? end_after(today, duration_arg(settings, args, 1)) : kNoDate;
            if (!world.set_event_flag(scope, key_arg(args, 0), expires)) {
                return refused(state.flags.size(), kMaxFlags);
            }
            return EffectStatus::Issued;
        }
        case kStRemoveFlag:
        case kStRemoveGlobalFlag: {
            const Scope scope = code == kStRemoveGlobalFlag ? kGlobal : target;
            if (!scoped(scope)) {
                return EffectStatus::BadScope;
            }
            (void)world.clear_event_flag(scope, key_arg(args, 0));  // removing an absent flag is no error
            return EffectStatus::Issued;
        }
        case kStSetVariable:
        case kStChangeVariable: {
            if (!scoped(target)) {
                return EffectStatus::BadScope;
            }
            const EventKey key = key_arg(args, 0);
            core::Fixed64 value = number_arg(args, 1);
            if (code == kStChangeVariable) {
                // add, multiply, min, max in this order; the defaults leave the value as it is (section 11.3)
                value = saturating_add(world.event_var(target, key), number_arg(args, 1));
                value = saturating_mul(value, args.size() > 2 ? number_arg(args, 2) : core::Fixed64::one());
                if (args.size() > 3) {
                    value = std::min(value, number_arg(args, 3));
                }
                if (args.size() > 4) {
                    value = std::max(value, number_arg(args, 4));
                }
            }
            if (!world.set_event_var(target, key, value)) {
                return refused(state.vars.size(), kMaxVars);
            }
            return EffectStatus::Issued;
        }
        case kStAddModifier: {
            const EventKey key = key_arg(args, 0);
            const EventModifierDef* def = context.set->find_modifier(key);
            if (def == nullptr) {
                return EffectStatus::BadArgument;
            }
            if (!scoped(target) || (def->scope != ScopeKind::None && def->scope != target.kind)) {
                return EffectStatus::BadScope;
            }
            const std::int32_t days = duration_arg(settings, args, 1);
            const Date expires = end_after(today, days > 0 ? days : def->duration_days);
            Modifier row{target, key, today, expires};
            if (world.has_event_modifier(target, key)) {
                const Modifier* old = world.find_event_modifier(target, key);
                if (def->stack == StackMode::Ignore) {
                    return EffectStatus::Issued;  // the running modifier stays as it is
                }
                if (def->stack == StackMode::Refresh) {
                    row.added = old->added;  // the same modifier, a new end
                }
            }
            if (!world.set_event_modifier(row)) {
                return refused(state.modifiers.size(), kMaxModifiers);
            }
            return EffectStatus::Issued;
        }
        case kStRemoveModifier:
            if (!scoped(target)) {
                return EffectStatus::BadScope;
            }
            (void)world.clear_event_modifier(target, key_arg(args, 0));
            return EffectStatus::Issued;
        case kStStartChain:
        case kStSetChainStep: {
            if (!scoped(target)) {
                return EffectStatus::BadScope;
            }
            const EventKey key = key_arg(args, 0);
            Chain row{target, key, 1, today, today};
            if (code == kStSetChainStep) {
                row.step =
                    static_cast<std::uint8_t>(std::clamp<std::int64_t>(number_arg(args, 1).to_int(), 0, 255));
                if (const Chain* old = world.find_event_chain(target, key)) {
                    row.started = old->started;
                }
            }
            if (!world.set_event_chain(row)) {
                return refused(state.chains.size(), kMaxChains);
            }
            return EffectStatus::Issued;
        }
        case kStEndChain:
            if (!scoped(target)) {
                return EffectStatus::BadScope;
            }
            (void)world.clear_event_chain(target, key_arg(args, 0));
            return EffectStatus::Issued;
        case kStNotify: {
            const DynastyId recipient = recipient_of(world, target);
            if (!recipient.valid()) {
                return EffectStatus::BadScope;
            }
            if (!is_player(state.meta, recipient)) {
                return EffectStatus::Issued;  // the journal is the player's (section 10.6); a house of the AI
                                              // reads none
            }
            JournalEntry entry;
            entry.date = today;
            entry.recipient = recipient;
            entry.kind = JournalKind::Notify;
            entry.urgency =
                static_cast<std::uint8_t>(std::clamp<std::int64_t>(number_arg(args, 3).to_int(), 0, 2));
            entry.key = key_arg(args, 0);
            entry.a = args.size() > 1 ? scope_id_or_none(args[1]) : -1;
            entry.b = args.size() > 2 ? scope_id_or_none(args[2]) : -1;
            if (!world.append_event_journal(entry)) {
                return EffectStatus::BadArgument;
            }
            return EffectStatus::Issued;
        }
        default:
            return EffectStatus::Unknown;
    }
}

void apply_state_ops(const StateOpContext& context, const OpBuffer& ops, StateOpReport& report) {
    for (std::size_t i = 0; i < ops.size(); ++i) {
        const OpBuffer::Entry& e = ops.entry(i);
        const EffectStatus status = apply_state_op(context, e.code, e.target, ops.args_of(i), report);
        if (status == EffectStatus::Issued) {
            ++report.applied;
        } else {
            report.issues.push_back(EffectIssue{static_cast<std::uint32_t>(i), e.code, status});
        }
    }
}

}  // namespace pat::sim::events
