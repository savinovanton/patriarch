// Rule-dependent court invariants (E10-T02, docs/dev/court-model.md section 12); see court/invariants.hpp.

#include "pat/sim/court/invariants.hpp"
#include "pat/sim/court/portfolio.hpp"
#include "pat/sim/court/tables.hpp"
#include "pat/sim/economy/tables.hpp"

#include <cstddef>
#include <cstdint>
#include <format>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace pat::sim {
namespace {

/// Why a seat (type, locus) cannot exist under the rules, or nullptr. `province_seat` is the flag of
/// the office row (vacancies and portfolios have none: their seat rule alone decides).
const char*
seat_problem(const World& world, const OfficeDef& def, std::uint32_t locus, const bool* province_seat) {
    if (def.seats.rule == SeatRule::Fixed) {
        if (province_seat != nullptr && *province_seat) {
            return "a fixed seat is flagged as a province seat";
        }
        return locus < static_cast<std::uint32_t>(def.seats.slots) ? nullptr
                                                                   : "the seat number is out of range";
    }
    if (province_seat != nullptr && !*province_seat) {
        return "a regional seat is not flagged as a province seat";
    }
    const ProvinceId province(locus);
    if (!world.exists(province)) {
        return "the seat names an unknown province";
    }
    if (def.seats.rule == SeatRule::PerCrownProvince && world.economy_present()) {
        const auto& provinces = world.economy().provinces;
        const std::size_t i = province.index();
        if (provinces.owner_kind[i] != static_cast<std::uint8_t>(economy::OwnerKind::Crown)) {
            return "the province of the seat is not the crown's";
        }
        if (provinces.kind[i] >= 32 || (def.seats.kinds & (std::uint32_t{1} << provinces.kind[i])) == 0) {
            return "the province of the seat is not of a kind of the office";
        }
    }
    return nullptr;
}

}  // namespace

std::vector<std::string> check_court_invariants(const World& world, const CourtRules& rules) {
    std::vector<std::string> bad;
    const auto fail = [&bad](std::string message) { bad.push_back(std::move(message)); };
    const court::State& c = world.court();
    const CourtBalance& balance = rules.balance;

    // C-2: offices.
    std::map<court::OfficeTypeId, std::int32_t> per_type;
    std::map<CharacterId, std::int32_t> per_holder;
    for (const court::Office& row : c.offices) {
        const OfficeDef* def = rules.find_office(row.type);
        if (def == nullptr) {
            fail(std::format("C-2: office {} has an unknown type {}", row.id, row.type));
            continue;
        }
        const bool province_seat = (row.flags & court::kOfficeProvinceSeat) != 0;
        if (const char* problem = seat_problem(world, *def, row.locus, &province_seat)) {
            fail(std::format("C-2: office {}: {}", row.id, problem));
        }
        ++per_type[row.type];
        ++per_holder[row.holder];
    }
    for (const auto& [type, count] : per_type) {
        const OfficeDef* def = rules.find_office(type);
        const std::int32_t seats = def->seats.rule == SeatRule::Fixed ? def->seats.slots : def->seats.max;
        if (count > seats) {
            fail(std::format("C-2: type {} has more holders than seats", type));
        }
    }
    for (const auto& [holder, count] : per_holder) {
        if (count > balance.max_offices_per_person) {
            fail(std::format("C-2: character {} holds more than max_offices_per_person", holder.value()));
        }
    }

    // C-4: vacancies.
    for (const court::Vacancy& row : c.vacancies) {
        const OfficeDef* def = rules.find_office(row.type);
        if (def == nullptr) {
            fail(std::format("C-4: vacancy {} has an unknown type {}", row.id, row.type));
            continue;
        }
        if (const char* problem = seat_problem(world, *def, row.locus, nullptr)) {
            fail(std::format("C-4: vacancy {}: {}", row.id, problem));
        }
        if (row.postponed > balance.max_postponements) {
            fail(std::format("C-4: vacancy {} is postponed more than max_postponements", row.id));
        }
    }

    // C-5: petitions per dynasty.
    std::map<DynastyId, std::int32_t> petitions;
    for (const court::Petition& row : c.petitions) {
        if (++petitions[row.petitioner] == balance.max_petitions_per_dynasty + 1) {
            fail(std::format("C-5: dynasty {} has more than max_petitions_per_dynasty",
                             row.petitioner.value()));
        }
    }

    // C-6: servants per role.
    std::map<std::pair<DynastyId, court::ServantRole>, std::int32_t> per_role;
    for (std::size_t i = 0; i < c.servants.size(); ++i) {
        const court::ServantRole role = c.servants.role[i];
        const ServantRoleDef* def = rules.find_servant_role(static_cast<std::uint8_t>(role));
        if (def == nullptr) {
            fail(std::format("C-6: servant {} has an unknown role", c.servants.id[i]));
        } else if (++per_role[{c.servants.dynasty[i], role}] == def->slots + 1) {
            fail(std::format("C-6: dynasty {} has more servants of role {} than slots",
                             c.servants.dynasty[i].value(),
                             static_cast<int>(role)));
        }
    }

    // C-7: favor.
    for (const court::Favor& row : c.favor) {
        if (row.services > balance.services_max) {
            fail(std::format("C-7: dynasty {} has more than services_max", row.dynasty.value()));
        }
        if (row.party != court::Party::None &&
            rules.find_party(static_cast<std::uint8_t>(row.party)) == nullptr) {
            fail(std::format("C-7: dynasty {} belongs to an unknown party", row.dynasty.value()));
        }
    }

    // C-9, C-13, C-16: the types the ring of decisions, the portfolios and the orders name.
    for (const court::Decision& row : c.decisions) {
        if (rules.find_office(row.type) == nullptr) {
            fail(std::format("C-9: decision {} has an unknown type {}", row.seq, row.type));
        }
    }
    for (const court::Portfolio& row : c.portfolios) {
        if (rules.find_office(row.type) == nullptr) {
            fail(std::format("C-13: portfolio ({}, {}) has an unknown type", row.type, row.locus));
        }
    }
    for (const court::PortfolioOrder& row : c.orders) {
        if (rules.find_office(row.type) == nullptr) {
            fail(std::format("C-16: order {} has an unknown type {}", row.seq, row.type));
        }
    }
    return bad;
}

}  // namespace pat::sim
