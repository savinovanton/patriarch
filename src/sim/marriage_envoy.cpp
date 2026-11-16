#include "pat/sim/marriage_envoy.hpp"

#include "pat/core/assert.hpp"
#include "pat/sim/economy/settlement.hpp"
#include "pat/sim/marriage_score.hpp"
#include "pat/sim/world.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <utility>

namespace pat::sim {
namespace {

struct EnvoyParties {
    CharacterId groom;
    CharacterId bride;
    DynastyId groom_house;
    DynastyId bride_house;
    DynastyId initiator;
};

EnvoyParties envoy_parties(const World& world, DynastyId actor, CharacterId groom, CharacterId bride) {
    const auto& people = world.characters();
    if (world.exists(groom) && world.exists(bride) && people.sex[groom.index()] == Sex::Female &&
        people.sex[bride.index()] == Sex::Male) {
        std::swap(groom, bride);
    }
    const auto groom_house = world.exists(groom) ? people.dynasty[groom.index()] : DynastyId{};
    const auto bride_house = world.exists(bride) ? people.dynasty[bride.index()] : DynastyId{};
    return {groom, bride, groom_house, bride_house, actor.valid() ? actor : groom_house};
}

bool same_pair(CharacterId a, CharacterId b, CharacterId groom, CharacterId bride) {
    return (a == groom && b == bride) || (a == bride && b == groom);
}

void sort_denials(MarriageDenials& denials) {
    std::ranges::sort(denials);
    const auto duplicates = std::ranges::unique(denials);
    denials.erase(duplicates.begin(), duplicates.end());
}

struct EnvoyDowry {
    economy::Kopecks amount = 0;
    DynastyId source;
};

EnvoyDowry envoy_dowry(const World& world, const MarriageRules& rules, const EnvoyParties& parties) {
    const auto* previous = world.family_marriage_of(parties.bride);
    if (previous != nullptr && previous->bride == parties.bride &&
        previous->state == family::MarriageState::Widowed) {
        return {previous->groom_house == parties.groom_house ? 0 : previous->claim_kop,
                previous->groom_house};
    }
    return {standard_dowry(world, rules, parties.bride_house), parties.bride_house};
}

bool answer_date_available(const World& world, const MarriageRules& rules) {
    const auto due = static_cast<std::int64_t>(world.today().days()) + rules.negotiation.answer_days;
    return is_sane_date(world.today()) && due >= world.today().days() && due <= kSaneDateLimit;
}

std::optional<family::Negotiation>
envoy_terms(const World& world, const MarriageRules& rules, const EnvoyParties& parties, bool dispensation) {
    if (!answer_date_available(world, rules)) {
        return std::nullopt;
    }
    const auto dowry = envoy_dowry(world, rules, parties);
    family::Negotiation row;
    row.initiator = parties.initiator;
    row.groom = parties.groom;
    row.bride = parties.bride;
    row.groom_house = parties.groom_house;
    row.bride_house = parties.bride_house;
    row.sent = world.today();
    row.answer_due = Date::from_days(static_cast<std::int32_t>(static_cast<std::int64_t>(row.sent.days()) +
                                                               rules.negotiation.answer_days));
    row.dowry_kop = dowry.amount;
    row.dowry_source = dowry.source;
    row.dispensation = static_cast<std::uint8_t>(dispensation);
    return row;
}

}  // namespace

CharacterId family_acting_head(const World& world, DynastyId house) {
    if (!world.exists(house)) {
        return {};
    }
    CharacterId acting;
    if (const auto* regency = world.family_regency(house)) {
        acting = regency->kind == family::RegencyKind::Imperial ? regency->ward : regency->regent;
    } else if (world.family_dispute(house) != nullptr) {
        acting = world.family_caretaker(house);
    } else {
        acting = world.dynasties().head[house.index()];
    }
    return world.is_alive(acting) ? acting : CharacterId{};
}

MarriageDenials can_send_envoy(const World& world,
                               const MarriageRules& rules,
                               DynastyId actor,
                               CharacterId groom,
                               CharacterId bride,
                               bool dispensation,
                               const FaithPolicy* faith) {
    auto denials = check_marriage(world,
                                  rules,
                                  groom,
                                  bride,
                                  {.as_betrothal = true, .dispensation = dispensation, .actor = actor},
                                  faith);
    const auto parties = envoy_parties(world, actor, groom, bride);
    if (actor.valid() && !family_acting_head(world, actor).valid()) {
        denials.push_back(MarriageDenial::NoActingHead);
    }

    std::int32_t groom_count = 0;
    std::int32_t bride_count = 0;
    for (const auto& row : world.family().negotiations) {
        if (parties.groom_house.valid() &&
            (row.groom_house == parties.groom_house || row.bride_house == parties.groom_house)) {
            ++groom_count;
        }
        if (parties.bride_house.valid() &&
            (row.groom_house == parties.bride_house || row.bride_house == parties.bride_house)) {
            ++bride_count;
        }
        if (same_pair(row.groom, row.bride, groom, bride)) {
            denials.push_back(MarriageDenial::DuplicateNegotiation);
        }
    }
    if (groom_count >= rules.negotiation.max_parallel_negotiations ||
        bride_count >= rules.negotiation.max_parallel_negotiations) {
        denials.push_back(MarriageDenial::HouseBusy);
    }
    for (const auto& outcome : world.family().outcomes) {
        if (outcome.result == family::NegotiationResult::Refused &&
            same_pair(outcome.groom, outcome.bride, groom, bride) &&
            static_cast<std::int64_t>(world.today().days()) - outcome.answered.days() <
                rules.negotiation.refusal_cooldown_days) {
            denials.push_back(MarriageDenial::RecentlyRefused);
        }
    }

    const auto cost = world.economy().meta.present != 0 ? envoy_cost(rules, dispensation) : 0;
    if (world.economy().meta.present != 0 &&
        !economy::can_afford(world, economy::AccountRef::of(parties.initiator), cost)) {
        denials.push_back(MarriageDenial::InsufficientFunds);
    }
    if (!world.has_family_negotiation_capacity() || !answer_date_available(world, rules)) {
        denials.push_back(MarriageDenial::TableFull);
    }
    if (denials.empty()) {
        const auto row = envoy_terms(world, rules, parties, dispensation);
        if (!row || !world.can_create_family_negotiation(*row)) {
            denials.push_back(MarriageDenial::TableFull);
        }
    }
    sort_denials(denials);
    return denials;
}

MarriageDenials can_withdraw_envoy(const World& world, DynastyId actor, family::NegotiationId negotiation) {
    MarriageDenials denials;
    const auto* row = world.find_family_negotiation(negotiation);
    if (row == nullptr) {
        denials.push_back(MarriageDenial::NegotiationNotFound);
    } else if (actor.valid() && actor != row->initiator) {
        denials.push_back(MarriageDenial::NotInitiator);
    }
    if (actor.valid() && !family_acting_head(world, actor).valid()) {
        denials.push_back(MarriageDenial::NoActingHead);
    }
    if (!world.dynasty_mode() || !world.has_family_outcome_capacity()) {
        denials.push_back(MarriageDenial::TableFull);
    }
    sort_denials(denials);
    return denials;
}

family::NegotiationId send_marriage_envoy(World& world,
                                          const MarriageRules& rules,
                                          DynastyId actor,
                                          CharacterId groom,
                                          CharacterId bride,
                                          bool dispensation,
                                          const FaithPolicy* /*faith*/) {
    const auto parties = envoy_parties(world, actor, groom, bride);
    const auto terms = envoy_terms(world, rules, parties, dispensation);
    PAT_ASSERT(terms.has_value(), "validated marriage envoy must have a representable answer date");
    if (!terms) {
        return 0;
    }
    auto row = *terms;
    const auto id = world.create_family_negotiation(row);
    PAT_ASSERT(id.has_value(), "validated marriage envoy must fit family storage");
    if (!id) {
        return 0;
    }
    if (world.economy().meta.present != 0) {
        row.id = *id;
        row.cost_kop = -world.post_money(economy::AccountRef::of(row.initiator),
                                         economy::LedgerCode::MarriageEnvoy,
                                         -envoy_cost(rules, dispensation));
        const bool updated = world.update_family_negotiation(*id, row);
        PAT_ASSERT(updated, "the stored marriage envoy retains its validated terms");
        (void)updated;
    }
    PAT_VERIFY(world.record_chronicle_note(row.initiator, row.groom, row.bride, 201));
    return *id;
}

void withdraw_marriage_envoy(World& world, const MarriageRules& rules, family::NegotiationId negotiation) {
    const auto* row = world.find_family_negotiation(negotiation);
    PAT_ASSERT(row != nullptr, "validated marriage withdrawal has a negotiation");
    if (row == nullptr) {
        return;
    }
    family::NegotiationOutcome outcome;
    outcome.negotiation = row->id;
    outcome.initiator = row->initiator;
    outcome.groom = row->groom;
    outcome.bride = row->bride;
    outcome.responder = row->initiator == row->groom_house ? row->bride_house : row->groom_house;
    outcome.sent = row->sent;
    outcome.answered = world.today();
    outcome.result = family::NegotiationResult::Withdrawn;
    const auto refund = economy::mul_permille(row->cost_kop, rules.negotiation.withdraw_refund_permille);
    const auto sequence = world.append_family_outcome(outcome);
    PAT_ASSERT(sequence.has_value(), "validated marriage withdrawal must fit the outcome ring");
    if (!sequence) {
        return;
    }
    if (world.economy().meta.present != 0) {
        (void)world.post_money(
            economy::AccountRef::of(outcome.initiator), economy::LedgerCode::MarriageEnvoy, refund);
    }
    const bool closed = world.close_family_negotiation(negotiation);
    PAT_ASSERT(closed, "the withdrawn marriage negotiation exists until closing");
    (void)closed;
}

}  // namespace pat::sim
