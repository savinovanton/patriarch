#include "pat/sim/events/realm_triggers.hpp"

#include "pat/core/fixed.hpp"
#include "pat/sim/economy/loans.hpp"
#include "pat/sim/economy/tables.hpp"
#include "pat/sim/economy/types.hpp"
#include "pat/sim/economy/unrest.hpp"
#include "pat/sim/events/state.hpp"

#include <cstddef>

namespace pat::sim::events {

namespace {

[[nodiscard]] TriggerResult yes(bool v) {
    return TriggerResult{v ? Word{1} : Word{0}, true};
}

[[nodiscard]] TriggerResult number(Word w) {
    return TriggerResult{w, true};
}

constexpr TriggerResult kNoData{0, false};

/// A name argument: its 64-bit key (what the compiler pushes for a literal name).
[[nodiscard]] EventKey key_arg(std::span<const Word> args, std::size_t at) {
    return at < args.size() ? static_cast<EventKey>(args[at]) : EventKey{0};
}

/// A reference / enum / suffix argument: the whole number the compiler pushes.
[[nodiscard]] std::int32_t index_arg(std::span<const Word> args, std::size_t at) {
    return at < args.size() ? int_from_word(args[at]) : -1;
}

[[nodiscard]] Scope realm_scope() noexcept {
    return Scope::make(ScopeKind::Realm, 0);
}

/// The province of the scope with economy rows, or nullopt (another kind, unknown, no economy).
[[nodiscard]] std::optional<std::size_t> economy_province(const World& world, Scope scope) {
    if (scope.kind != ScopeKind::Province || !world.exists(ProvinceId{scope.id}) ||
        !world.economy_present()) {
        return std::nullopt;
    }
    const std::size_t i = ProvinceId{scope.id}.index();
    return i < world.economy().provinces.size() ? std::optional<std::size_t>(i) : std::nullopt;
}

[[nodiscard]] std::uint8_t building_slot(const TriggerEnv& env, std::int32_t index) {
    if (env.building_slots.empty()) {
        return index > 0 && index <= 0xFF ? static_cast<std::uint8_t>(index) : std::uint8_t{0};
    }
    return index >= 0 && static_cast<std::size_t>(index) < env.building_slots.size()
               ? env.building_slots[static_cast<std::size_t>(index)]
               : std::uint8_t{0};
}

/// A finished building of this slot stands in the province (construction sites do not count).
[[nodiscard]] bool has_building(const World& world, std::size_t province, std::uint8_t slot) {
    if (slot == 0) {
        return false;
    }
    const std::vector<std::uint8_t>& slots = world.economy().provinces.slot_building;
    for (std::size_t s = 0; s < economy::kMaxSlots; ++s) {
        const std::size_t at = province * economy::kMaxSlots + s;
        if (at < slots.size() && slots[at] == slot) {
            return true;
        }
    }
    return false;
}

/// The house that holds the scope: the house of a person, the dynasty itself, the owner of a province.
[[nodiscard]] DynastyId holder_of(const World& world, Scope scope) {
    if (scope.kind == ScopeKind::Province) {
        const ProvinceId p{scope.id};
        return world.exists(p) ? world.provinces().owner[p.index()] : DynastyId{};
    }
    return dynasty_of_scope(world, scope);
}

[[nodiscard]] bool player_controlled(const World& world, Scope scope) {
    const DynastyId d = holder_of(world, scope);
    if (!d.valid()) {
        return false;
    }
    const Meta& meta = world.event_state().meta;
    for (std::size_t i = 0; i < meta.player_count && i < meta.players.size(); ++i) {
        if (meta.players[i] == d) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool has_quota(const World& world, DynastyId dynasty, std::int32_t kind) {
    const economy::QuotaTable& q = world.economy().quotas;
    for (std::size_t i = 0; i < q.dynasty.size(); ++i) {
        if (q.dynasty[i] == dynasty && (kind <= 0 || static_cast<std::int32_t>(q.kind[i]) == kind - 1)) {
            return true;
        }
    }
    return false;
}

/// The group of a `group_size.<group>` suffix, or nullopt.
[[nodiscard]] std::optional<economy::Group> group_arg(std::span<const Word> args) {
    const std::int32_t g = index_arg(args, 0);
    if (g < 0 || static_cast<std::size_t>(g) >= economy::kGroupCount) {
        return std::nullopt;
    }
    return static_cast<economy::Group>(g);
}

}  // namespace

std::optional<TriggerResult>
eval_realm_predicate(const TriggerEnv& env, Scope scope, std::uint16_t code, std::span<const Word> args) {
    const World& world = *env.world;
    switch (code) {
        case kTrigHasFlag:
            return yes(world.has_event_flag(scope, key_arg(args, 0)));
        case kTrigHasGlobalFlag:
            return yes(world.has_event_flag(realm_scope(), key_arg(args, 0)));
        case kTrigHasModifier:
            return yes(world.has_event_modifier(scope, key_arg(args, 0)));
        case kTrigHasFired: {
            // `once` and cooldowns keep a row while they hold; a chain keeps its row while it runs.
            const EventKey key = key_arg(args, 0);
            return yes(world.find_event_cooldown(scope, key) != nullptr ||
                       world.find_event_chain(scope, key) != nullptr);
        }
        case kTrigHasCooldown: {
            const EventKey key = key_arg(args, 0);
            return yes(key != 0 && (world.event_on_cooldown(scope, key) ||
                                    world.event_on_cooldown(scope, key | kCooldownGroupBit)));
        }
        case kTrigRandomChance:
            return kNoData;  // compiled to RAND_PERMILLE: a draw from the stream of the event, never asked
                             // here
        case kTrigHasBuilding: {
            const std::optional<std::size_t> p = economy_province(world, scope);
            if (!p) {
                return world.economy_present() ? yes(false) : kNoData;
            }
            return yes(has_building(world, *p, building_slot(env, index_arg(args, 0))));
        }
        case kTrigIsPlayerControlled:
            return yes(player_controlled(world, scope));
        case kTrigHasQuota: {
            if (!world.economy_present()) {
                return kNoData;
            }
            const DynastyId d =
                scope.kind == ScopeKind::Dynasty ? dynasty_of_scope(world, scope) : DynastyId{};
            return yes(d.valid() && has_quota(world, d, args.empty() ? 0 : index_arg(args, 0)));
        }
        default:
            return std::nullopt;
    }
}

std::optional<TriggerResult>
eval_realm_value(const TriggerEnv& env, Scope scope, std::uint16_t code, std::span<const Word> args) {
    const World& world = *env.world;
    const auto province_column = [&](const std::vector<core::Fixed32>& column) -> TriggerResult {
        const std::optional<std::size_t> p = economy_province(world, scope);
        if (!p) {
            return world.economy_present() ? number(0) : kNoData;
        }
        return number(word_from_fixed32(column[*p]));
    };
    switch (code) {
        case kValChainStep: {
            const Chain* chain = world.find_event_chain(scope, key_arg(args, 0));
            return number(chain != nullptr ? word_from_int(chain->step) : 0);
        }
        case kValVariable:
            return number(word_from_fixed(world.event_var(scope, key_arg(args, 0))));
        case kValGlobalVariable:
            return number(word_from_fixed(world.event_var(realm_scope(), key_arg(args, 0))));
        case kValCurrentYear:
            return number(word_from_int(world.today().year()));
        case kValCurrentMonth:
            return number(word_from_int(world.today().month()));
        case kValDate:
            return number(word_from_int(world.today().days()));
        case kValUnrest:
            return province_column(world.economy().provinces.unrest);
        case kValControl:
            return province_column(world.economy().provinces.control);
        case kValCorruption:
            return province_column(world.economy().provinces.corruption);
        case kValPopulation: {
            if (scope.kind != ScopeKind::Province || !world.exists(ProvinceId{scope.id})) {
                return number(0);
            }
            const ProvinceId p{scope.id};
            if (world.economy_present()) {
                return number(word_from_whole(static_cast<std::int64_t>(world.population_persons(p))));
            }
            // Without the economy only the cache in thousands exists.
            const core::Fixed32 thousands = world.provinces().population[p.index()];
            return number(word_from_whole((static_cast<std::int64_t>(thousands.raw()) * 1000) >>
                                          core::Fixed32::frac_bits));
        }
        case kValRecruitPool: {
            const std::optional<std::size_t> p = economy_province(world, scope);
            if (!p) {
                return world.economy_present() ? number(0) : kNoData;
            }
            return number(word_from_whole(world.economy().provinces.recruit_pool[*p]));
        }
        case kValUnrestState: {
            if (env.economy == nullptr) {
                return kNoData;  // the thresholds are rules of the content
            }
            const std::optional<std::size_t> p = economy_province(world, scope);
            if (!p) {
                return world.economy_present() ? number(0) : kNoData;
            }
            const economy::UnrestState state =
                economy::unrest_state(env.economy->unrest, world.economy().provinces.unrest[*p]);
            return number(word_from_int(static_cast<std::int32_t>(state)));
        }
        case kValDevelopment: {
            if (scope.kind != ScopeKind::Province || !world.exists(ProvinceId{scope.id})) {
                return number(0);
            }
            return number(word_from_int(world.provinces().development[ProvinceId{scope.id}.index()]));
        }
        case kValGroupSize:
        case kValGroupMood: {
            const std::optional<std::size_t> p = economy_province(world, scope);
            if (!p) {
                return world.economy_present() ? number(0) : kNoData;
            }
            const std::optional<economy::Group> group = group_arg(args);
            if (!group) {
                return number(0);
            }
            const std::size_t row = economy::group_row(*p, *group);
            const economy::PopulationGroups& groups = world.economy().groups;
            if (row >= groups.rows()) {
                return number(0);
            }
            return number(code == kValGroupSize ? word_from_whole(groups.size[row])
                                                : word_from_fixed32(groups.mood[row]));
        }
        case kValDebt: {
            if (!world.economy_present()) {
                return kNoData;
            }
            const DynastyId d =
                scope.kind == ScopeKind::Dynasty ? dynasty_of_scope(world, scope) : DynastyId{};
            if (!d.valid()) {
                return number(0);
            }
            return number(word_from_fixed(core::Fixed64::from_ratio(economy::dynasty_debt(world, d), 100)));
        }
        default:
            return std::nullopt;
    }
}

}  // namespace pat::sim::events
