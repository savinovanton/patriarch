#include "pat/sim/economy/invariants.hpp"

#include "pat/sim/economy/construction.hpp"
#include "pat/sim/economy/modifiers.hpp"
#include "pat/sim/economy/production.hpp"
#include "pat/sim/economy/recruits.hpp"
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

    // ---- E-R1: policies of the dynasties, the levels the world was given -------------------------
    for (std::size_t d = 0; d < e.dynasties.size(); ++d) {
        if (e.dynasties.tax_policy[d] != kPolicyDefault &&
            e.dynasties.tax_policy[d] >= rules.tax.level_count) {
            bad.push_back(std::format("dynasty {} has the tax policy level {}, the rules define {}",
                                      d,
                                      static_cast<unsigned>(e.dynasties.tax_policy[d]),
                                      rules.tax.level_count));
        }
        if (e.dynasties.recruit_policy[d] != kPolicyDefault &&
            e.dynasties.recruit_policy[d] >= rules.recruit.level_count) {
            bad.push_back(std::format("dynasty {} has the recruit policy level {}, the rules define {}",
                                      d,
                                      static_cast<unsigned>(e.dynasties.recruit_policy[d]),
                                      rules.recruit.level_count));
        }
    }
    if (e.meta.tax_levels != 0 && e.meta.tax_levels != rules.tax.level_count) {
        bad.push_back(std::format("the world accepts {} tax policy levels, the rules define {}",
                                  static_cast<unsigned>(e.meta.tax_levels),
                                  rules.tax.level_count));
    }
    if (e.meta.recruit_levels != 0 && e.meta.recruit_levels != rules.recruit.level_count) {
        bad.push_back(std::format("the world accepts {} recruit policy levels, the rules define {}",
                                  static_cast<unsigned>(e.meta.recruit_levels),
                                  rules.recruit.level_count));
    }

    // ---- E-R1: provinces ---------------------------------------------------------------------
    for (std::size_t p = 0; p < provinces; ++p) {
        if (pe.tax_policy[p] != kPolicyDefault && pe.tax_policy[p] >= rules.tax.level_count) {
            bad.push_back(std::format("province {} has the tax policy level {}, the rules define {}",
                                      p,
                                      static_cast<unsigned>(pe.tax_policy[p]),
                                      rules.tax.level_count));
        }
        if (pe.recruit_policy[p] != kPolicyDefault && pe.recruit_policy[p] >= rules.recruit.level_count) {
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
            // F11: the pool never exceeds what the people can give under the heaviest policy and full
            // control. (Not the cap of the policy of the day: a command or a fall of control between
            // two slots may leave the pool above it until the next slot cuts it.)
            const std::uint32_t ceiling = recruit_ceiling(rules, sizes);
            if (pe.recruit_pool[p] > ceiling) {
                bad.push_back(std::format("province {} has a recruit pool of {}, above the ceiling of {}",
                                          p,
                                          pe.recruit_pool[p],
                                          ceiling));
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
            continue;
        }
        // E9-T16: an entry is what StartConstruction made of the rules: the time and the price of the type.
        const BuildingDef* def = find_building(rules.buildings, q.building[i]);
        if (def != nullptr && q.finish_date[i] - q.start_date[i] != def->build_days) {
            bad.push_back(std::format("construction entry {} takes {} days, the rules give {} to building {}",
                                      i,
                                      q.finish_date[i] - q.start_date[i],
                                      def->build_days,
                                      static_cast<unsigned>(q.building[i])));
        }
        if (def != nullptr && q.paid_kop[i] != def->cost_kop) {
            bad.push_back(
                std::format("construction entry {} was paid {} kopecks, the rules price building {} at {}",
                            i,
                            q.paid_kop[i],
                            static_cast<unsigned>(q.building[i]),
                            def->cost_kop));
        }
    }
    // E9-T16: per province no type exceeds `max_per_province` (finished and queued together) and every
    // type stands only in the kinds it allows.
    for (std::size_t p = 0; p < provinces; ++p) {
        const ProvinceId province(static_cast<std::uint32_t>(p));
        std::array<std::int32_t, 256> count{};
        for (std::size_t slot = 0; slot < std::min<std::size_t>(pe.slots_total[p], kMaxSlots); ++slot) {
            ++count[pe.slot_building[p * kMaxSlots + slot]];
        }
        for (std::size_t i = 0; i < q.size(); ++i) {
            if (q.province[i] == province) {
                ++count[q.building[i]];
            }
        }
        for (std::size_t b = 1; b < count.size(); ++b) {
            const BuildingDef* def =
                count[b] != 0 ? find_building(rules.buildings, static_cast<std::uint8_t>(b)) : nullptr;
            if (def == nullptr) {
                continue;  // an unknown type is reported above (slots) and in the queue
            }
            if (count[b] > def->max_per_province) {
                bad.push_back(std::format("province {} holds {} of building {}, the rules allow {}",
                                          p,
                                          count[b],
                                          b,
                                          def->max_per_province));
            }
            const std::uint8_t kind = pe.kind[p];
            if (def->allowed_kinds != 0 &&
                (kind >= kProvinceKindCount || ((def->allowed_kinds >> kind) & 1U) == 0)) {
                bad.push_back(std::format("province {} of kind {} holds building {}, which the rules do not "
                                          "allow in such a province",
                                          p,
                                          static_cast<unsigned>(kind),
                                          b));
            }
        }
    }

    // ---- E-R3 (E9-T17): the cache of the province modifiers equals the direct computation ----------
    // Every CLEAN entry must be what compute_province_modifiers gives now (the profile of the
    // administrator is left out: it depends on the source the cache was made with); a difference means
    // an input changed without a dirty flag. Dirty entries are not checked: they are recomputed on use.
    for (std::size_t p = 0; p < provinces; ++p) {
        if (!world.modifier_cache().clean(p)) {
            continue;
        }
        const ProvinceModifiers direct =
            compute_province_modifiers(world, rules, nullptr, ProvinceId(static_cast<std::uint32_t>(p)));
        if (!world.modifier_cache().value(p).same_without_profile(direct)) {
            bad.push_back(std::format("province {} has a modifier cache entry that differs from the direct "
                                      "computation (an input changed without a dirty flag)",
                                      p));
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
