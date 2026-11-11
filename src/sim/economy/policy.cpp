#include "pat/sim/economy/policy.hpp"

#include "pat/sim/economy/tables.hpp"
#include "pat/sim/world.hpp"

#include <span>

namespace pat::sim::economy {

namespace {

std::uint8_t resolve(const World& world,
                     ProvinceId province,
                     std::int32_t default_level,
                     std::span<const std::uint8_t> province_column,
                     std::span<const std::uint8_t> dynasty_column) noexcept {
    const auto fallback = static_cast<std::uint8_t>(default_level);
    if (!world.economy_present() || !world.exists(province)) {
        return fallback;
    }
    const std::size_t p = province.index();
    if (province_column[p] != kPolicyDefault) {
        return province_column[p];
    }
    const DynastyId owner = world.provinces().owner[p];
    if (world.economy().provinces.owner_kind[p] == static_cast<std::uint8_t>(OwnerKind::Dynasty) &&
        owner.valid() && world.exists(owner) && dynasty_column[owner.index()] != kPolicyDefault) {
        return dynasty_column[owner.index()];
    }
    return fallback;
}

}  // namespace

void bind_policy_levels(World& world, const EconomyRules& rules) noexcept {
    (void)world.set_economy_policy_levels(static_cast<std::uint8_t>(rules.tax.level_count),
                                          static_cast<std::uint8_t>(rules.recruit.level_count));
}

std::uint8_t
effective_tax_level(const World& world, const EconomyRules& rules, ProvinceId province) noexcept {
    const EconomyState& e = world.economy();
    return resolve(world, province, rules.tax.default_level, e.provinces.tax_policy, e.dynasties.tax_policy);
}

std::uint8_t
effective_recruit_level(const World& world, const EconomyRules& rules, ProvinceId province) noexcept {
    const EconomyState& e = world.economy();
    return resolve(
        world, province, rules.recruit.default_level, e.provinces.recruit_policy, e.dynasties.recruit_policy);
}

}  // namespace pat::sim::economy
