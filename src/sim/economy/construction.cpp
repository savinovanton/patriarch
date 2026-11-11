#include "pat/sim/economy/construction.hpp"

#include "pat/sim/economy/settlement.hpp"
#include "pat/sim/economy/tables.hpp"
#include "pat/sim/world.hpp"

#include <algorithm>

namespace pat::sim::economy {

const char* refusal_text(ConstructionRefusal refusal) noexcept {
    switch (refusal) {
        case ConstructionRefusal::None:
            return "";
        case ConstructionRefusal::NoEconomy:
            return "the world has no economy";
        case ConstructionRefusal::NoRules:
            return "the simulation has no economy rules";
        case ConstructionRefusal::UnknownProvince:
            return "unknown province";
        case ConstructionRefusal::UnknownBuilding:
            return "unknown building type";
        case ConstructionRefusal::NotOwner:
            return "the actor does not own the province";
        case ConstructionRefusal::NotHeld:
            return "no dynasty holds the province";
        case ConstructionRefusal::WrongKind:
            return "this building cannot stand in a province of this kind";
        case ConstructionRefusal::TooLittleDevelopment:
            return "the province is too little developed for this building";
        case ConstructionRefusal::TooMany:
            return "the province has as many of this building as allowed";
        case ConstructionRefusal::NoFreeSlot:
            return "the province has no free building slot";
        case ConstructionRefusal::NoMoney:
            return "not enough money in the treasury";
        case ConstructionRefusal::NoConstruction:
            return "nothing is being built in that slot";
        case ConstructionRefusal::NotPayer:
            return "only the one who paid may cancel the construction";
    }
    return "";
}

const BuildingDef* find_building(const BuildingRules& rules, std::uint8_t building) noexcept {
    if (building == 0) {
        return nullptr;
    }
    const auto count = static_cast<std::size_t>(
        std::clamp<std::int32_t>(rules.count, 0, static_cast<std::int32_t>(kMaxBuildingTypes)));
    for (std::size_t i = 0; i < count; ++i) {
        if (rules.defs[i].slot == building) {
            return &rules.defs[i];
        }
    }
    return nullptr;
}

namespace {

bool queued(const ConstructionQueue& q, ProvinceId province, std::size_t slot) noexcept {
    for (std::size_t i = 0; i < q.size(); ++i) {
        if (q.province[i] == province && q.slot[i] == slot) {
            return true;
        }
    }
    return false;
}

}  // namespace

std::optional<std::uint8_t> free_slot(const World& world, ProvinceId province) noexcept {
    if (!world.economy_present() || !world.exists(province)) {
        return std::nullopt;
    }
    const EconomyState& e = world.economy();
    const std::size_t p = province.index();
    const std::size_t slots = std::min<std::size_t>(e.provinces.slots_total[p], kMaxSlots);
    for (std::size_t s = 0; s < slots; ++s) {
        if (e.provinces.slot_building[p * kMaxSlots + s] == 0 && !queued(e.construction, province, s)) {
            return static_cast<std::uint8_t>(s);
        }
    }
    return std::nullopt;
}

std::int32_t building_count(const World& world, ProvinceId province, std::uint8_t building) noexcept {
    if (!world.economy_present() || !world.exists(province)) {
        return 0;
    }
    const EconomyState& e = world.economy();
    const std::size_t p = province.index();
    std::int32_t count = 0;
    for (std::size_t s = 0; s < kMaxSlots; ++s) {
        count += e.provinces.slot_building[p * kMaxSlots + s] == building ? 1 : 0;
    }
    for (std::size_t i = 0; i < e.construction.size(); ++i) {
        count += e.construction.province[i] == province && e.construction.building[i] == building ? 1 : 0;
    }
    return count;
}

StartConstructionCheck check_start_construction(const World& world,
                                                const EconomyRules& rules,
                                                DynastyId actor,
                                                ProvinceId province,
                                                std::uint8_t building) noexcept {
    const auto refuse = [](ConstructionRefusal why) {
        return StartConstructionCheck{why, DynastyId(), 0, 0};
    };
    if (!world.economy_present()) {
        return refuse(ConstructionRefusal::NoEconomy);
    }
    if (!world.exists(province)) {
        return refuse(ConstructionRefusal::UnknownProvince);
    }
    const BuildingDef* def = find_building(rules.buildings, building);
    if (def == nullptr) {
        return refuse(ConstructionRefusal::UnknownBuilding);
    }
    const EconomyState& e = world.economy();
    const std::size_t p = province.index();
    const DynastyId owner = world.provinces().owner[p];
    const bool held = e.provinces.owner_kind[p] == static_cast<std::uint8_t>(OwnerKind::Dynasty) &&
                      owner.valid() && world.exists(owner);
    DynastyId payer = owner;
    if (actor.valid()) {
        if (!rules.build_foreign_allowed && !(held && owner == actor)) {
            return refuse(ConstructionRefusal::NotOwner);
        }
        payer = actor;
    } else if (!held) {
        return refuse(ConstructionRefusal::NotHeld);
    }
    const std::uint8_t kind = e.provinces.kind[p];
    if (def->allowed_kinds != 0 && (kind >= kProvinceKindCount || ((def->allowed_kinds >> kind) & 1U) == 0)) {
        return refuse(ConstructionRefusal::WrongKind);
    }
    if (world.provinces().development[p] < def->min_development) {
        return refuse(ConstructionRefusal::TooLittleDevelopment);
    }
    if (building_count(world, province, building) >= def->max_per_province) {
        return refuse(ConstructionRefusal::TooMany);
    }
    const std::optional<std::uint8_t> slot = free_slot(world, province);
    if (!slot) {
        return refuse(ConstructionRefusal::NoFreeSlot);
    }
    if (!can_afford(world, AccountRef::of(payer), def->cost_kop)) {
        return refuse(ConstructionRefusal::NoMoney);
    }
    return StartConstructionCheck{ConstructionRefusal::None, payer, *slot, def->cost_kop};
}

ConstructionRefusal check_cancel_construction(const World& world,
                                              DynastyId actor,
                                              ProvinceId province,
                                              std::uint8_t slot) noexcept {
    if (!world.economy_present()) {
        return ConstructionRefusal::NoEconomy;
    }
    if (!world.exists(province)) {
        return ConstructionRefusal::UnknownProvince;
    }
    const ConstructionQueue& q = world.economy().construction;
    for (std::size_t i = 0; i < q.size(); ++i) {
        if (q.province[i] == province && q.slot[i] == slot) {
            return actor.valid() && actor != q.payer[i] ? ConstructionRefusal::NotPayer
                                                        : ConstructionRefusal::None;
        }
    }
    return ConstructionRefusal::NoConstruction;
}

Kopecks cancel_refund(const BuildingRules& rules, Kopecks paid_kop) noexcept {
    return mul_permille(paid_kop, rules.refund_permille);
}

std::optional<std::uint8_t> start_construction(
    World& world, const EconomyRules& rules, DynastyId actor, ProvinceId province, std::uint8_t building) {
    const StartConstructionCheck check = check_start_construction(world, rules, actor, province, building);
    if (!check.ok()) {
        return std::nullopt;
    }
    const BuildingDef* def = find_building(rules.buildings, building);
    return world.begin_construction(province, building, check.payer, check.cost_kop, def->build_days);
}

std::optional<Kopecks> cancel_construction(
    World& world, const EconomyRules& rules, DynastyId actor, ProvinceId province, std::uint8_t slot) {
    if (check_cancel_construction(world, actor, province, slot) != ConstructionRefusal::None) {
        return std::nullopt;
    }
    return world.abort_construction(province, slot, rules.buildings.refund_permille);
}

}  // namespace pat::sim::economy
