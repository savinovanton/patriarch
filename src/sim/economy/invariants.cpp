#include "pat/sim/economy/invariants.hpp"

#include "pat/sim/economy/production.hpp"
#include "pat/sim/economy/tables.hpp"
#include "pat/sim/world.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>

namespace pat::sim::economy {

namespace {

/// Which building numbers (`BuildingDef::slot`) the rules define.
std::array<bool, 256> defined_buildings(const BuildingRules& rules) noexcept {
    std::array<bool, 256> known{};
    const auto count = static_cast<std::size_t>(
        std::clamp<std::int32_t>(rules.count, 0, static_cast<std::int32_t>(kMaxBuildingTypes)));
    for (std::size_t i = 0; i < count; ++i) {
        known[rules.defs[i].slot] = rules.defs[i].slot != 0;
    }
    return known;
}

}  // namespace

std::vector<std::string> check_rules_invariants(const World& world, const EconomyRules& rules) {
    std::vector<std::string> bad;
    if (!world.economy_present()) {
        return bad;
    }
    const EconomyState& e = world.economy();
    const ProvinceEconomy& pe = e.provinces;
    const std::size_t provinces = std::min(world.provinces().size(), pe.size());
    const std::array<bool, 256> known_building = defined_buildings(rules.buildings);

    // ---- E-R1: provinces ---------------------------------------------------------------------
    for (std::size_t p = 0; p < provinces; ++p) {
        if (pe.tax_policy[p] >= rules.tax.level_count) {
            bad.push_back(std::format("province {} has the tax policy level {}, the rules define {}",
                                      p,
                                      static_cast<unsigned>(pe.tax_policy[p]),
                                      rules.tax.level_count));
        }
        if (pe.recruit_policy[p] >= rules.recruit.level_count) {
            bad.push_back(std::format("province {} has the recruit policy level {}, the rules define {}",
                                      p,
                                      static_cast<unsigned>(pe.recruit_policy[p]),
                                      rules.recruit.level_count));
        }
        const std::size_t slots = std::min<std::size_t>(pe.slots_total[p], kMaxSlots);
        for (std::size_t slot = 0; slot < slots; ++slot) {
            const std::uint8_t building = pe.slot_building[p * kMaxSlots + slot];
            if (building != 0 && !known_building[building]) {
                bad.push_back(
                    std::format("province {} slot {} holds the building {}, which the rules do not define",
                                p,
                                slot,
                                static_cast<unsigned>(building)));
            }
        }
        if (pe.last_slot_date[p] != kNoDate) {
            std::array<std::uint32_t, kGroupCount> sizes{};
            for (std::size_t g = 0; g < kGroupCount; ++g) {
                sizes[g] = e.groups.size[p * kGroupCount + g];
            }
            const Quantity need = compute_need(rules.consumption, sizes, kGrainGood);
            const Quantity cap =
                std::min<Quantity>(kStockMax.raw(), saturating_mul(need, rules.consumption.stock_cap_months));
            if (pe.grain_stock[p].raw() > cap) {
                bad.push_back(std::format("province {} holds {} raw units of grain, above the cap of {}",
                                          p,
                                          pe.grain_stock[p].raw(),
                                          cap));
            }
        }
    }

    // ---- E-R1: the construction queue --------------------------------------------------------
    const ConstructionQueue& q = e.construction;
    for (std::size_t i = 0; i < q.size(); ++i) {
        if (q.building[i] == 0 || !known_building[q.building[i]]) {
            bad.push_back(
                std::format("construction entry {} builds the building {}, which the rules do not define",
                            i,
                            static_cast<unsigned>(q.building[i])));
        }
    }

    // ---- E-L1 (rules part): loans per dynasty ------------------------------------------------
    const LoanTable& loans = e.loans;
    std::vector<std::uint32_t> per_borrower(world.dynasties().size(), 0);
    for (std::size_t i = 0; i < loans.size(); ++i) {
        if (loans.borrower[i].valid() && loans.borrower[i].index() < per_borrower.size()) {
            ++per_borrower[loans.borrower[i].index()];
        }
    }
    for (std::size_t d = 0; d < per_borrower.size(); ++d) {
        if (per_borrower[d] > static_cast<std::uint32_t>(std::max(0, rules.loans.max_loans_per_dynasty))) {
            bad.push_back(std::format("dynasty {} holds {} loans, the rules allow {}",
                                      d,
                                      per_borrower[d],
                                      rules.loans.max_loans_per_dynasty));
        }
    }
    return bad;
}

}  // namespace pat::sim::economy
