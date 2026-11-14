#include "pat/core/hash_stream.hpp"
#include "pat/sim/world.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

#include "state_sections.hpp"

// E8-T03. Version-1 sections 16..23 and extended hash parts 8..14.
// Shared emitters define both the little-endian file fields and their FNV byte representation.
// Loading writes raw rows: public mutators assign new ids and check today's house membership,
// whereas these houses are historical snapshots. World::load_state validates cross-table links
// only after the family, character and economy sections have all been read.

namespace pat::sim {
namespace {

using detail::plausible;

constexpr std::size_t kNegotiationRowBytes = 55;
constexpr std::size_t kOutcomeRowBytes = 75;
constexpr std::size_t kBetrothalRowBytes = 50;
constexpr std::size_t kMarriageRowBytes = 54;
constexpr std::size_t kStatusRowBytes = 17;
constexpr std::size_t kSuccessionRowBytes = 16;
constexpr std::size_t kGrantRowBytes = 12;
constexpr std::size_t kRegencyRowBytes = 24;
constexpr std::size_t kDisputeRowBytes = 50;

template <class Sink>
void write_row(Sink& out, const family::Negotiation& row) {
    out.u32(row.id);
    out.u32(row.initiator.value());
    out.u32(row.groom.value());
    out.u32(row.bride.value());
    out.u32(row.groom_house.value());
    out.u32(row.bride_house.value());
    out.i32(row.sent.days());
    out.i32(row.answer_due.days());
    out.i64(row.cost_kop);
    out.i64(row.dowry_kop);
    out.u32(row.dowry_source.value());
    out.u8(row.dispensation);
    out.u8(row.dowry_tier);
    out.u8(static_cast<std::uint8_t>(row.state));
}

template <class Sink>
void write_row(Sink& out, const family::NegotiationOutcome& row) {
    out.u32(row.seq);
    out.u32(row.negotiation);
    out.u32(row.initiator.value());
    out.u32(row.groom.value());
    out.u32(row.bride.value());
    out.u32(row.responder.value());
    out.i32(row.sent.days());
    out.i32(row.answered.days());
    out.u8(static_cast<std::uint8_t>(row.result));
    out.u16(row.reason);
    out.i32(row.score);
    out.i32(row.threshold);
    for (const auto term : row.terms) {
        out.i32(term);
    }
}

template <class Sink>
void write_row(Sink& out, const family::Betrothal& row) {
    out.u32(row.id);
    out.u32(row.groom.value());
    out.u32(row.bride.value());
    out.u32(row.groom_house.value());
    out.u32(row.bride_house.value());
    out.i32(row.contracted.days());
    out.i32(row.wedding_not_before.days());
    out.i32(row.wedding_deadline.days());
    out.i64(row.dowry_kop);
    out.u32(row.dowry_source.value());
    out.u32(row.claim_marriage);
    out.u8(row.dispensation);
    out.u8(static_cast<std::uint8_t>(row.origin));
}

template <class Sink>
void write_row(Sink& out, const family::Marriage& row) {
    out.u32(row.id);
    out.u32(row.groom.value());
    out.u32(row.bride.value());
    out.u32(row.groom_house.value());
    out.u32(row.bride_house.value());
    out.i32(row.wedding.days());
    out.i64(row.dowry_kop);
    out.u8(static_cast<std::uint8_t>(row.state));
    out.i32(row.ended.days());
    out.u8(static_cast<std::uint8_t>(row.end_reason));
    out.i64(row.claim_kop);
    out.i64(row.refunded_kop);
}

template <class Sink>
void write_row(Sink& out, const family::PersonStatus& row) {
    out.u32(row.character.value());
    out.u8(row.flags);
    out.i32(row.tonsured_on.days());
    out.i32(row.disowned_on.days());
    out.i32(row.widowed_on.days());
}

template <class Sink>
void write_row(Sink& out, const family::SuccessionState& row) {
    out.u32(row.dynasty.value());
    out.u32(row.designated_heir.value());
    out.u32(row.guardian.value());
    out.i32(row.law_changed_on.days());
}

template <class Sink>
void write_row(Sink& out, const family::LandGrant& row) {
    out.u32(row.province.value());
    out.u32(row.holder.value());
    out.i32(row.granted_on.days());
}

template <class Sink>
void write_row(Sink& out, const family::Regency& row) {
    out.u32(row.dynasty.value());
    out.u32(row.ward.value());
    out.u32(row.regent.value());
    out.u8(static_cast<std::uint8_t>(row.kind));
    out.i32(row.started.days());
    out.u8(row.checks_done);
    out.u16(row.last_risk_permille);
    out.u32(row.excluded.value());
}

template <class Sink>
void write_row(Sink& out, const family::Dispute& row) {
    out.u32(row.dynasty.value());
    out.u32(row.predecessor.value());
    out.u8(static_cast<std::uint8_t>(row.reason));
    out.i32(row.opened.days());
    out.i32(row.resolve_by.days());
    out.u8(row.claimants_n);
    for (const auto& claimant : row.claimants) {
        out.u32(claimant.character.value());
        out.i32(claimant.score);
    }
}

template <class Sink, class Rows>
void write_rows(Sink& out, const Rows& rows) {
    out.u32(static_cast<std::uint32_t>(rows.size()));
    for (const auto& row : rows) {
        write_row(out, row);
    }
}

template <class Sink>
void write_counters(Sink& out, const family::State& state) {
    out.u32(state.next_negotiation_id);
    out.u32(state.next_betrothal_id);
    out.u32(state.next_marriage_id);
    out.u32(state.next_outcome_seq);
}

template <class Sink>
void write_body(Sink& out, std::uint16_t id, const family::State& state) {
    switch (id) {
        case detail::kSectionFamilyCounters:
            write_counters(out, state);
            break;
        case detail::kSectionNegotiations:
            write_rows(out, state.negotiations);
            write_rows(out, state.outcomes);
            break;
        case detail::kSectionBetrothals:
            write_rows(out, state.betrothals);
            break;
        case detail::kSectionMarriages:
            write_rows(out, state.marriages);
            break;
        case detail::kSectionPersonStatus:
            write_rows(out, state.person_statuses);
            break;
        case detail::kSectionSuccession:
            out.u32(state.godparent_cursor);
            write_rows(out, state.succession);
            write_rows(out, state.land_grants);
            break;
        case detail::kSectionRegency:
            write_rows(out, state.regencies);
            break;
        case detail::kSectionDispute:
            write_rows(out, state.disputes);
            break;
        default:
            break;
    }
}

bool read_row(core::ByteReader& in, family::Negotiation& row) {
    row.id = in.u32();
    row.initiator = DynastyId(in.u32());
    row.groom = CharacterId(in.u32());
    row.bride = CharacterId(in.u32());
    row.groom_house = DynastyId(in.u32());
    row.bride_house = DynastyId(in.u32());
    row.sent = Date::from_days(in.i32());
    row.answer_due = Date::from_days(in.i32());
    row.cost_kop = in.i64();
    row.dowry_kop = in.i64();
    row.dowry_source = DynastyId(in.u32());
    row.dispensation = in.u8();
    row.dowry_tier = in.u8();
    row.state = static_cast<family::NegotiationState>(in.u8());
    return in.ok() && row.id > 0 && row.dispensation <= 1 && row.dowry_tier == 0 &&
           row.state == family::NegotiationState::Pending;
}

bool read_row(core::ByteReader& in, family::NegotiationOutcome& row) {
    row.seq = in.u32();
    row.negotiation = in.u32();
    row.initiator = DynastyId(in.u32());
    row.groom = CharacterId(in.u32());
    row.bride = CharacterId(in.u32());
    row.responder = DynastyId(in.u32());
    row.sent = Date::from_days(in.i32());
    row.answered = Date::from_days(in.i32());
    row.result = static_cast<family::NegotiationResult>(in.u8());
    row.reason = in.u16();
    row.score = in.i32();
    row.threshold = in.i32();
    for (auto& term : row.terms) {
        term = in.i32();
    }
    return in.ok() && row.seq > 0 && row.negotiation > 0 &&
           row.result >= family::NegotiationResult::Accepted && row.result <= family::NegotiationResult::Void;
}

bool read_row(core::ByteReader& in, family::Betrothal& row) {
    row.id = in.u32();
    row.groom = CharacterId(in.u32());
    row.bride = CharacterId(in.u32());
    row.groom_house = DynastyId(in.u32());
    row.bride_house = DynastyId(in.u32());
    row.contracted = Date::from_days(in.i32());
    row.wedding_not_before = Date::from_days(in.i32());
    row.wedding_deadline = Date::from_days(in.i32());
    row.dowry_kop = in.i64();
    row.dowry_source = DynastyId(in.u32());
    row.claim_marriage = in.u32();
    row.dispensation = in.u8();
    row.origin = static_cast<family::BetrothalOrigin>(in.u8());
    return in.ok() && row.id > 0 && row.dispensation <= 1 &&
           (row.origin == family::BetrothalOrigin::Negotiation ||
            row.origin == family::BetrothalOrigin::Command);
}

bool read_row(core::ByteReader& in, family::Marriage& row) {
    row.id = in.u32();
    row.groom = CharacterId(in.u32());
    row.bride = CharacterId(in.u32());
    row.groom_house = DynastyId(in.u32());
    row.bride_house = DynastyId(in.u32());
    row.wedding = Date::from_days(in.i32());
    row.dowry_kop = in.i64();
    row.state = static_cast<family::MarriageState>(in.u8());
    row.ended = Date::from_days(in.i32());
    row.end_reason = static_cast<family::MarriageEndReason>(in.u8());
    row.claim_kop = in.i64();
    row.refunded_kop = in.i64();
    return in.ok() && row.id > 0 && row.state >= family::MarriageState::Active &&
           row.state <= family::MarriageState::Closed && row.end_reason >= family::MarriageEndReason::None &&
           row.end_reason <= family::MarriageEndReason::BothDead;
}

bool read_row(core::ByteReader& in, family::PersonStatus& row) {
    row.character = CharacterId(in.u32());
    row.flags = in.u8();
    row.tonsured_on = Date::from_days(in.i32());
    row.disowned_on = Date::from_days(in.i32());
    row.widowed_on = Date::from_days(in.i32());
    return in.ok() && row.character.valid() && (row.flags & ~family::kPersonStatusFlags) == 0;
}

bool read_row(core::ByteReader& in, family::SuccessionState& row) {
    row.dynasty = DynastyId(in.u32());
    row.designated_heir = CharacterId(in.u32());
    row.guardian = CharacterId(in.u32());
    row.law_changed_on = Date::from_days(in.i32());
    return in.ok() && row.dynasty.valid();
}

bool read_row(core::ByteReader& in, family::LandGrant& row) {
    row.province = ProvinceId(in.u32());
    row.holder = CharacterId(in.u32());
    row.granted_on = Date::from_days(in.i32());
    return in.ok() && row.province.valid();
}

bool read_row(core::ByteReader& in, family::Regency& row) {
    row.dynasty = DynastyId(in.u32());
    row.ward = CharacterId(in.u32());
    row.regent = CharacterId(in.u32());
    row.kind = static_cast<family::RegencyKind>(in.u8());
    row.started = Date::from_days(in.i32());
    row.checks_done = in.u8();
    row.last_risk_permille = in.u16();
    row.excluded = CharacterId(in.u32());
    return in.ok() && row.dynasty.valid() && row.kind >= family::RegencyKind::Mother &&
           row.kind <= family::RegencyKind::Imperial;
}

bool read_row(core::ByteReader& in, family::Dispute& row) {
    row.dynasty = DynastyId(in.u32());
    row.predecessor = CharacterId(in.u32());
    row.reason = static_cast<family::DisputeReason>(in.u8());
    row.opened = Date::from_days(in.i32());
    row.resolve_by = Date::from_days(in.i32());
    row.claimants_n = in.u8();
    for (auto& claimant : row.claimants) {
        claimant.character = CharacterId(in.u32());
        claimant.score = in.i32();
    }
    return in.ok() && row.dynasty.valid() && row.reason >= family::DisputeReason::CloseScores &&
           row.reason <= family::DisputeReason::NoAppointment && row.claimants_n >= 2 &&
           row.claimants_n <= family::kMaxDisputeClaimants;
}

template <class Rows, class Projection>
bool read_rows(
    core::ByteReader& in, Rows& rows, std::size_t limit, std::size_t row_bytes, Projection projection) {
    const std::uint32_t count = in.u32();
    if (count > limit || !plausible(in, count, row_bytes)) {
        return false;
    }
    using Row = typename Rows::value_type;
    using Key = std::remove_cvref_t<std::invoke_result_t<Projection, Row>>;
    std::optional<Key> previous;
    rows.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        Row row;
        if (!read_row(in, row)) {
            return false;
        }
        const Key key = std::invoke(projection, row);
        if (previous && key <= *previous) {
            return false;
        }
        previous = key;
        rows.push_back(std::move(row));
    }
    return true;
}

std::uint64_t nonzero(std::uint64_t hash) {
    return hash == 0 ? 1 : hash;
}

}  // namespace

bool World::family_section_present(std::uint16_t id) const {
    switch (id) {
        case detail::kSectionFamilyCounters:
            return family_.next_negotiation_id != 1 || family_.next_betrothal_id != 1 ||
                   family_.next_marriage_id != 1 || family_.next_outcome_seq != 1;
        case detail::kSectionNegotiations:
            return !family_.negotiations.empty() || !family_.outcomes.empty();
        case detail::kSectionBetrothals:
            return !family_.betrothals.empty();
        case detail::kSectionMarriages:
            return !family_.marriages.empty();
        case detail::kSectionPersonStatus:
            return !family_.person_statuses.empty();
        case detail::kSectionSuccession:
            return family_.godparent_cursor != 0 || !family_.succession.empty() ||
                   !family_.land_grants.empty();
        case detail::kSectionRegency:
            return !family_.regencies.empty();
        case detail::kSectionDispute:
            return !family_.disputes.empty();
        default:
            return false;
    }
}

void World::family_write_sections(std::vector<StateSection>& sections) const {
    for (std::uint16_t id = detail::kSectionFamilyCounters; id <= detail::kSectionDispute; ++id) {
        if (family_section_present(id)) {
            StateSection section{id, 1, {}};
            write_body(section.body, id, family_);
            sections.push_back(std::move(section));
        }
    }
}

const char* World::family_read_section(std::uint16_t id, core::ByteReader& in) {
    switch (id) {
        case detail::kSectionFamilyCounters: {
            const auto negotiation = in.u32();
            const auto betrothal = in.u32();
            const auto marriage = in.u32();
            const auto outcome = in.u32();
            if (!in.ok() || negotiation == 0 || betrothal == 0 || marriage == 0 || outcome == 0) {
                return "invalid family counters section";
            }
            family_.next_negotiation_id = negotiation;
            family_.next_betrothal_id = betrothal;
            family_.next_marriage_id = marriage;
            family_.next_outcome_seq = outcome;
            break;
        }
        case detail::kSectionNegotiations: {
            family::NegotiationTable negotiations;
            family::NegotiationOutcomeRing outcomes;
            if (!read_rows(in,
                           negotiations,
                           family::kMaxNegotiations,
                           kNegotiationRowBytes,
                           &family::Negotiation::id) ||
                !read_rows(in,
                           outcomes,
                           family::kNegotiationOutcomeRingSize,
                           kOutcomeRowBytes,
                           &family::NegotiationOutcome::seq)) {
                return "invalid family negotiations or outcomes section";
            }
            family_.negotiations = std::move(negotiations);
            family_.outcomes = std::move(outcomes);
            break;
        }
        case detail::kSectionBetrothals: {
            family::BetrothalTable rows;
            if (!read_rows(in, rows, family::kMaxBetrothals, kBetrothalRowBytes, &family::Betrothal::id)) {
                return "invalid family betrothals section";
            }
            family_.betrothals = std::move(rows);
            break;
        }
        case detail::kSectionMarriages: {
            family::MarriageTable rows;
            if (!read_rows(in, rows, family::kMaxMarriageRecords, kMarriageRowBytes, &family::Marriage::id)) {
                return "invalid family marriages section";
            }
            family_.marriages = std::move(rows);
            break;
        }
        case detail::kSectionPersonStatus: {
            family::PersonStatusTable rows;
            if (!read_rows(in,
                           rows,
                           characters_.ids.capacity(),
                           kStatusRowBytes,
                           &family::PersonStatus::character)) {
                return "invalid family person status section";
            }
            family_.person_statuses = std::move(rows);
            break;
        }
        case detail::kSectionSuccession: {
            const auto cursor = in.u32();
            family::SuccessionStateTable choices;
            family::LandGrantTable grants;
            if (!in.ok() || cursor > characters_.ids.capacity() ||
                !read_rows(
                    in, choices, dynasties_.size(), kSuccessionRowBytes, &family::SuccessionState::dynasty) ||
                !read_rows(in, grants, provinces_.size(), kGrantRowBytes, &family::LandGrant::province)) {
                return "invalid family succession section";
            }
            family_.godparent_cursor = cursor;
            family_.succession = std::move(choices);
            family_.land_grants = std::move(grants);
            break;
        }
        case detail::kSectionRegency: {
            family::RegencyTable rows;
            if (!read_rows(in, rows, dynasties_.size(), kRegencyRowBytes, &family::Regency::dynasty)) {
                return "invalid family regency section";
            }
            family_.regencies = std::move(rows);
            break;
        }
        case detail::kSectionDispute: {
            family::DisputeTable rows;
            if (!read_rows(in, rows, dynasties_.size(), kDisputeRowBytes, &family::Dispute::dynasty)) {
                return "invalid family dispute section";
            }
            family_.disputes = std::move(rows);
            break;
        }
        default:
            return "unknown family section";
    }
    return in.ok() && in.at_end() ? nullptr : "a family section is truncated or has extra bytes";
}

std::array<std::uint64_t, 7> World::family_hash_parts() const {
    std::array<std::uint64_t, 7> parts{};
    if (family_section_present(detail::kSectionFamilyCounters) ||
        family_section_present(detail::kSectionNegotiations)) {
        core::HashStream hash;
        write_counters(hash, family_);
        write_body(hash, detail::kSectionNegotiations, family_);
        parts[0] = nonzero(hash.value());
    }
    constexpr std::array<std::uint16_t, 6> sections = {detail::kSectionBetrothals,
                                                       detail::kSectionMarriages,
                                                       detail::kSectionPersonStatus,
                                                       detail::kSectionSuccession,
                                                       detail::kSectionRegency,
                                                       detail::kSectionDispute};
    for (std::size_t i = 0; i < sections.size(); ++i) {
        if (family_section_present(sections[i])) {
            core::HashStream hash;
            write_body(hash, sections[i], family_);
            parts[i + 1] = nonzero(hash.value());
        }
    }
    return parts;
}

}  // namespace pat::sim
