#include "pat/sim/economy/modifiers.hpp"

#include "pat/core/assert.hpp"
#include "pat/core/fixed.hpp"
#include "pat/sim/economy/construction.hpp"
#include "pat/sim/economy/policy.hpp"
#include "pat/sim/economy/tables.hpp"
#include "pat/sim/world.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <functional>

namespace pat::sim::economy {

namespace {

constexpr std::int64_t kOne = core::Fixed32::one_raw;

/// The ready buildings of a province, in slot order: pointers into the rules (null slots are left out).
struct ReadyBuildings {
    std::array<const BuildingDef*, kMaxSlots> defs{};
    std::size_t count = 0;
};

ReadyBuildings ready_buildings(const World& world, const EconomyRules& rules, ProvinceId province) {
    ReadyBuildings out;
    if (!world.economy_present() || !world.exists(province)) {
        return out;
    }
    const EconomyState& e = world.economy();
    const std::size_t p = province.index();
    // the whole grid: a cell past slots_total is empty (invariant), so the flag of slots_total is not an
    // input
    for (std::size_t s = 0; s < kMaxSlots; ++s) {
        const std::uint8_t type = e.provinces.slot_building[p * kMaxSlots + s];
        if (type == 0) {
            continue;
        }
        if (const BuildingDef* def = find_building(rules.buildings, type)) {
            out.defs[out.count++] = def;
        }
    }
    return out;
}

/// The sums of the effects before they are clamped to the range of their unit.
struct RawSums {
    std::array<std::int32_t, kMaxGoods> output{};  ///< sum of (multiplier - 1000)
    std::int32_t control = 0;
    std::int32_t oversight = 0;
    std::int32_t recruits = 0;  ///< sum of (multiplier - 1000)
    std::int32_t famine = 0;
};

RawSums sum_effects(const ReadyBuildings& ready) {
    RawSums sums;
    for (std::size_t i = 0; i < ready.count; ++i) {
        const BuildingDef& def = *ready.defs[i];
        for (std::size_t g = 0; g < kMaxGoods; ++g) {
            sums.output[g] += def.output_permille[g] - kPermilleOne;
        }
        sums.control += def.control_points;
        sums.oversight += def.oversight_permille;
        sums.recruits += def.recruit_permille - kPermilleOne;
        sums.famine += def.famine_protect_permille;
    }
    return sums;
}

std::int16_t clamped16(std::int32_t value, std::int32_t lo, std::int32_t hi) noexcept {
    return static_cast<std::int16_t>(std::clamp(value, lo, hi));
}

void mix(std::uint64_t& hash, std::int64_t value) noexcept {
    hash ^= static_cast<std::uint64_t>(value);
    hash *= 1099511628211ULL;
}

}  // namespace

// ---- cache ---------------------------------------------------------------------------------------

void ModifierCache::store(std::size_t province, const ProvinceModifiers& modifiers) {
    if (province >= clean_.size()) {
        values_.resize(province + 1);
        clean_.resize(province + 1, 0);
    }
    if (clean_[province] == 0) {
        ++clean_count_;
    }
    clean_[province] = 1;
    values_[province] = modifiers;
}

void ModifierCache::mark(std::size_t province) noexcept {
    if (province < clean_.size() && clean_[province] != 0) {
        clean_[province] = 0;
        --clean_count_;
    }
}

void ModifierCache::mark_all() noexcept {
    std::fill(clean_.begin(), clean_.end(), std::uint8_t{0});
    clean_count_ = 0;
}

// ---- the direct computation ----------------------------------------------------------------------

std::uint64_t modifier_rules_stamp(const EconomyRules& rules) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    mix(hash, rules.buildings.count);
    for (std::size_t i = 0; i < static_cast<std::size_t>(rules.buildings.count); ++i) {
        const BuildingDef& def = rules.buildings.defs[i];
        mix(hash, def.slot);
        for (const std::int32_t m : def.output_permille) {
            mix(hash, m);
        }
        mix(hash, def.control_points);
        mix(hash, def.oversight_permille);
        mix(hash, def.recruit_permille);
        mix(hash, def.famine_protect_permille);
    }
    mix(hash, rules.tax.default_level);
    mix(hash, rules.recruit.default_level);
    return hash != 0 ? hash : 1;
}

ProvinceModifiers compute_province_modifiers(const World& world,
                                             const EconomyRules& rules,
                                             const AdministratorSource* administrators,
                                             ProvinceId province) {
    ProvinceModifiers out;
    out.tax_level = effective_tax_level(world, rules, province);
    out.recruit_level = effective_recruit_level(world, rules, province);
    if (!world.economy_present() || !world.exists(province)) {
        return out;
    }

    const RawSums sums = sum_effects(ready_buildings(world, rules, province));
    for (std::size_t g = 0; g < kMaxGoods; ++g) {
        out.output_permille[g] = clamped16(kPermilleOne + sums.output[g], 0, kMaxMultiplierPermille);
    }
    out.control_points = clamped16(sums.control, -32768, 32767);
    out.oversight_permille = clamped16(sums.oversight, 0, kPermilleOne);
    out.recruit_permille = clamped16(kPermilleOne + sums.recruits, 0, kMaxMultiplierPermille);
    out.famine_protect_permille = clamped16(sums.famine, 0, kPermilleOne);

    // the profile: the neutral one when nobody (alive) governs, exactly as administration_input did
    const CharacterId administrator = world.economy().provinces.administrator[province.index()];
    if (administrator.valid() && world.is_alive(administrator)) {
        const AdministratorProfile profile = administrators != nullptr
                                                 ? administrators->profile(world, administrator)
                                                 : administrator_profile(world, administrator);
        out.stewardship = static_cast<std::uint8_t>(std::clamp(profile.stewardship, 0, kProfileMax));
        out.integrity = static_cast<std::uint8_t>(std::clamp(profile.integrity, 0, kProfileMax));
    }
    return out;
}

ProvinceModifiers province_modifiers(const World& world,
                                     const EconomyRules& rules,
                                     const AdministratorSource* administrators,
                                     ProvinceId province) {
    const ModifierCache& cache = world.modifier_cache();
    if (world.economy_present() && world.exists(province) && cache.clean(province.index())) {
        return cache.value(province.index());
    }
    return compute_province_modifiers(world, rules, administrators, province);
}

bool refresh_province_modifiers(World& world,
                                const EconomyRules& rules,
                                const AdministratorSource* administrators,
                                ProvinceId province) {
    if (!world.economy_present() || !world.exists(province) ||
        world.modifier_cache().clean(province.index())) {
        return false;
    }
    world.modifier_cache_mut().store(province.index(),
                                     compute_province_modifiers(world, rules, administrators, province));
    return true;
}

std::size_t
refresh_modifier_cache(World& world, const EconomyRules& rules, const AdministratorSource* administrators) {
    if (!world.economy_present()) {
        return 0;
    }
    const std::size_t count = world.provinces().size();
    if (world.modifier_cache().clean_count() >= count) {
        return 0;
    }
    std::size_t done = 0;
    for (std::size_t p = 0; p < count; ++p) {
        if (refresh_province_modifiers(
                world, rules, administrators, ProvinceId(static_cast<std::uint32_t>(p)))) {
            ++done;
        }
    }
    return done;
}

std::size_t
rebuild_modifier_cache(World& world, const EconomyRules& rules, const AdministratorSource* administrators) {
    world.modifier_cache_mut().mark_all();
    world.modifier_cache_mut().set_stamp(modifier_rules_stamp(rules));
    return refresh_modifier_cache(world, rules, administrators);
}

// ---- bounds --------------------------------------------------------------------------------------

std::int32_t max_building_recruit_permille(const EconomyRules& rules) noexcept {
    // The best legal set: fill the slots with the types of the largest gain, each up to its limit.
    std::array<std::int32_t, kMaxBuildingTypes> gains{};
    std::array<std::int32_t, kMaxBuildingTypes> limits{};
    std::size_t types = 0;
    for (std::size_t i = 0; i < static_cast<std::size_t>(rules.buildings.count) && types < kMaxBuildingTypes;
         ++i) {
        const BuildingDef& def = rules.buildings.defs[i];
        if (def.recruit_permille > kPermilleOne) {
            gains[types] = def.recruit_permille - kPermilleOne;
            limits[types] = std::max(1, def.max_per_province);
            ++types;
        }
    }
    // the types by descending gain (insertion sort: at most 16 entries, and GCC mistrusts std::sort on a
    // prefix)
    std::array<std::size_t, kMaxBuildingTypes> order{};
    for (std::size_t i = 0; i < types; ++i) {
        std::size_t j = i;
        for (; j > 0 && gains[order[j - 1]] < gains[i]; --j) {
            order[j] = order[j - 1];
        }
        order[j] = i;
    }
    std::int64_t total = kPermilleOne;
    std::int32_t room = static_cast<std::int32_t>(kMaxSlots);
    for (std::size_t k = 0; k < types && room > 0; ++k) {
        const std::size_t i = order[k];
        const std::int32_t take = std::min(room, limits[i]);
        total += static_cast<std::int64_t>(take) * gains[i];
        room -= take;
    }
    return static_cast<std::int32_t>(std::min<std::int64_t>(total, kMaxMultiplierPermille));
}

// ---- explanations --------------------------------------------------------------------------------

namespace {

struct ExplainSpec {
    std::uint16_t root = 0;
    std::uint16_t base = 0;  ///< 0 = no base term
    std::uint16_t limit = 0;
    Unit unit = Unit::Permille;
    std::int64_t base_value = 0;
    std::int64_t lo = 0;
    std::int64_t hi = 0;
    bool multiplier = false;  ///< the factor of a contribution is the multiplier of the building
    std::int64_t scale = 1;   ///< raw units per unit of the effect (Points: one point = kOne)
};

}  // namespace

Breakdown explain_modifier(
    const World& world, const EconomyRules& rules, ProvinceId province, ModifierKind kind, std::size_t good) {
    Breakdown out;
    if (!world.economy_present() || !world.exists(province) ||
        (kind == ModifierKind::Output && good >= kMaxGoods)) {
        return out;
    }
    ExplainSpec spec;
    std::function<std::int64_t(const BuildingDef&)> delta;  // the contribution of a building, in effect units
    std::function<std::int32_t(const BuildingDef&)> factor;
    switch (kind) {
        case ModifierKind::Output:
            spec = {term::kOutputBuildingsTotal,
                    term::kOutputBuildingsBase,
                    term::kOutputBuildingsLimit,
                    Unit::Permille,
                    kPermilleOne,
                    0,
                    kMaxMultiplierPermille,
                    true,
                    1};
            delta = [good](const BuildingDef& d) {
                return std::int64_t{d.output_permille[good]} - kPermilleOne;
            };
            factor = [good](const BuildingDef& d) { return d.output_permille[good]; };
            break;
        case ModifierKind::Control:
            spec = {term::kControlBuildingsTotal, 0, 0, Unit::Points, 0, -32768, 32767, false, kOne};
            delta = [](const BuildingDef& d) { return std::int64_t{d.control_points}; };
            break;
        case ModifierKind::Oversight:
            spec = {term::kCorruptionOversightTotal,
                    0,
                    term::kCorruptionOversightLimit,
                    Unit::Permille,
                    0,
                    0,
                    kPermilleOne,
                    false,
                    1};
            delta = [](const BuildingDef& d) { return std::int64_t{d.oversight_permille}; };
            break;
        case ModifierKind::Recruits:
            spec = {term::kRecruitBuildingsTotal,
                    term::kRecruitBuildingsBase,
                    term::kRecruitBuildingsLimit,
                    Unit::Permille,
                    kPermilleOne,
                    0,
                    kMaxMultiplierPermille,
                    true,
                    1};
            delta = [](const BuildingDef& d) { return std::int64_t{d.recruit_permille} - kPermilleOne; };
            factor = [](const BuildingDef& d) { return d.recruit_permille; };
            break;
        case ModifierKind::FamineProtection:
            spec = {term::kWealthFamineTotal,
                    0,
                    term::kWealthFamineLimit,
                    Unit::Permille,
                    0,
                    0,
                    kPermilleOne,
                    false,
                    1};
            delta = [](const BuildingDef& d) { return std::int64_t{d.famine_protect_permille}; };
            break;
    }

    // the contributions, in slot order (the order does not change the sum)
    const EconomyState& e = world.economy();
    const std::size_t p = province.index();
    std::int64_t sum = spec.base_value;
    struct Contribution {
        std::uint8_t type;
        std::int64_t value;
        std::int32_t factor;
    };
    std::array<Contribution, kMaxSlots> contributions{};
    std::size_t count = 0;
    for (std::size_t s = 0; s < kMaxSlots; ++s) {
        const std::uint8_t type = e.provinces.slot_building[p * kMaxSlots + s];
        const BuildingDef* def = type != 0 ? find_building(rules.buildings, type) : nullptr;
        if (def == nullptr) {
            continue;
        }
        const std::int64_t value = delta(*def);
        sum += value;
        if (value != 0) {
            contributions[count++] = {type, value * spec.scale, spec.multiplier ? factor(*def) : 0};
        }
    }
    const std::int64_t total = std::clamp(sum, spec.lo, spec.hi);

    out.begin(spec.root, spec.unit, total * spec.scale);
    if (spec.base != 0) {
        (void)out.add_term(kRootNode, spec.base, spec.base_value);
    }
    for (std::size_t i = 0; i < count; ++i) {
        (void)out.add_term(kRootNode,
                           term::building_term(contributions[i].type),
                           contributions[i].value,
                           contributions[i].factor);
    }
    if (total != sum && spec.limit != 0) {
        (void)out.add_term(kRootNode, spec.limit, (total - sum) * spec.scale);
    }
    PAT_ASSERT(out.is_consistent(), "the modifier breakdown must add up to the modifier");
    return out;
}

}  // namespace pat::sim::economy
