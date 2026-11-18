#include "pat/sim/events/event_set.hpp"

#include "pat/core/bytes.hpp"
#include "pat/core/hash_stream.hpp"
#include "pat/core/rng.hpp"

#include <algorithm>
#include <cstdint>

namespace pat::sim::events {

std::uint64_t event_key(std::string_view id) noexcept {
    return core::fnv1a64(id);
}

std::int32_t EventSettings::pulse_days(PulseKind kind) const noexcept {
    switch (kind) {
        case PulseKind::Daily:
            return 1;
        case PulseKind::Monthly:
            return pulse_month_days;
        case PulseKind::Quarterly:
            return pulse_quarter_days;
        case PulseKind::Yearly:
            return pulse_year_days;
        case PulseKind::None:
            break;
    }
    return 0;
}

std::uint32_t mtth_ppm(std::int32_t pulse_days, std::int64_t mtth_days) noexcept {
    constexpr std::int64_t kOne = 1'000'000;
    if (pulse_days <= 0 || mtth_days <= 0) {
        return 0;
    }
    // round(10^6 * pulse / mtth) with integers; pulse and mtth are far below 2^31, so no overflow.
    const std::int64_t numerator = kOne * static_cast<std::int64_t>(pulse_days);
    const std::int64_t ppm = (2 * numerator + mtth_days) / (2 * mtth_days);
    return static_cast<std::uint32_t>(std::clamp<std::int64_t>(ppm, 1, kOne));
}

const EventDef* EventSet::find(std::uint64_t key) const noexcept {
    const auto it = std::lower_bound(
        events.begin(), events.end(), key, [](const EventDef& e, std::uint64_t k) { return e.key < k; });
    return it != events.end() && it->key == key ? &*it : nullptr;
}

const EventDef* EventSet::find(std::string_view id) const noexcept {
    const EventDef* e = find(event_key(id));
    return e != nullptr && e->id == id ? e : nullptr;
}

const EventModifierDef* EventSet::find_modifier(std::uint64_t key) const noexcept {
    const auto it = std::lower_bound(modifiers.begin(),
                                     modifiers.end(),
                                     key,
                                     [](const EventModifierDef& m, std::uint64_t k) { return m.key < k; });
    return it != modifiers.end() && it->key == key ? &*it : nullptr;
}

std::string_view EventSet::name_of_key(std::uint64_t key) const noexcept {
    const auto it = std::lower_bound(
        key_names.begin(),
        key_names.end(),
        key,
        [](const std::pair<std::uint64_t, std::string>& e, std::uint64_t k) { return e.first < k; });
    return it != key_names.end() && it->first == key ? std::string_view(it->second) : std::string_view();
}

namespace {

void put_text(core::ByteWriter& out, std::string_view text) {
    out.u32(static_cast<std::uint32_t>(text.size()));
    for (const char c : text) {
        out.u8(static_cast<std::uint8_t>(c));
    }
}

void put_program(core::ByteWriter& out, const std::optional<Program>& program) {
    out.boolean(program.has_value());
    if (program) {
        out.u64(program->hash());
    }
}

void put_event(core::ByteWriter& out, const EventDef& e) {
    out.u64(e.key);
    out.u8(static_cast<std::uint8_t>(e.scope));
    out.u8(static_cast<std::uint8_t>(e.type));
    out.u32(static_cast<std::uint32_t>(e.on.size()));
    for (const OnSub& s : e.on) {
        out.u16(s.action);
        out.u8(s.role);
    }
    out.i32(e.priority);
    out.u8(static_cast<std::uint8_t>(e.urgency));
    out.u8(static_cast<std::uint8_t>(e.display));
    out.u8(static_cast<std::uint8_t>(e.autopause));
    out.boolean(e.demotable);
    out.boolean(e.rumor);
    out.boolean(e.survives_death);
    out.boolean(e.single_option);
    put_text(out, e.group_key);
    out.u8(static_cast<std::uint8_t>(e.once));
    out.i32(e.cooldown_days);
    out.u64(e.cooldown_group);
    out.boolean(e.has_window);
    out.i32(e.window_from);
    out.i32(e.window_to);
    out.boolean(e.anchor);
    out.u64(e.pool);
    out.i32(e.mtth_days);
    for (const std::uint32_t ppm : e.mtth_ppm) {
        out.u32(ppm);
    }
    out.u32(static_cast<std::uint32_t>(e.mtth_modifiers.size()));
    for (const MtthModifier& m : e.mtth_modifiers) {
        out.i32(m.factor_permille);
        out.u64(m.trigger.hash());
    }
    put_program(out, e.chance);
    put_program(out, e.weight);
    put_program(out, e.trigger);
    put_program(out, e.immediate);
    put_program(out, e.after);
    put_program(out, e.speaker);
    out.u32(static_cast<std::uint32_t>(e.options.size()));
    for (const OptionDef& o : e.options) {
        out.u64(o.key);
        put_program(out, o.available);
        put_program(out, o.visible);
        put_program(out, o.ai_chance);
        put_program(out, o.effect);
        out.boolean(o.is_default);
        out.boolean(o.neutral);
        out.u8(o.local_saved);
    }
    out.u8(e.provided_saved);
    out.u32(static_cast<std::uint32_t>(e.saved_names.size()));
    for (const std::string& name : e.saved_names) {
        put_text(out, name);
    }
}

void put_modifier(core::ByteWriter& out, const EventModifierDef& m) {
    out.u64(m.key);
    out.u8(static_cast<std::uint8_t>(m.scope));
    out.i32(m.duration_days);
    out.u8(static_cast<std::uint8_t>(m.stack));
    out.u32(static_cast<std::uint32_t>(m.values.size()));
    for (const auto& [key, value] : m.values) {
        out.u64(key);
        out.i64(value.raw());
    }
}

void put_settings(core::ByteWriter& out, const EventSettings& s) {
    out.i32(s.modal_per_month);
    out.i32(s.modal_per_month_crisis);
    out.i32(s.crisis_phase_from);
    out.i32(s.defer_max);
    out.i32(s.defer_days);
    out.u32(static_cast<std::uint32_t>(s.frequency_choices.size()));
    for (const std::int32_t f : s.frequency_choices) {
        out.i32(f);
    }
    out.boolean(s.frequency_applies_to_all);
    out.i32(s.max_chain_depth);
    out.i32(s.max_timer_chain);
    out.i32(s.abort_disable_months);
    out.i32(s.fire_cap_per_pulse);
    out.i32(s.pulse_month_days);
    out.i32(s.pulse_quarter_days);
    out.i32(s.pulse_year_days);
    out.i32(s.rumor_delay_min_days);
    out.i32(s.rumor_delay_max_days);
    out.i32(s.notification_ring);
    out.i32(s.history_ring);
    out.u32(s.max_trigger_instr);
    out.u32(s.max_effect_instr);
    out.u32(s.max_commands);
    out.u32(s.max_state_ops);
    out.u32(s.max_fanout);
}

}  // namespace

std::vector<std::byte> EventSet::to_bytes() const {
    core::ByteWriter out;
    out.u32(0x31535645);  // "EVS1": bump when this layout changes
    put_settings(out, settings);
    out.u32(static_cast<std::uint32_t>(modifiers.size()));
    for (const EventModifierDef& m : modifiers) {
        put_modifier(out, m);
    }
    out.u32(static_cast<std::uint32_t>(events.size()));
    for (const EventDef& e : events) {
        put_event(out, e);
    }
    return core::to_std_bytes(out.data());
}

std::uint64_t EventSet::hash() const noexcept {
    return core::hash_bytes(to_bytes());
}

}  // namespace pat::sim::events
