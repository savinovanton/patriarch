#include "pat/sim/marriage_weddings.hpp"

#include "pat/core/assert.hpp"
#include "pat/sim/marriage_envoy.hpp"
#include "pat/sim/marriage_negotiations.hpp"
#include "pat/sim/marriage_score.hpp"

#include <algorithm>
#include <cstdint>

namespace pat::sim {
namespace {

using Check = core::Result<void, CommandError>;

Check reject(Rejection rejection, const char* detail, MarriageDenial denial) {
    return Check::err(CommandError{rejection, detail, static_cast<std::uint16_t>(denial)});
}

Check invalid_state(const char* detail) {
    return Check::err(CommandError{Rejection::InvalidState, detail, 0});
}

void note_pair(World& world,
               DynastyId groom_house,
               DynastyId bride_house,
               CharacterId groom,
               CharacterId bride,
               std::int32_t note) {
    PAT_VERIFY(world.record_chronicle_note(groom_house, groom, bride, note));
    if (bride_house != groom_house) {
        PAT_VERIFY(world.record_chronicle_note(bride_house, groom, bride, note));
    }
}

MarriageDenials wedding_denials(const World& world,
                                const MarriageRules& rules,
                                const family::Betrothal& row,
                                const FaithPolicy* faith) {
    return check_marriage(world,
                          rules,
                          row.groom,
                          row.bride,
                          {.dispensation = row.dispensation != 0, .actor = {}, .ignore_betrothal = row.id},
                          faith);
}

bool houses_changed(const World& world, const family::Betrothal& row) {
    return !world.exists(row.groom_house) || !world.exists(row.bride_house) ||
           world.characters().dynasty[row.groom.index()] != row.groom_house ||
           world.characters().dynasty[row.bride.index()] != row.bride_house;
}

Check wedding_writable(const World& world, const family::Betrothal& row) {
    if (!world.can_marry(row.groom, row.bride)) {
        return invalid_state("the wedding cannot update the spouse history");
    }
    if (!world.has_family_marriage_capacity()) {
        return reject(
            Rejection::InvalidState, "the marriage record table is full", MarriageDenial::TableFull);
    }
    for (const auto& marriage : world.family().marriages) {
        if (marriage.state == family::MarriageState::Active &&
            (marriage.groom == row.groom || marriage.bride == row.bride)) {
            return invalid_state("a previous marriage still awaits settlement");
        }
        if (marriage.state == family::MarriageState::Widowed && marriage.bride == row.bride &&
            marriage.claim_kop != 0) {
            return invalid_state("the widow's existing claim requires settlement before this wedding");
        }
    }
    if (row.claim_marriage != 0) {
        const auto* claim = world.find_family_marriage(row.claim_marriage);
        if (claim == nullptr || claim->bride != row.bride || claim->claim_kop != 0) {
            return invalid_state("the betrothal refers to an unsettled widow's claim");
        }
    }
    return Check::ok();
}

bool settlement_needed(const family::Marriage& marriage, CharacterId deceased) {
    return (marriage.state == family::MarriageState::Active &&
            (marriage.groom == deceased || marriage.bride == deceased)) ||
           (marriage.state == family::MarriageState::Widowed && marriage.bride == deceased);
}

void clear_mourning(World& world, CharacterId person) {
    if (const auto* status = world.family_status(person);
        status != nullptr && status->widowed_on != kNoDate) {
        auto updated = *status;
        updated.widowed_on = kNoDate;
        PAT_VERIFY(world.set_family_status(updated));
    }
}

}  // namespace

Check validate_betroth(const World& world,
                       const MarriageRules& rules,
                       const cmd::Betroth& payload,
                       const FaithPolicy* faith) {
    const auto denials =
        check_marriage(world,
                       rules,
                       payload.groom,
                       payload.bride,
                       {.as_betrothal = true, .dispensation = payload.dispensation, .actor = {}},
                       faith);
    if (!denials.empty()) {
        return reject(Rejection::InvalidState, "the betrothal parties are not eligible", denials.front());
    }
    const auto row = make_family_betrothal(world,
                                           rules,
                                           payload.groom,
                                           payload.bride,
                                           payload.dowry_kop,
                                           payload.dowry_source,
                                           payload.dispensation,
                                           family::BetrothalOrigin::Command);
    if (!row) {
        return Check::err(CommandError{Rejection::InvalidArgument, "invalid betrothal terms or dates", 0});
    }
    if (!world.can_create_family_betrothal(*row)) {
        return reject(Rejection::InvalidState, "the betrothal cannot be stored", MarriageDenial::TableFull);
    }
    return Check::ok();
}

std::optional<family::BetrothalBreakReason> wedding_break_reason(const World& world,
                                                                 const MarriageRules& rules,
                                                                 const family::Betrothal& row,
                                                                 const FaithPolicy* faith) {
    if (!world.is_alive(row.groom) || !world.is_alive(row.bride)) {
        return family::BetrothalBreakReason::PartyDied;
    }
    if (houses_changed(world, row)) {
        return family::BetrothalBreakReason::Ineligible;
    }
    auto denials = wedding_denials(world, rules, row, faith);
    if (world.today() < row.wedding_not_before) {
        std::erase_if(denials, [](MarriageDenial denial) {
            return denial == MarriageDenial::GroomTooYoung || denial == MarriageDenial::BrideTooYoung;
        });
    }
    if (!denials.empty()) {
        return family::BetrothalBreakReason::Ineligible;
    }
    if (world.today() > row.wedding_deadline) {
        return family::BetrothalBreakReason::GraceExpired;
    }
    return std::nullopt;
}

Check validate_break_betrothal(const World& world,
                               const MarriageRules& rules,
                               DynastyId actor,
                               const cmd::BreakBetrothal& payload,
                               const FaithPolicy* faith) {
    const auto* row = world.find_family_betrothal(payload.betrothal);
    if (row == nullptr) {
        return reject(
            Rejection::InvalidState, "the betrothal no longer exists", MarriageDenial::BetrothalNotFound);
    }
    if (payload.reason == family::BetrothalBreakReason::Voluntary) {
        if (!world.exists(actor) || (actor != row->groom_house && actor != row->bride_house)) {
            return reject(Rejection::NotAuthorized,
                          "the actor is not a betrothal party",
                          MarriageDenial::NotBetrothalParty);
        }
        if (!family_acting_head(world, actor).valid()) {
            return reject(
                Rejection::InvalidState, "the actor has no acting head", MarriageDenial::NoActingHead);
        }
    } else {
        if (actor.valid()) {
            return Check::err(CommandError{
                Rejection::NotAuthorized, "only the system may automatically break a betrothal", 0});
        }
        const auto expected = wedding_break_reason(world, rules, *row, faith);
        if (!expected || *expected != payload.reason) {
            return invalid_state("the betrothal break reason does not match the current facts");
        }
    }
    return Check::ok();
}

Check validate_wed(const World& world,
                   const MarriageRules& rules,
                   const cmd::Wed& payload,
                   const FaithPolicy* faith) {
    const auto* row = world.find_family_betrothal(payload.betrothal);
    if (row == nullptr) {
        return reject(
            Rejection::InvalidState, "the betrothal no longer exists", MarriageDenial::BetrothalNotFound);
    }
    if (world.today() < row->wedding_not_before ||
        static_cast<std::int64_t>(world.today().days()) >
            static_cast<std::int64_t>(row->wedding_deadline.days()) + 1) {
        return invalid_state("the wedding is outside its scheduled dates");
    }
    const auto denials = wedding_denials(world, rules, *row, faith);
    if (!denials.empty()) {
        return reject(Rejection::InvalidState, "the wedding parties are not eligible", denials.front());
    }
    if (houses_changed(world, *row)) {
        return reject(
            Rejection::InvalidState, "a betrothal party changed house", MarriageDenial::HouseChanged);
    }
    return wedding_writable(world, *row);
}

Check validate_settle_widowhood(const World& world, const cmd::SettleWidowhood& payload) {
    if (!world.dynasty_mode() || !world.is_archived(payload.deceased)) {
        return reject(Rejection::UnknownEntity,
                      "widowhood requires an archived deceased spouse",
                      MarriageDenial::NotAlive);
    }
    if (!std::ranges::any_of(world.family().marriages, [&](const auto& marriage) {
            return settlement_needed(marriage, payload.deceased);
        })) {
        return invalid_state("the deceased has no marriage awaiting widowhood settlement");
    }
    return Check::ok();
}

void apply_betroth(World& world, const MarriageRules& rules, const cmd::Betroth& payload) {
    const auto row = make_family_betrothal(world,
                                           rules,
                                           payload.groom,
                                           payload.bride,
                                           payload.dowry_kop,
                                           payload.dowry_source,
                                           payload.dispensation,
                                           family::BetrothalOrigin::Command);
    PAT_ASSERT(row.has_value(), "a validated betrothal has valid terms and dates");
    if (!row || !world.can_create_family_betrothal(*row)) {
        return;
    }
    const auto id = world.create_family_betrothal(*row);
    PAT_ASSERT(id.has_value(), "a validated betrothal fits the family table");
    if (id) {
        note_pair(world, row->groom_house, row->bride_house, row->groom, row->bride, 204);
    }
}

void apply_break_betrothal(World& world,
                           const MarriageRules& rules,
                           DynastyId actor,
                           const cmd::BreakBetrothal& payload) {
    const auto* pending = world.find_family_betrothal(payload.betrothal);
    if (pending == nullptr) {
        return;
    }
    const auto row = *pending;
    PAT_VERIFY(world.break_family_betrothal(row.id, payload.reason));
    if (payload.reason == family::BetrothalBreakReason::Voluntary) {
        world.add_honor(actor, -rules.consequences.betrothal_broken_honor, kStandingReasonBetrothalBroken);
        const auto other = actor == row.groom_house ? row.bride_house : row.groom_house;
        add_family_head_opinion(world,
                                family_acting_head(world, other),
                                family_acting_head(world, actor),
                                -rules.consequences.betrothal_broken_opinion,
                                kOpinionReasonBetrothalBroken);
    }
}

void apply_wed(World& world, const MarriageRules& rules, const cmd::Wed& payload) {
    const auto* pending = world.find_family_betrothal(payload.betrothal);
    if (pending == nullptr || !wedding_writable(world, *pending)) {
        return;
    }
    const auto row = *pending;
    const auto groom_head = family_acting_head(world, row.groom_house);
    const auto bride_head = family_acting_head(world, row.bride_house);
    const auto groom_rank = honor_rank(rules, world.dynasties().honor[row.groom_house.index()]);
    const auto bride_rank = honor_rank(rules, world.dynasties().honor[row.bride_house.index()]);
    const bool groom_third = world.characters().marriages[row.groom.index()] + 1 == kMaxMarriages;
    const bool bride_third = world.characters().marriages[row.bride.index()] + 1 == kMaxMarriages;
    PAT_VERIFY(world.marry(row.groom, row.bride));

    family::Marriage marriage;
    marriage.groom = row.groom;
    marriage.bride = row.bride;
    marriage.groom_house = row.groom_house;
    marriage.bride_house = row.bride_house;
    marriage.wedding = world.today();
    marriage.dowry_kop = row.dowry_kop;
    if (world.economy_present()) {
        marriage.dowry_kop = world.transfer(economy::AccountRef::of(row.dowry_source),
                                            economy::AccountRef::of(row.groom_house),
                                            economy::LedgerCode::DowryOut,
                                            economy::LedgerCode::DowryIn,
                                            row.dowry_kop);
    }
    // T11 will redeem nonzero claims. Zero-value completed widowhood remains safe to close now.
    if (row.claim_marriage != 0) {
        auto previous = *world.find_family_marriage(row.claim_marriage);
        previous.state = family::MarriageState::Closed;
        PAT_VERIFY(world.update_family_marriage(previous.id, previous));
    }
    const auto id = world.create_family_marriage(marriage);
    PAT_ASSERT(id.has_value(), "a preflighted wedding always stores its marriage record");
    if (!id) {
        return;
    }
    clear_mourning(world, row.groom);
    clear_mourning(world, row.bride);
    if (rules.consequences.bride_joins_groom_house &&
        world.dynasties().head[row.bride_house.index()] != row.bride) {
        world.set_character_dynasty(row.bride, row.groom_house);
        PAT_VERIFY(world.set_character_branch(row.bride, world.characters().branch[row.groom.index()]));
    }
    if (groom_third) {
        world.add_honor(
            row.groom_house, -rules.consequences.third_marriage_honor, kStandingReasonThirdMarriage);
    }
    if (bride_third) {
        world.add_honor(
            row.bride_house, -rules.consequences.third_marriage_honor, kStandingReasonThirdMarriage);
    }
    world.add_honor(row.groom_house,
                    rules.consequences.wedding_rank_honor * std::max(0, bride_rank - groom_rank),
                    kStandingReasonWeddingRank);
    add_family_head_opinion(
        world, groom_head, bride_head, rules.consequences.married_into_opinion, kOpinionReasonMarriedInto);
    add_family_head_opinion(
        world, bride_head, groom_head, rules.consequences.married_into_opinion, kOpinionReasonMarriedInto);
    note_pair(world, row.groom_house, row.bride_house, row.groom, row.bride, 206);
}

void apply_settle_widowhood(World& world, const cmd::SettleWidowhood& payload) {
    if (!validate_settle_widowhood(world, payload)) {
        return;
    }
    // Updates preserve vector order and size, so processing every affected record is stable.
    for (std::size_t i = 0; i < world.family().marriages.size(); ++i) {
        auto marriage = world.family().marriages[i];
        if (!settlement_needed(marriage, payload.deceased)) {
            continue;
        }
        const bool groom_alive = world.is_alive(marriage.groom);
        const bool bride_alive = world.is_alive(marriage.bride);
        const bool newly_widowed = marriage.state == family::MarriageState::Active;
        if (newly_widowed) {
            const auto groom_death = world.characters().death_date[marriage.groom.index()];
            const auto bride_death = world.characters().death_date[marriage.bride.index()];
            marriage.ended = groom_alive   ? bride_death
                             : bride_alive ? groom_death
                                           : std::min(groom_death, bride_death);
        }
        if (!groom_alive && bride_alive) {
            marriage.state = family::MarriageState::Widowed;
            marriage.end_reason = family::MarriageEndReason::Widowhood;
        } else {
            marriage.state = family::MarriageState::Closed;
            marriage.end_reason =
                groom_alive ? family::MarriageEndReason::Widowhood : family::MarriageEndReason::BothDead;
            marriage.claim_kop = 0;
        }
        PAT_VERIFY(world.update_family_marriage(marriage.id, marriage));
        const auto survivor = groom_alive ? marriage.groom : marriage.bride;
        if (newly_widowed && (groom_alive || bride_alive)) {
            auto status = world.family_status(survivor) != nullptr ? *world.family_status(survivor)
                                                                   : family::PersonStatus{};
            status.character = survivor;
            if (!world.characters().spouse[survivor.index()].valid() &&
                (status.widowed_on == kNoDate || status.widowed_on < marriage.ended)) {
                status.widowed_on = marriage.ended;
                PAT_VERIFY(world.set_family_status(status));
            }
            note_pair(world, marriage.groom_house, marriage.bride_house, survivor, payload.deceased, 208);
        }
    }
}

}  // namespace pat::sim
