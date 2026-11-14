#include "pat/sim/world.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <tuple>

namespace pat::sim {
namespace {

template <class Rows, class Key, class Projection>
auto row_position(Rows& rows, const Key& key, Projection projection) {
    return std::ranges::lower_bound(rows, key, {}, projection);
}

template <class Rows, class Key, class Projection>
const typename Rows::value_type* find_row(const Rows& rows, const Key& key, Projection projection) {
    const auto position = row_position(rows, key, projection);
    return position != rows.end() && std::invoke(projection, *position) == key ? &*position : nullptr;
}

template <class Rows, class Row, class Projection>
void replace_row(Rows& rows, const Row& row, Projection projection) {
    const auto key = std::invoke(projection, row);
    const auto position = row_position(rows, key, projection);
    if (position != rows.end() && std::invoke(projection, *position) == key) {
        *position = row;
    } else {
        rows.insert(position, row);
    }
}

template <class Rows, class Key, class Projection>
bool erase_row(Rows& rows, const Key& key, Projection projection) {
    const auto position = row_position(rows, key, projection);
    if (position == rows.end() || std::invoke(projection, *position) != key) {
        return false;
    }
    rows.erase(position);
    return true;
}

template <class Rows, class Projection>
bool ordered_unique(const Rows& rows, Projection projection) {
    return std::adjacent_find(rows.begin(), rows.end(), [&](const auto& a, const auto& b) {
               return std::invoke(projection, a) >= std::invoke(projection, b);
           }) == rows.end();
}

bool past_date(const World& world, Date date) {
    return is_sane_date(date) && date <= world.today();
}

bool optional_past_date(const World& world, Date date) {
    return date == kNoDate || past_date(world, date);
}

bool money(economy::Kopecks amount) {
    return amount >= 0 && amount <= economy::kMaxPostingKop;
}

bool allocated_id(std::uint32_t id, std::uint32_t next) {
    return id > 0 && id < next;
}

bool allocatable(std::uint32_t next) {
    return next > 0 && next < std::numeric_limits<std::uint32_t>::max();
}

bool dead_today(const World& world, CharacterId character) {
    return world.is_archived(character) && world.characters().death_date[character.index()] == world.today();
}

bool blocked(const World& world, CharacterId character, std::uint8_t flags) {
    const auto* status = world.family_status(character);
    return status != nullptr && (status->flags & flags) != 0;
}

bool pair_exists(const World& world, CharacterId groom, CharacterId bride) {
    return world.exists(groom) && world.exists(bride) && groom != bride &&
           world.characters().sex[groom.index()] == Sex::Male &&
           world.characters().sex[bride.index()] == Sex::Female;
}

template <class Row>
bool current_houses(const World& world, const Row& row) {
    // Bind new writes to the participants. Stored house snapshots may outlive a membership change.
    return world.characters().dynasty[row.groom.index()] == row.groom_house &&
           world.characters().dynasty[row.bride.index()] == row.bride_house;
}

bool engaged_elsewhere(const World& world,
                       CharacterId character,
                       family::NegotiationId negotiation,
                       family::BetrothalId betrothal) {
    const auto* n = world.family_negotiation_of(character);
    const auto* b = world.family_betrothal_of(character);
    return (n != nullptr && n->id != negotiation) || (b != nullptr && b->id != betrothal);
}

bool valid_negotiation(const World& world, const family::Negotiation& row) {
    if (!world.dynasty_mode() || !pair_exists(world, row.groom, row.bride) || !world.is_alive(row.groom) ||
        !world.is_alive(row.bride) || !world.exists(row.groom_house) || !world.exists(row.bride_house) ||
        row.groom_house == row.bride_house || !world.exists(row.initiator) ||
        !world.exists(row.dowry_source) ||
        (row.initiator != row.groom_house && row.initiator != row.bride_house) ||
        !past_date(world, row.sent) || !is_sane_date(row.answer_due) || row.answer_due < row.sent ||
        !money(row.cost_kop) || !money(row.dowry_kop) || row.dispensation > 1 || row.dowry_tier != 0 ||
        row.state != family::NegotiationState::Pending) {
        return false;
    }
    return !world.characters().spouse[row.groom.index()].valid() &&
           !world.characters().spouse[row.bride.index()].valid() &&
           !engaged_elsewhere(world, row.groom, row.id, 0) && !engaged_elsewhere(world, row.bride, row.id, 0);
}

bool valid_betrothal(const World& world, const family::Betrothal& row) {
    if (!world.dynasty_mode() || !pair_exists(world, row.groom, row.bride) || !world.is_alive(row.groom) ||
        !world.is_alive(row.bride) || !world.exists(row.groom_house) || !world.exists(row.bride_house) ||
        row.groom_house == row.bride_house || !world.exists(row.dowry_source) ||
        !past_date(world, row.contracted) || !is_sane_date(row.wedding_not_before) ||
        !is_sane_date(row.wedding_deadline) || row.wedding_not_before < row.contracted ||
        row.wedding_deadline < row.wedding_not_before || !money(row.dowry_kop) || row.dispensation > 1 ||
        (row.origin != family::BetrothalOrigin::Negotiation &&
         row.origin != family::BetrothalOrigin::Command)) {
        return false;
    }
    if (row.claim_marriage != 0 && world.find_family_marriage(row.claim_marriage) == nullptr) {
        return false;
    }
    return !world.characters().spouse[row.groom.index()].valid() &&
           !world.characters().spouse[row.bride.index()].valid() &&
           !engaged_elsewhere(world, row.groom, 0, row.id) && !engaged_elsewhere(world, row.bride, 0, row.id);
}

bool valid_marriage(const World& world, const family::Marriage& row, bool pending_death) {
    if (!world.dynasty_mode() || !pair_exists(world, row.groom, row.bride) ||
        !world.exists(row.groom_house) || !world.exists(row.bride_house) || !past_date(world, row.wedding) ||
        !money(row.dowry_kop) || !money(row.refunded_kop) || row.refunded_kop > row.dowry_kop ||
        row.claim_kop < 0 || row.claim_kop > economy::kMaxTreasuryKop) {
        return false;
    }
    if (row.state == family::MarriageState::Active) {
        if (row.ended != kNoDate || row.end_reason != family::MarriageEndReason::None || row.claim_kop != 0) {
            return false;
        }
        const bool living_pair = world.is_alive(row.groom) && world.is_alive(row.bride) &&
                                 world.characters().spouse[row.groom.index()] == row.bride &&
                                 world.characters().spouse[row.bride.index()] == row.groom;
        if (!living_pair &&
            !(pending_death && (dead_today(world, row.groom) || dead_today(world, row.bride)))) {
            return false;
        }
        for (const auto& other : world.family().marriages) {
            if (other.id != row.id && other.state == family::MarriageState::Active &&
                (other.groom == row.groom || other.bride == row.bride)) {
                return false;
            }
        }
        return world.engagement_of(row.groom) == 0 && world.engagement_of(row.bride) == 0;
    }
    if (!past_date(world, row.ended) || row.ended < row.wedding ||
        row.end_reason < family::MarriageEndReason::Widowhood ||
        row.end_reason > family::MarriageEndReason::BothDead) {
        return false;
    }
    if (row.state == family::MarriageState::Widowed) {
        return world.is_archived(row.groom) &&
               (world.is_alive(row.bride) || (pending_death && dead_today(world, row.bride))) &&
               row.end_reason == family::MarriageEndReason::Widowhood;
    }
    return row.state == family::MarriageState::Closed && row.claim_kop == 0;
}

bool valid_outcome(const World& world, const family::NegotiationOutcome& row) {
    if (!world.dynasty_mode() || !allocated_id(row.negotiation, world.family().next_negotiation_id) ||
        !world.exists(row.initiator) || !world.exists(row.responder) ||
        !pair_exists(world, row.groom, row.bride) || !past_date(world, row.sent) ||
        !past_date(world, row.answered) || row.answered < row.sent ||
        row.result < family::NegotiationResult::Accepted || row.result > family::NegotiationResult::Void) {
        return false;
    }
    std::int64_t total = 0;
    for (const auto term : row.terms) {
        total += term;
    }
    return total == row.score;
}

bool status_default(const family::PersonStatus& row) {
    return row.flags == 0 && row.tonsured_on == kNoDate && row.disowned_on == kNoDate &&
           row.widowed_on == kNoDate;
}

bool valid_status(const World& world, const family::PersonStatus& row) {
    if (!world.dynasty_mode() || !world.exists(row.character) ||
        (row.flags & ~family::kPersonStatusFlags) != 0 || !optional_past_date(world, row.tonsured_on) ||
        !optional_past_date(world, row.disowned_on) || !optional_past_date(world, row.widowed_on) ||
        ((row.flags & family::kPersonTonsured) != 0) != (row.tonsured_on != kNoDate) ||
        ((row.flags & family::kPersonDisowned) != 0) != (row.disowned_on != kNoDate) ||
        (row.widowed_on != kNoDate && !world.is_alive(row.character))) {
        return false;
    }
    if ((row.flags & family::kPersonTonsured) != 0) {
        if (std::ranges::find(world.dynasties().head, row.character) != world.dynasties().head.end()) {
            return false;
        }
        for (const auto& succession : world.family().succession) {
            if (succession.designated_heir == row.character) {
                return false;
            }
        }
    }
    if ((row.flags & (family::kPersonTonsured | family::kPersonDisowned)) != 0) {
        for (const auto& regency : world.family().regencies) {
            if (regency.regent == row.character) {
                return false;
            }
        }
        for (const auto& grant : world.family().land_grants) {
            if (grant.holder == row.character) {
                return false;
            }
        }
    }
    return true;
}

bool succession_default(const family::SuccessionState& row) {
    return !row.designated_heir.valid() && !row.guardian.valid() && row.law_changed_on == kNoDate;
}

bool valid_succession(const World& world, const family::SuccessionState& row) {
    return world.dynasty_mode() && world.exists(row.dynasty) &&
           (!row.designated_heir.valid() ||
            (world.exists(row.designated_heir) &&
             world.characters().dynasty[row.designated_heir.index()] == row.dynasty &&
             !blocked(world, row.designated_heir, family::kPersonTonsured))) &&
           (!row.guardian.valid() || world.exists(row.guardian)) &&
           optional_past_date(world, row.law_changed_on);
}

CharacterId caretaker_of(const World& world, const family::Dispute& dispute) {
    CharacterId minor;
    for (std::size_t i = 0; i < dispute.claimants_n && i < dispute.claimants.size(); ++i) {
        const auto candidate = dispute.claimants[i].character;
        if (world.is_alive(candidate)) {
            if (age_years(world.characters().birth_date[candidate.index()], world.today()) >= 16) {
                return candidate;
            }
            if (!minor.valid()) {
                minor = candidate;
            }
        }
    }
    return minor;
}

bool valid_regency(const World& world, const family::Regency& row, bool pending_death) {
    if (!world.dynasty_mode() || !world.exists(row.dynasty) || !world.is_alive(row.ward) ||
        world.characters().dynasty[row.ward.index()] != row.dynasty ||
        age_years(world.characters().birth_date[row.ward.index()], world.today()) >= 16 ||
        !past_date(world, row.started) || row.last_risk_permille > 1000 ||
        row.kind < family::RegencyKind::Mother || row.kind > family::RegencyKind::Imperial ||
        (row.excluded.valid() && !world.exists(row.excluded))) {
        return false;
    }
    const auto head = world.dynasties().head[row.dynasty.index()];
    if (head != row.ward && (head.valid() || world.family_dispute(row.dynasty) == nullptr ||
                             world.family_caretaker(row.dynasty) != row.ward)) {
        return false;
    }
    if (row.kind == family::RegencyKind::Imperial) {
        return !row.regent.valid();
    }
    if ((!world.is_alive(row.regent) && !(pending_death && dead_today(world, row.regent))) ||
        row.regent == row.ward || row.regent == row.excluded ||
        age_years(world.characters().birth_date[row.regent.index()], world.today()) < 16 ||
        blocked(world, row.regent, family::kPersonTonsured | family::kPersonDisowned)) {
        return false;
    }
    if (row.kind == family::RegencyKind::Mother) {
        return world.characters().mother[row.ward.index()] == row.regent &&
               world.characters().sex[row.regent.index()] == Sex::Female;
    }
    return world.characters().dynasty[row.regent.index()] == row.dynasty;
}

bool valid_dispute(const World& world, const family::Dispute& row, bool pending_death) {
    if (!world.dynasty_mode() || !world.exists(row.dynasty) ||
        world.dynasties().head[row.dynasty.index()].valid() || !world.is_archived(row.predecessor) ||
        world.characters().dynasty[row.predecessor.index()] != row.dynasty ||
        row.reason < family::DisputeReason::CloseScores ||
        row.reason > family::DisputeReason::NoAppointment || !past_date(world, row.opened) ||
        !is_sane_date(row.resolve_by) || row.resolve_by < row.opened || row.claimants_n < 2 ||
        row.claimants_n > family::kMaxDisputeClaimants) {
        return false;
    }
    for (std::size_t i = 0; i < row.claimants.size(); ++i) {
        const auto& candidate = row.claimants[i];
        if (i >= row.claimants_n) {
            if (candidate.character.valid() || candidate.score != 0) {
                return false;
            }
            continue;
        }
        if (!world.exists(candidate.character) || candidate.character == row.predecessor ||
            (!pending_death && !world.is_alive(candidate.character)) ||
            world.characters().dynasty[candidate.character.index()] != row.dynasty ||
            (!pending_death &&
             blocked(world, candidate.character, family::kPersonTonsured | family::kPersonDisowned))) {
            return false;
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (row.claimants[j].character == candidate.character) {
                return false;
            }
        }
        if (i > 0) {
            const auto& previous = row.claimants[i - 1];
            const auto previous_key = std::tuple{-static_cast<std::int64_t>(previous.score),
                                                 world.characters().birth_date[previous.character.index()],
                                                 previous.character};
            const auto key = std::tuple{-static_cast<std::int64_t>(candidate.score),
                                        world.characters().birth_date[candidate.character.index()],
                                        candidate.character};
            if (key < previous_key) {
                return false;
            }
        }
    }
    return true;
}

bool valid_grant(const World& world, const family::LandGrant& row, bool pending_death) {
    if (!world.dynasty_mode() || !world.exists(row.province) || !world.exists(row.holder) ||
        (!world.is_alive(row.holder) && !(pending_death && dead_today(world, row.holder))) ||
        !world.characters().dynasty[row.holder.index()].valid() ||
        age_years(world.characters().birth_date[row.holder.index()], world.today()) < 16 ||
        blocked(world, row.holder, family::kPersonTonsured | family::kPersonDisowned) ||
        world.provinces().owner[row.province.index()] != world.characters().dynasty[row.holder.index()] ||
        !past_date(world, row.granted_on)) {
        return false;
    }
    return !world.economy_present() || !world.is_alive(row.holder) ||
           world.economy().provinces.administrator[row.province.index()] == row.holder;
}

}  // namespace

const family::Negotiation* World::find_family_negotiation(family::NegotiationId id) const {
    return find_row(family_.negotiations, id, &family::Negotiation::id);
}

const family::Betrothal* World::find_family_betrothal(family::BetrothalId id) const {
    return find_row(family_.betrothals, id, &family::Betrothal::id);
}

const family::Marriage* World::find_family_marriage(family::MarriageId id) const {
    return find_row(family_.marriages, id, &family::Marriage::id);
}

const family::NegotiationOutcome* World::find_family_outcome(family::OutcomeSequence seq) const {
    return find_row(family_.outcomes, seq, &family::NegotiationOutcome::seq);
}

const family::PersonStatus* World::family_status(CharacterId character) const {
    return find_row(family_.person_statuses, character, &family::PersonStatus::character);
}

const family::SuccessionState* World::family_succession(DynastyId dynasty) const {
    return find_row(family_.succession, dynasty, &family::SuccessionState::dynasty);
}

const family::Regency* World::family_regency(DynastyId dynasty) const {
    return find_row(family_.regencies, dynasty, &family::Regency::dynasty);
}

const family::Dispute* World::family_dispute(DynastyId dynasty) const {
    return find_row(family_.disputes, dynasty, &family::Dispute::dynasty);
}

const family::LandGrant* World::family_land_grant(ProvinceId province) const {
    return find_row(family_.land_grants, province, &family::LandGrant::province);
}

const family::Negotiation* World::family_negotiation_of(CharacterId character) const {
    const auto position = std::ranges::find_if(family_.negotiations, [&](const auto& row) {
        return row.groom == character || row.bride == character;
    });
    return position == family_.negotiations.end() ? nullptr : &*position;
}

const family::Betrothal* World::family_betrothal_of(CharacterId character) const {
    const auto position = std::ranges::find_if(family_.betrothals, [&](const auto& row) {
        return row.groom == character || row.bride == character;
    });
    return position == family_.betrothals.end() ? nullptr : &*position;
}

const family::Marriage* World::family_marriage_of(CharacterId character) const {
    for (auto position = family_.marriages.rbegin(); position != family_.marriages.rend(); ++position) {
        if ((position->groom == character || position->bride == character) &&
            position->state != family::MarriageState::Closed) {
            return &*position;
        }
    }
    return nullptr;
}

std::uint8_t World::engagement_of(CharacterId character) const {
    if (family_negotiation_of(character) != nullptr) {
        return 1;
    }
    return family_betrothal_of(character) != nullptr ? 2 : 0;
}

CharacterId World::family_caretaker(DynastyId dynasty) const {
    const auto* dispute = family_dispute(dynasty);
    return dispute != nullptr ? caretaker_of(*this, *dispute) : CharacterId{};
}

std::optional<family::NegotiationId> World::create_family_negotiation(family::Negotiation row) {
    if (row.id != 0 || !allocatable(family_.next_negotiation_id) ||
        family_.negotiations.size() >= family::kMaxNegotiations || !valid_negotiation(*this, row) ||
        !current_houses(*this, row)) {
        return std::nullopt;
    }
    row.id = family_.next_negotiation_id;
    family_.negotiations.push_back(row);
    ++family_.next_negotiation_id;
    return row.id;
}

bool World::update_family_negotiation(family::NegotiationId id, const family::Negotiation& row) {
    if (row.id != id || find_family_negotiation(id) == nullptr || !valid_negotiation(*this, row) ||
        !current_houses(*this, row)) {
        return false;
    }
    replace_row(family_.negotiations, row, &family::Negotiation::id);
    return true;
}

bool World::close_family_negotiation(family::NegotiationId id) {
    return erase_row(family_.negotiations, id, &family::Negotiation::id);
}

std::optional<family::BetrothalId> World::create_family_betrothal(family::Betrothal row) {
    if (row.id != 0 || !allocatable(family_.next_betrothal_id) ||
        family_.betrothals.size() >= family::kMaxBetrothals || !valid_betrothal(*this, row) ||
        !current_houses(*this, row)) {
        return std::nullopt;
    }
    row.id = family_.next_betrothal_id;
    family_.betrothals.push_back(row);
    ++family_.next_betrothal_id;
    return row.id;
}

bool World::update_family_betrothal(family::BetrothalId id, const family::Betrothal& row) {
    if (row.id != id || find_family_betrothal(id) == nullptr || !valid_betrothal(*this, row) ||
        !current_houses(*this, row)) {
        return false;
    }
    replace_row(family_.betrothals, row, &family::Betrothal::id);
    return true;
}

bool World::close_family_betrothal(family::BetrothalId id) {
    return erase_row(family_.betrothals, id, &family::Betrothal::id);
}

std::optional<family::MarriageId> World::create_family_marriage(family::Marriage row) {
    if (row.id != 0 || !allocatable(family_.next_marriage_id) || !valid_marriage(*this, row, false)) {
        return std::nullopt;
    }
    auto evict = family_.marriages.end();
    if (family_.marriages.size() >= family::kMaxMarriageRecords) {
        evict = std::ranges::find_if(family_.marriages, [&](const auto& existing) {
            return existing.state == family::MarriageState::Closed && existing.claim_kop == 0 &&
                   !std::ranges::any_of(family_.betrothals, [&](const auto& betrothal) {
                       return betrothal.claim_marriage == existing.id;
                   });
        });
        if (evict == family_.marriages.end()) {
            return std::nullopt;
        }
    }
    row.id = family_.next_marriage_id;
    if (evict != family_.marriages.end()) {
        family_.marriages.erase(evict);
    }
    family_.marriages.push_back(row);
    ++family_.next_marriage_id;
    return row.id;
}

bool World::update_family_marriage(family::MarriageId id, const family::Marriage& row) {
    if (row.id != id || find_family_marriage(id) == nullptr || !valid_marriage(*this, row, false)) {
        return false;
    }
    replace_row(family_.marriages, row, &family::Marriage::id);
    return true;
}

std::optional<family::OutcomeSequence> World::append_family_outcome(family::NegotiationOutcome row) {
    if (row.seq != 0 || !allocatable(family_.next_outcome_seq) || !valid_outcome(*this, row)) {
        return std::nullopt;
    }
    row.seq = family_.next_outcome_seq;
    if (family_.outcomes.size() == family::kNegotiationOutcomeRingSize) {
        family_.outcomes.erase(family_.outcomes.begin());
    }
    family_.outcomes.push_back(row);
    ++family_.next_outcome_seq;
    return row.seq;
}

bool World::set_family_status(const family::PersonStatus& row) {
    if (!valid_status(*this, row)) {
        return false;
    }
    if (status_default(row)) {
        (void)clear_family_status(row.character);
    } else {
        replace_row(family_.person_statuses, row, &family::PersonStatus::character);
    }
    return true;
}

bool World::clear_family_status(CharacterId character) {
    return erase_row(family_.person_statuses, character, &family::PersonStatus::character);
}

bool World::set_family_succession(const family::SuccessionState& row) {
    if (!valid_succession(*this, row)) {
        return false;
    }
    if (succession_default(row)) {
        (void)clear_family_succession(row.dynasty);
    } else {
        replace_row(family_.succession, row, &family::SuccessionState::dynasty);
    }
    return true;
}

bool World::clear_family_succession(DynastyId dynasty) {
    return erase_row(family_.succession, dynasty, &family::SuccessionState::dynasty);
}

bool World::set_family_regency(const family::Regency& row) {
    if (!valid_regency(*this, row, false)) {
        return false;
    }
    replace_row(family_.regencies, row, &family::Regency::dynasty);
    return true;
}

bool World::clear_family_regency(DynastyId dynasty) {
    return erase_row(family_.regencies, dynasty, &family::Regency::dynasty);
}

bool World::set_family_dispute(const family::Dispute& row) {
    if (!valid_dispute(*this, row, family_dispute(row.dynasty) != nullptr)) {
        return false;
    }
    const auto* regency = family_regency(row.dynasty);
    if (regency != nullptr && caretaker_of(*this, row) != regency->ward) {
        return false;
    }
    replace_row(family_.disputes, row, &family::Dispute::dynasty);
    return true;
}

bool World::clear_family_dispute(DynastyId dynasty) {
    const auto* regency = family_regency(dynasty);
    if (regency != nullptr && dynasties_.head[dynasty.index()] != regency->ward) {
        return false;
    }
    return erase_row(family_.disputes, dynasty, &family::Dispute::dynasty);
}

bool World::set_family_land_grant(const family::LandGrant& row) {
    if (!valid_grant(*this, row, false)) {
        return false;
    }
    replace_row(family_.land_grants, row, &family::LandGrant::province);
    return true;
}

bool World::clear_family_land_grant(ProvinceId province) {
    return erase_row(family_.land_grants, province, &family::LandGrant::province);
}

bool World::set_family_godparent_cursor(std::uint32_t cursor) {
    if (!dynasty_mode() || cursor > characters_.ids.capacity()) {
        return false;
    }
    family_.godparent_cursor = cursor;
    return true;
}

void World::family_on_death(CharacterId character) {
    for (const auto& row : family_.negotiations) {
        if (row.groom == character || row.bride == character) {
            family::NegotiationOutcome outcome;
            outcome.negotiation = row.id;
            outcome.initiator = row.initiator;
            outcome.groom = row.groom;
            outcome.bride = row.bride;
            outcome.responder = row.initiator == row.groom_house ? row.bride_house : row.groom_house;
            outcome.sent = row.sent;
            outcome.answered = today();
            outcome.result = family::NegotiationResult::Void;
            outcome.reason = 1;  // MarriageDenial::NotAlive (E8-T04).
            (void)append_family_outcome(outcome);
        }
    }
    std::erase_if(family_.negotiations,
                  [&](const auto& row) { return row.groom == character || row.bride == character; });
    std::erase_if(family_.betrothals,
                  [&](const auto& row) { return row.groom == character || row.bride == character; });
    std::erase_if(family_.regencies, [&](const auto& row) { return row.ward == character; });
    const auto position = row_position(family_.person_statuses, character, &family::PersonStatus::character);
    if (position != family_.person_statuses.end() && position->character == character) {
        position->widowed_on = kNoDate;
        if (status_default(*position)) {
            family_.person_statuses.erase(position);
        }
    }
}

void World::check_family_invariants(std::vector<std::string>& bad) const {
    const auto fail = [&](const char* message) { bad.emplace_back(message); };
    const bool counters_valid = family_.next_negotiation_id > 0 && family_.next_betrothal_id > 0 &&
                                family_.next_marriage_id > 0 && family_.next_outcome_seq > 0;
    if (!counters_valid || family_.godparent_cursor > characters_.ids.capacity()) {
        fail("invalid family counters");
    }
    if (!dynasty_mode() && family_ != family::State{}) {
        fail("family state outside dynasty mode");
    }
    if (family_.negotiations.size() > family::kMaxNegotiations ||
        family_.betrothals.size() > family::kMaxBetrothals ||
        family_.marriages.size() > family::kMaxMarriageRecords ||
        family_.outcomes.size() > family::kNegotiationOutcomeRingSize) {
        fail("family table exceeds its bound");
    }
    if (!ordered_unique(family_.negotiations, &family::Negotiation::id) ||
        !ordered_unique(family_.betrothals, &family::Betrothal::id) ||
        !ordered_unique(family_.marriages, &family::Marriage::id) ||
        !ordered_unique(family_.outcomes, &family::NegotiationOutcome::seq) ||
        !ordered_unique(family_.person_statuses, &family::PersonStatus::character) ||
        !ordered_unique(family_.succession, &family::SuccessionState::dynasty) ||
        !ordered_unique(family_.regencies, &family::Regency::dynasty) ||
        !ordered_unique(family_.disputes, &family::Dispute::dynasty) ||
        !ordered_unique(family_.land_grants, &family::LandGrant::province)) {
        fail("family table keys are not unique and ordered");
        return;  // The lookup helpers require ordered tables.
    }
    for (const auto& row : family_.negotiations) {
        if (!allocated_id(row.id, family_.next_negotiation_id) || !valid_negotiation(*this, row)) {
            fail("invalid family negotiation");
        }
    }
    for (const auto& row : family_.betrothals) {
        if (!allocated_id(row.id, family_.next_betrothal_id) || !valid_betrothal(*this, row)) {
            fail("invalid family betrothal");
        }
    }
    for (const auto& row : family_.marriages) {
        if (!allocated_id(row.id, family_.next_marriage_id) || !valid_marriage(*this, row, true)) {
            fail("invalid family marriage record");
        }
    }
    for (const auto& row : family_.outcomes) {
        if (!allocated_id(row.seq, family_.next_outcome_seq) || !valid_outcome(*this, row)) {
            fail("invalid family negotiation outcome");
        }
    }
    for (const auto& row : family_.person_statuses) {
        if (status_default(row) || !valid_status(*this, row)) {
            fail("invalid family person status");
        }
    }
    for (const auto& row : family_.succession) {
        if (succession_default(row) || !valid_succession(*this, row)) {
            fail("invalid family succession row");
        }
    }
    for (const auto& row : family_.regencies) {
        if (!valid_regency(*this, row, true)) {
            fail("invalid family regency");
        }
    }
    for (const auto& row : family_.disputes) {
        if (!valid_dispute(*this, row, true)) {
            fail("invalid family dispute");
        }
    }
    for (const auto& row : family_.land_grants) {
        if (!valid_grant(*this, row, true)) {
            fail("invalid family land grant");
        }
    }
    // M-12: family lookups are computed from these primary tables, so no separate index can stale.
}

}  // namespace pat::sim
