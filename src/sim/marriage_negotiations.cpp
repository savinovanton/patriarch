#include "pat/sim/marriage_negotiations.hpp"

#include "pat/core/assert.hpp"
#include "pat/sim/dynasty_relation.hpp"
#include "pat/sim/marriage_envoy.hpp"
#include "pat/sim/marriage_score.hpp"

#include <algorithm>
#include <cstdint>

namespace pat::sim {
namespace {

using Check = core::Result<void, CommandError>;

Check reject(Rejection rejection, const char* detail, std::uint16_t code = 0) {
    return Check::err(CommandError{rejection, detail, code});
}

bool houses_changed(const World& world, const family::Negotiation& row) {
    return (world.exists(row.groom) && world.characters().dynasty[row.groom.index()] != row.groom_house) ||
           (world.exists(row.bride) && world.characters().dynasty[row.bride.index()] != row.bride_house);
}

bool valid_void_shape(const cmd::RespondMarriage& response) {
    return response.score == 0 && response.threshold == 0 &&
           std::ranges::none_of(response.terms, [](std::int32_t value) { return value != 0; });
}

MarriageDenials response_denials(const World& world,
                                 const MarriageRules& rules,
                                 const family::Negotiation& row,
                                 const FaithPolicy* faith) {
    return check_marriage(world,
                          rules,
                          row.groom,
                          row.bride,
                          {.as_betrothal = true,
                           .dispensation = row.dispensation != 0,
                           .actor = row.initiator,
                           .ignore_negotiation = row.id},
                          faith);
}

bool valid_score_shape(const MarriageRules& rules, const cmd::RespondMarriage& response) {
    std::int64_t total = 0;
    for (std::size_t i = 0; i < response.terms.size(); ++i) {
        const auto bounds = term_bounds(rules, static_cast<ScoreTerm>(i));
        if (response.terms[i] < bounds.min || response.terms[i] > bounds.max) {
            return false;
        }
        total += response.terms[i];
    }
    if (total != response.score) {
        return false;
    }
    if (response.result == family::NegotiationResult::Accepted) {
        return response.reason == 0 && response.score >= response.threshold;
    }
    if (response.score >= response.threshold) {
        return false;
    }
    return response.reason == score_term_code(weakest_score_term(response.terms));
}

std::optional<Date> date_after(Date date, std::int32_t days) {
    const auto sum = static_cast<std::int64_t>(date.days()) + days;
    if (!is_sane_date(date) || days < 0 || sum > kSaneDateLimit) {
        return std::nullopt;
    }
    return Date::from_days(static_cast<std::int32_t>(sum));
}

}  // namespace

void add_family_head_opinion(
    World& world, CharacterId from, CharacterId to, std::int32_t points, OpinionReason reason) {
    if (from != to && world.is_alive(from) && world.is_alive(to)) {
        PAT_VERIFY(world.add_opinion(from, to, core::Fixed32::from_int(points), reason));
    }
}

std::optional<family::Betrothal> make_family_betrothal(const World& world,
                                                       const MarriageRules& rules,
                                                       CharacterId groom,
                                                       CharacterId bride,
                                                       std::int64_t dowry_kop,
                                                       DynastyId dowry_source,
                                                       bool dispensation,
                                                       family::BetrothalOrigin origin) {
    const auto delayed = date_after(world.today(), rules.negotiation.wedding_delay_days);
    if (!delayed || !world.is_alive(groom) || !world.is_alive(bride)) {
        return std::nullopt;
    }
    const auto& people = world.characters();
    const auto groom_birth = people.birth_date[groom.index()];
    const auto bride_birth = people.birth_date[bride.index()];
    const auto groom_adult =
        anniversary_in_year(groom_birth, groom_birth.year() + rules.eligibility.min_marriage_age_male);
    const auto bride_adult =
        anniversary_in_year(bride_birth, bride_birth.year() + rules.eligibility.min_marriage_age_female);
    const auto not_before = std::max({*delayed, groom_adult, bride_adult});
    const auto deadline = date_after(not_before, rules.negotiation.wedding_grace_days);
    if (!deadline) {
        return std::nullopt;
    }
    family::Betrothal row;
    row.groom = groom;
    row.bride = bride;
    row.groom_house = people.dynasty[groom.index()];
    row.bride_house = people.dynasty[bride.index()];
    row.contracted = world.today();
    row.wedding_not_before = not_before;
    row.wedding_deadline = *deadline;
    row.dowry_kop = dowry_kop;
    row.dowry_source = dowry_source;
    row.dispensation = dispensation;
    row.origin = origin;
    if (const auto widow = widow_dowry(world, row.bride, row.groom_house)) {
        row.dowry_kop = widow->amount;
        row.dowry_source = widow->source;
        row.claim_marriage = widow->claim_marriage;
    }
    return row;
}

namespace {

std::optional<family::Betrothal>
accepted_betrothal(const World& world, const MarriageRules& rules, const family::Negotiation& negotiation) {
    return make_family_betrothal(world,
                                 rules,
                                 negotiation.groom,
                                 negotiation.bride,
                                 negotiation.dowry_kop,
                                 negotiation.dowry_source,
                                 negotiation.dispensation != 0,
                                 family::BetrothalOrigin::Negotiation);
}

void response_opinion(World& world,
                      const family::NegotiationOutcome& outcome,
                      std::int32_t points,
                      OpinionReason reason) {
    const auto from = family_acting_head(world, outcome.responder);
    const auto to = family_acting_head(world, outcome.initiator);
    add_family_head_opinion(world, from, to, points, reason);
}

}  // namespace

std::optional<cmd::RespondMarriage> compute_marriage_response(const World& world,
                                                              const MarriageRules& rules,
                                                              family::NegotiationId negotiation,
                                                              const FaithPolicy* faith) {
    const auto* row = world.find_family_negotiation(negotiation);
    if (row == nullptr) {
        return std::nullopt;
    }
    cmd::RespondMarriage response;
    response.negotiation = row->id;
    if (houses_changed(world, *row)) {
        response.result = family::NegotiationResult::Void;
        response.reason = static_cast<std::uint16_t>(MarriageDenial::HouseChanged);
        return response;
    }
    if (world.today() < row->answer_due) {
        return std::nullopt;
    }
    const auto denials = response_denials(world, rules, *row, faith);
    if (!denials.empty()) {
        response.result = family::NegotiationResult::Void;
        response.reason = static_cast<std::uint16_t>(denials.front());
        return response;
    }
    MatchContext context;
    context.initiator = row->initiator;
    context.relation = dynasty_relation(world, row->groom_house, row->bride_house, rules);
    const auto evaluation = evaluate_match(world, rules, row->groom, row->bride, row->dowry_kop, context);
    if (!evaluation) {
        return std::nullopt;
    }
    const auto responder_side = row->initiator == row->groom_house ? MatchSide::Bride : MatchSide::Groom;
    const auto& score = evaluation->score(responder_side);
    response.result =
        score.accepted ? family::NegotiationResult::Accepted : family::NegotiationResult::Refused;
    response.reason = score.refusal_code;
    response.score = score.score;
    response.threshold = score.threshold;
    response.terms = score.terms;
    return response;
}

core::Result<void, CommandError> validate_marriage_response(const World& world,
                                                            const MarriageRules& rules,
                                                            const cmd::RespondMarriage& response,
                                                            const FaithPolicy* faith) {
    if (!cmd::is_marriage_response_result(response.result)) {
        return reject(Rejection::InvalidArgument, "the marriage answer result is not supported");
    }
    const auto* row = world.find_family_negotiation(response.negotiation);
    if (row == nullptr) {
        return reject(Rejection::InvalidState,
                      "the marriage negotiation no longer exists",
                      static_cast<std::uint16_t>(MarriageDenial::NegotiationNotFound));
    }
    const bool changed_house = houses_changed(world, *row);
    if (world.today() < row->answer_due && !changed_house) {
        return reject(Rejection::InvalidState, "the marriage answer is not due");
    }
    if (!world.has_family_outcome_capacity()) {
        return reject(Rejection::InvalidState,
                      "the marriage outcome sequence is exhausted",
                      static_cast<std::uint16_t>(MarriageDenial::TableFull));
    }
    if (changed_house) {
        if (response.result != family::NegotiationResult::Void ||
            response.reason != static_cast<std::uint16_t>(MarriageDenial::HouseChanged) ||
            !valid_void_shape(response)) {
            return reject(Rejection::InvalidState,
                          "the marriage participant changed house",
                          static_cast<std::uint16_t>(MarriageDenial::HouseChanged));
        }
        return Check::ok();
    }
    const auto denials = response_denials(world, rules, *row, faith);
    if (response.result == family::NegotiationResult::Void) {
        if (denials.empty() || response.reason != static_cast<std::uint16_t>(denials.front()) ||
            !valid_void_shape(response)) {
            return reject(Rejection::InvalidArgument, "the void marriage answer has inconsistent facts");
        }
        return Check::ok();
    }
    if (!valid_score_shape(rules, response)) {
        return reject(Rejection::InvalidArgument, "the marriage answer has an inconsistent score");
    }
    if (!denials.empty()) {
        return reject(Rejection::InvalidState,
                      "the marriage participants are no longer eligible",
                      static_cast<std::uint16_t>(denials.front()));
    }
    if (response.result == family::NegotiationResult::Accepted) {
        const auto betrothal = accepted_betrothal(world, rules, *row);
        if (!betrothal || !world.can_create_family_betrothal(*betrothal, row->id)) {
            return reject(Rejection::InvalidState,
                          "the accepted negotiation cannot create a betrothal",
                          static_cast<std::uint16_t>(MarriageDenial::TableFull));
        }
    }
    return Check::ok();
}

void respond_marriage(World& world,
                      const MarriageRules& rules,
                      const cmd::RespondMarriage& response,
                      const FaithPolicy* /*faith*/) {
    const auto* pending = world.find_family_negotiation(response.negotiation);
    PAT_ASSERT(pending != nullptr, "a validated marriage response has a pending negotiation");
    if (pending == nullptr) {
        return;
    }
    const auto negotiation = *pending;
    auto outcome = family::negotiation_outcome(negotiation, world.today(), response.result);
    outcome.reason = response.reason;
    outcome.score = response.score;
    outcome.threshold = response.threshold;
    outcome.terms = response.terms;
    const auto sequence = world.append_family_outcome(outcome);
    PAT_ASSERT(sequence.has_value(), "a validated marriage answer fits the outcome ring");
    if (!sequence) {
        return;
    }
    if (response.result == family::NegotiationResult::Accepted) {
        const auto betrothal = accepted_betrothal(world, rules, negotiation);
        PAT_ASSERT(betrothal.has_value(), "a validated accepted answer has wedding dates");
        if (!betrothal) {
            return;
        }
        const auto id = world.accept_family_negotiation(negotiation.id, *betrothal);
        PAT_ASSERT(id.has_value(), "a validated accepted answer creates its betrothal");
        if (!id) {
            return;
        }
        if (negotiation.dispensation != 0) {
            world.add_honor(negotiation.initiator,
                            -rules.consequences.dispensation_honor,
                            kStandingReasonEnvoyDispensation);
        }
        response_opinion(
            world, outcome, rules.consequences.marriage_accepted_opinion, kOpinionReasonMarriageAccepted);
        PAT_VERIFY(world.record_chronicle_note(outcome.initiator, outcome.groom, outcome.bride, 202));
        PAT_VERIFY(world.record_chronicle_note(outcome.initiator, outcome.groom, outcome.bride, 204));
        return;
    }
    PAT_VERIFY(world.close_family_negotiation(negotiation.id));
    if (response.result == family::NegotiationResult::Refused) {
        response_opinion(
            world, outcome, -rules.consequences.marriage_refused_opinion, kOpinionReasonMarriageRefused);
        PAT_VERIFY(world.record_chronicle_note(outcome.initiator, outcome.groom, outcome.bride, 203));
    }
}

}  // namespace pat::sim
