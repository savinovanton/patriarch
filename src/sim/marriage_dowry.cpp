#include "pat/sim/marriage_dowry.hpp"

#include "pat/core/assert.hpp"
#include "pat/sim/marriage_rules.hpp"
#include "pat/sim/world.hpp"

#include <algorithm>

namespace pat::sim {

economy::Kopecks
refund_marriage_dowry(World& world, const MarriageRules& rules, family::MarriageId marriage) {
    const auto* row = world.find_family_marriage(marriage);
    if (!world.economy_present() || row == nullptr || row->state != family::MarriageState::Active ||
        row->groom_house == row->bride_house || !world.is_alive(row->groom) || !world.is_alive(row->bride)) {
        return 0;
    }
    const auto& people = world.characters();
    if (people.spouse[row->groom.index()] != row->bride || people.spouse[row->bride.index()] != row->groom) {
        return 0;
    }

    auto updated = *row;
    const auto quota =
        std::clamp(economy::mul_permille(row->dowry_kop, rules.consequences.tonsure_refund_permille),
                   economy::Kopecks{0},
                   row->dowry_kop);
    if (row->refunded_kop >= quota) {
        return 0;
    }
    const auto returned = world.transfer(economy::AccountRef::of(row->groom_house),
                                         economy::AccountRef::of(row->bride_house),
                                         economy::LedgerCode::DowryOut,
                                         economy::LedgerCode::DowryIn,
                                         quota - row->refunded_kop);
    if (returned != 0) {
        updated.refunded_kop += returned;
        PAT_VERIFY(world.update_family_marriage(marriage, updated));
    }
    return returned;
}

}  // namespace pat::sim
