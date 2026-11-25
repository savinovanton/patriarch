#include "pat/sim/marriage_tonsure.hpp"

#include "pat/core/assert.hpp"
#include "pat/sim/marriage_check.hpp"
#include "pat/sim/marriage_dowry.hpp"
#include "pat/sim/marriage_envoy.hpp"
#include "pat/sim/marriage_negotiations.hpp"

#include <algorithm>

namespace pat::sim {
namespace {

using Check = core::Result<void, CommandError>;

Check reject(Rejection rejection, const char* detail, std::uint16_t code = 0) {
    return Check::err(CommandError{rejection, detail, code});
}

const family::Marriage*
active_marriage_of_pair(const World& world, CharacterId character, CharacterId spouse) {
    const auto* marriage = world.family_marriage_of(character);
    return marriage != nullptr && marriage->state == family::MarriageState::Active &&
                   ((marriage->groom == character && marriage->bride == spouse) ||
                    (marriage->bride == character && marriage->groom == spouse))
               ? marriage
               : nullptr;
}

DynastyId reaction_house(const World& world, CharacterId character, CharacterId spouse, DynastyId actor) {
    if (const auto* marriage = active_marriage_of_pair(world, character, spouse)) {
        const auto target_house =
            character == marriage->bride ? marriage->bride_house : marriage->groom_house;
        const auto spouse_house =
            character == marriage->bride ? marriage->groom_house : marriage->bride_house;
        return target_house != actor ? target_house : spouse_house;
    }
    const auto target_house = world.characters().dynasty[character.index()];
    return target_house != actor ? target_house : world.characters().dynasty[spouse.index()];
}

// E15 will inspect the broken marriage's treaty here. There is no treaty state in M1.
void treaty_breach_hook([[maybe_unused]] CharacterId character,
                        [[maybe_unused]] CharacterId spouse,
                        [[maybe_unused]] DynastyId actor) {}

}  // namespace

Check validate_tonsure(const World& world, DynastyId actor, const cmd::Tonsure& payload) {
    if (!world.dynasty_mode()) {
        return reject(Rejection::InvalidState, "tonsure requires dynasty mode");
    }
    if (!world.exists(actor)) {
        return reject(Rejection::NotAuthorized, "tonsure requires a house actor");
    }
    if (!world.is_alive(payload.character)) {
        return reject(Rejection::UnknownEntity,
                      "the target is not alive",
                      static_cast<std::uint16_t>(MarriageDenial::NotAlive));
    }
    const auto character = payload.character;
    const auto spouse = world.characters().spouse[character.index()];
    if (world.characters().dynasty[character.index()] != actor &&
        (!world.is_alive(spouse) || world.characters().dynasty[spouse.index()] != actor)) {
        return reject(Rejection::NotAuthorized,
                      "the target is neither a house member nor a member's spouse",
                      static_cast<std::uint16_t>(MarriageDenial::NotYourSpouse));
    }
    if (!family_acting_head(world, actor).valid()) {
        return reject(Rejection::InvalidState,
                      "the actor has no acting head",
                      static_cast<std::uint16_t>(MarriageDenial::NoActingHead));
    }
    if (world.family_regency(actor) != nullptr) {
        // SuccessionDenial::RegencyActive, specification section 7.4.
        return reject(Rejection::InvalidState, "tonsure is forbidden during regency", 4);
    }
    if (world.is_tonsured(character)) {
        return reject(Rejection::InvalidState,
                      "the target is already tonsured",
                      static_cast<std::uint16_t>(MarriageDenial::Tonsured));
    }
    if (std::ranges::find(world.dynasties().head, character) != world.dynasties().head.end()) {
        return reject(Rejection::InvalidState,
                      "a dynasty head cannot be tonsured",
                      static_cast<std::uint16_t>(MarriageDenial::CannotTonsureHead));
    }
    for (const auto& row : world.family().regencies) {
        if (row.regent == character || row.ward == character) {
            return reject(Rejection::InvalidState, "the target has an active regency role");
        }
    }
    for (const auto& row : world.family().disputes) {
        if (std::ranges::any_of(row.claimants, [character](const auto& claimant) {
                return claimant.character == character;
            })) {
            return reject(Rejection::InvalidState, "the target is an active succession claimant");
        }
    }
    for (const auto& row : world.family().land_grants) {
        if (row.holder == character) {
            return reject(Rejection::InvalidState, "the target holds a land grant");
        }
    }
    if (spouse.valid() && !world.can_end_marriage(character)) {
        return reject(Rejection::InvalidState, "the marriage cannot be ended");
    }
    return Check::ok();
}

void apply_tonsure(World& world, const MarriageRules& rules, DynastyId actor, const cmd::Tonsure& payload) {
    const auto character = payload.character;
    const auto spouse = world.characters().spouse[character.index()];
    const auto other_house = spouse.valid() ? reaction_house(world, character, spouse, actor) : DynastyId{};
    if (spouse.valid()) {
        const auto* record = active_marriage_of_pair(world, character, spouse);
        const auto marriage = record != nullptr ? record->id : 0;
        if (marriage != 0) {
            (void)refund_marriage_dowry(world, rules, marriage);
        }
        PAT_VERIFY(world.end_marriage(character));
        if (marriage != 0) {
            auto closed = *world.find_family_marriage(marriage);
            closed.state = family::MarriageState::Closed;
            closed.ended = world.today();
            closed.end_reason = family::MarriageEndReason::Tonsure;
            closed.claim_kop = 0;
            PAT_VERIFY(world.update_family_marriage(marriage, closed));
        }
        treaty_breach_hook(character, spouse, actor);
    }
    // These choices are revocable; unrelated succession settings and testamentary guardians survive.
    const auto succession = world.family().succession;
    for (auto row : succession) {
        if (row.designated_heir == character) {
            row.designated_heir = {};
            PAT_VERIFY(world.set_family_succession(row));
        }
    }
    world.cancel_family_engagements(character, static_cast<std::uint16_t>(MarriageDenial::Tonsured));
    auto status =
        world.family_status(character) != nullptr ? *world.family_status(character) : family::PersonStatus{};
    status.character = character;
    status.flags |= family::kPersonTonsured;
    status.tonsured_on = world.today();
    status.widowed_on = kNoDate;
    PAT_VERIFY(world.set_family_status(status));
    world.add_honor(actor, -rules.consequences.tonsure_honor, kStandingReasonTonsure);
    world.add_unity(
        actor, core::Fixed32::from_int(-rules.consequences.tonsure_unity), kStandingReasonTonsure);
    if (other_house.valid() && other_house != actor) {
        add_family_head_opinion(world,
                                family_acting_head(world, other_house),
                                family_acting_head(world, actor),
                                -rules.consequences.tonsure_opinion,
                                kOpinionReasonTonsuredSpouse);
        PAT_VERIFY(world.record_chronicle_note(other_house, character, spouse, 207));
    }
    PAT_VERIFY(world.record_chronicle_note(actor, character, spouse, 207));
}

}  // namespace pat::sim
