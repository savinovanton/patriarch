#pragma once

#include "pat/sim/economy/money.hpp"
#include "pat/sim/economy/rules.hpp"
#include "pat/sim/entity_ids.hpp"

#include <cstdint>
#include <optional>

// Construction of buildings (E9-T16, docs/dev/economy-model.md sections 4.6, 8 and 9 F15).
//
// A province has `slots_total` building slots (a column of the world). StartConstruction picks the
// lowest free slot, charges the cost to the payer in ONE posting (code Construction) and puts an entry
// into the queue of the world; the entry reserves the slot from the first day. The system
// `economy.construction` (systems.hpp) finishes the entries whose date has come: the building then
// stands in `ProvinceEconomy::slot_building`, which is what the settlement charges the upkeep by
// (settlement.hpp: building_upkeep_by_account; a building under construction costs no upkeep) and what
// the effects of E9-T17 will read. CancelConstruction returns a share of the paid cost, `refund_permille`
// of the rules, to the payer and frees the slot.
//
// WHO MAY BUILD WHERE (open question Q4, #139, decided by the owner: only in one's own provinces):
// the actor must own the province; the system (invalid actor) may build in any province that a dynasty
// holds, the holder paying. The rule `build_foreign_allowed` of the rules lifts the demand of
// ownership: then any dynasty may build in any province, paying itself (the column `payer` of the queue
// is what keeps that possible without a change of the layout). The crown, the Church and free people
// hold no account of a dynasty, so a province they hold has no payer in M1 unless the actor pays.
//
// The checks are PURE functions of the world and the rules: the commands validate through them and the
// UI may ask the same question ("can I build it here, and if not, why") without a command.

namespace pat::sim {
class World;
}

namespace pat::sim::economy {

/// Why a construction command is refused. Stable order, not saved.
enum class ConstructionRefusal : std::uint8_t {
    None = 0,
    NoEconomy,             ///< the world has no economy
    NoRules,               ///< the simulation was not given the economy rules (install_economy_systems)
    UnknownProvince,       ///< no such province
    UnknownBuilding,       ///< no building type with that number in the rules
    NotOwner,              ///< the actor does not own the province (and `build_foreign_allowed` is off)
    NotHeld,               ///< the system cannot name a payer: no dynasty holds the province
    WrongKind,             ///< the building may not stand in a province of this kind
    TooLittleDevelopment,  ///< the province is less developed than the building demands
    TooMany,               ///< the province has as many of the type as `max_per_province` allows
    NoFreeSlot,            ///< every slot holds a building or a construction (or the province has none)
    NoMoney,               ///< the payer cannot afford the cost (can_afford)
    NoConstruction,        ///< cancel: nothing is being built in that slot
    NotPayer,              ///< cancel: the actor is not the one who paid
};

/// A fixed English text for a refusal (the `detail` of the rejection; the UI maps the enum to its own
/// localised text). "" for None.
[[nodiscard]] const char* refusal_text(ConstructionRefusal refusal) noexcept;

/// The type with that number, or null (0 is "no building").
[[nodiscard]] const BuildingDef* find_building(const BuildingRules& rules, std::uint8_t building) noexcept;

/// The lowest slot of the province that holds no building and no construction; nullopt when there is
/// none or the province is unknown.
[[nodiscard]] std::optional<std::uint8_t> free_slot(const World& world, ProvinceId province) noexcept;

/// How many buildings of the type stand in the province, finished and under construction together.
[[nodiscard]] std::int32_t
building_count(const World& world, ProvinceId province, std::uint8_t building) noexcept;

/// The verdict of check_start_construction: on success the payer, the slot that will be taken and the
/// cost; otherwise the first refusal.
struct StartConstructionCheck {
    ConstructionRefusal refusal = ConstructionRefusal::None;
    DynastyId payer;
    std::uint8_t slot = 0;
    Kopecks cost_kop = 0;

    [[nodiscard]] bool ok() const noexcept { return refusal == ConstructionRefusal::None; }
};

/// Everything StartConstruction demands, in this order: an economy; a known province and building;
/// who may build there (NotOwner/NotHeld, see the top of the file); the requirements of the type (kind,
/// development, number per province); a free slot; the money. `actor` is the dynasty that issues the
/// command, invalid for the system.
[[nodiscard]] StartConstructionCheck check_start_construction(const World& world,
                                                              const EconomyRules& rules,
                                                              DynastyId actor,
                                                              ProvinceId province,
                                                              std::uint8_t building) noexcept;

/// What CancelConstruction demands: an economy, a known province, an entry in that slot, and an actor
/// that is the payer of the entry (the system may cancel any).
[[nodiscard]] ConstructionRefusal check_cancel_construction(const World& world,
                                                            DynastyId actor,
                                                            ProvinceId province,
                                                            std::uint8_t slot) noexcept;

/// The share of `paid_kop` that a cancellation returns (`refund_permille` of the rules, truncated).
[[nodiscard]] Kopecks cancel_refund(const BuildingRules& rules, Kopecks paid_kop) noexcept;

/// The effect of StartConstruction: re-checks (so a stale command does nothing) and calls
/// World::begin_construction. Returns the slot taken.
std::optional<std::uint8_t> start_construction(
    World& world, const EconomyRules& rules, DynastyId actor, ProvinceId province, std::uint8_t building);

/// The effect of CancelConstruction: re-checks and calls World::abort_construction with the refund
/// share of the rules. Returns the refund.
std::optional<Kopecks> cancel_construction(
    World& world, const EconomyRules& rules, DynastyId actor, ProvinceId province, std::uint8_t slot);

}  // namespace pat::sim::economy
