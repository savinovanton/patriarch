#include "pat/sim/economy/administrator.hpp"

#include "pat/core/assert.hpp"
#include "pat/sim/economy/modifiers.hpp"
#include "pat/sim/economy/money.hpp"
#include "pat/sim/economy/taxes.hpp"
#include "pat/sim/world.hpp"

#include <algorithm>

namespace pat::sim::economy {

using core::Fixed32;

namespace {

constexpr std::int64_t kOne = Fixed32::one_raw;
constexpr std::int64_t kControlMaxRaw = 100 * kOne;
constexpr std::int32_t kCorruptionMaxPermille = 1000;

AdministratorProfile clamped(AdministratorProfile profile) noexcept {
    profile.stewardship = std::clamp(profile.stewardship, 0, kProfileMax);
    profile.integrity = std::clamp(profile.integrity, 0, kProfileMax);
    return profile;
}

/// permille of a share (1000 = 1.0) as the smallest Fixed32 raw value that reads back as that many
/// per-mille through `corruption_permille` (which truncates): a column that reached its target is exact.
Fixed32 share_of_permille(std::int32_t permille) noexcept {
    const std::int64_t clamped_value = std::clamp(permille, 0, kCorruptionMaxPermille);
    return Fixed32::from_raw(static_cast<std::int32_t>((clamped_value * kOne + 999) / 1000));
}

}  // namespace

// ---- profile -------------------------------------------------------------------------------------

AdministratorProfile administrator_profile(const World& /*world*/, CharacterId /*character*/) {
    // E7 maps Stewardship 0..20 (x5) and the traits of honesty and greed to 0..100 here; the stub is
    // the neutral profile, which changes nothing.
    return AdministratorProfile{};
}

AdministratorProfile StubAdministratorSource::profile(const World& world, CharacterId character) const {
    return administrator_profile(world, character);
}

// ---- control state -------------------------------------------------------------------------------

ControlState control_state(const ControlRules& rules, Fixed32 control) noexcept {
    const std::int64_t raw = control.raw();
    if (raw < static_cast<std::int64_t>(rules.anarchy_threshold) * kOne) {
        return ControlState::Anarchy;
    }
    if (raw < static_cast<std::int64_t>(rules.low_threshold) * kOne) {
        return ControlState::Low;
    }
    return ControlState::Normal;
}

// ---- F7 ------------------------------------------------------------------------------------------

Fixed32
compute_control_target(const ControlRules& rules, const AdministrationInput& input, Breakdown* breakdown) {
    const AdministratorProfile profile = clamped(input.profile);
    const std::int64_t base = static_cast<std::int64_t>(rules.control_base) * kOne;
    const std::int64_t administrator =
        mul_permille(static_cast<std::int64_t>(profile.stewardship - rules.administrator_neutral) * kOne,
                     rules.steward_gain_permille);
    const std::int64_t buildings = input.building_control.raw();
    const std::int64_t disorder =
        -mul_permille(std::max<std::int64_t>(0, input.unrest.raw()), rules.unrest_penalty_permille);
    const std::int64_t sum = base + administrator + buildings + disorder;
    const std::int64_t target = std::clamp<std::int64_t>(sum, 0, kControlMaxRaw);

    if (breakdown != nullptr) {
        breakdown->begin(term::kControlTarget, Unit::Points, target);
        (void)breakdown->add_term(kRootNode, term::kControlBase, base);
        if (administrator != 0) {
            (void)breakdown->add_term(
                kRootNode, term::kControlAdministrator, administrator, rules.steward_gain_permille);
        }
        if (buildings != 0) {
            (void)breakdown->add_term(kRootNode, term::kControlBuildings, buildings);
        }
        if (disorder != 0) {
            (void)breakdown->add_term(
                kRootNode, term::kControlUnrest, disorder, rules.unrest_penalty_permille);
        }
        if (target != sum) {
            (void)breakdown->add_term(kRootNode, term::kControlLimit, target - sum);
        }
        PAT_ASSERT(breakdown->is_consistent(), "the control breakdown must add up to the target");
    }
    return Fixed32::from_raw(static_cast<std::int32_t>(target));
}

Fixed32
compute_corruption_target(const ControlRules& rules, const AdministrationInput& input, Breakdown* breakdown) {
    const AdministratorProfile profile = clamped(input.profile);
    const std::int32_t base = rules.corruption_base_permille;
    const std::int32_t integrity =
        -(profile.integrity - rules.administrator_neutral) * rules.integrity_gain_permille;
    const std::int32_t oversight = -std::clamp(input.oversight_permille, 0, kCorruptionMaxPermille);
    // per-mille per point of control below the low threshold; control is Q16.16 points
    std::int32_t low_control = 0;
    if (control_state(rules, input.control) != ControlState::Normal) {
        const std::int64_t gap_raw =
            static_cast<std::int64_t>(rules.low_threshold) * kOne - input.control.raw();
        low_control = static_cast<std::int32_t>(std::min<std::int64_t>(
            kCorruptionMaxPermille, gap_raw * rules.low_control_corruption_permille / kOne));
    }
    const std::int32_t sum = base + integrity + oversight + low_control;
    const std::int32_t target = std::clamp(sum, 0, kCorruptionMaxPermille);

    if (breakdown != nullptr) {
        breakdown->begin(term::kCorruptionTarget, Unit::Permille, target);
        (void)breakdown->add_term(kRootNode, term::kCorruptionBase, base);
        if (integrity != 0) {
            (void)breakdown->add_term(
                kRootNode, term::kCorruptionIntegrity, integrity, rules.integrity_gain_permille);
        }
        if (oversight != 0) {
            (void)breakdown->add_term(kRootNode, term::kCorruptionOversight, oversight);
        }
        if (low_control != 0) {
            (void)breakdown->add_term(
                kRootNode, term::kCorruptionLowControl, low_control, rules.low_control_corruption_permille);
        }
        if (target != sum) {
            (void)breakdown->add_term(kRootNode, term::kCorruptionLimit, target - sum);
        }
        PAT_ASSERT(breakdown->is_consistent(), "the corruption breakdown must add up to the target");
    }
    return share_of_permille(target);
}

Fixed32 approach(Fixed32 current, Fixed32 target, std::int32_t inertia_permille) noexcept {
    const std::int64_t gap = static_cast<std::int64_t>(target.raw()) - current.raw();
    std::int64_t step = mul_permille(gap, std::clamp(inertia_permille, 0, kPermilleOne));
    if (step == 0 && gap != 0 && inertia_permille > 0) {
        step = gap > 0 ? 1 : -1;
    }
    return Fixed32::from_raw(static_cast<std::int32_t>(current.raw() + step));
}

Administration compute_administration(const ControlRules& rules, const AdministrationInput& input) {
    Administration out;
    out.control_target = compute_control_target(rules, input);
    out.corruption_target = compute_corruption_target(rules, input);
    // a column that was written out of its range is pulled back into it here
    const Fixed32 control = Fixed32::from_raw(
        static_cast<std::int32_t>(std::clamp<std::int64_t>(input.control.raw(), 0, kControlMaxRaw)));
    const Fixed32 corruption =
        Fixed32::from_raw(std::clamp<std::int32_t>(input.corruption.raw(), 0, Fixed32::one_raw));
    out.control = approach(control, out.control_target, rules.control_inertia_permille);
    out.corruption = approach(corruption, out.corruption_target, rules.corruption_inertia_permille);
    PAT_ASSERT(out.control.raw() >= 0 && out.control.raw() <= kControlMaxRaw,
               "control must stay within 0..100");
    PAT_ASSERT(out.corruption.raw() >= 0 && out.corruption.raw() <= Fixed32::one_raw,
               "corruption must stay within 0..1");
    return out;
}

// ---- province --------------------------------------------------------------------------------------

AdministrationInput administration_input(const World& world,
                                         const AdministratorSource* source,
                                         ProvinceId province,
                                         const ProvinceModifiers* modifiers) {
    AdministrationInput in;
    if (!world.economy_present() || !world.exists(province)) {
        return in;
    }
    const EconomyState& e = world.economy();
    const std::size_t p = province.index();
    in.control = e.provinces.control[p];
    in.corruption = e.provinces.corruption[p];
    in.unrest = e.provinces.unrest[p];
    if (modifiers != nullptr) {
        in.profile = modifiers->profile();
        in.building_control = Fixed32::from_int(modifiers->control_points);
        in.oversight_permille = modifiers->oversight_permille;
        return in;
    }
    const CharacterId administrator = e.provinces.administrator[p];
    if (administrator.valid() && world.is_alive(administrator)) {
        in.profile = clamped(source != nullptr ? source->profile(world, administrator)
                                               : administrator_profile(world, administrator));
    }
    return in;
}

void apply_province_administration(World& world, ProvinceId province, const Administration& result) {
    if (!world.economy_present() || !world.exists(province)) {
        return;
    }
    EconomyColumnsMut columns = world.economy_columns_mut();
    columns.control[province.index()] = result.control;
    columns.corruption[province.index()] = result.corruption;
}

Administration step_province_administration(World& world,
                                            const EconomyRules& rules,
                                            const AdministratorSource* source,
                                            ProvinceId province) {
    if (!world.economy_present() || !world.exists(province)) {
        return {};
    }
    const ProvinceModifiers modifiers = province_modifiers(world, rules, source, province);
    const Administration result =
        compute_administration(rules.control, administration_input(world, source, province, &modifiers));
    apply_province_administration(world, province, result);
    return result;
}

DynastyId incident_dynasty(const World& world, ProvinceId province) {
    if (!world.economy_present() || !world.exists(province)) {
        return DynastyId();
    }
    const std::size_t p = province.index();
    return static_cast<OwnerKind>(world.economy().provinces.owner_kind[p]) == OwnerKind::Dynasty
               ? world.provinces().owner[p]
               : DynastyId();
}

bool record_anarchy(World& world, ProvinceId province, bool anarchy) {
    if (!anarchy || !world.economy_present() || !world.exists(province) ||
        !income_recipient(world, province)) {
        return false;
    }
    const auto whole_points =
        static_cast<std::int32_t>(world.economy().provinces.control[province.index()].to_int());
    return world
        .log_incident(IncidentKind::Anarchy, province, incident_dynasty(world, province), whole_points)
        .has_value();
}

// ---- explanations ----------------------------------------------------------------------------------

Breakdown explain_control(const EconomyRules& rules, const AdministrationInput& input) {
    Breakdown out;
    (void)compute_control_target(rules.control, input, &out);
    return out;
}

Breakdown explain_corruption(const EconomyRules& rules, const AdministrationInput& input) {
    Breakdown out;
    (void)compute_corruption_target(rules.control, input, &out);
    return out;
}

}  // namespace pat::sim::economy
