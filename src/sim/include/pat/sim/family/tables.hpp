#pragma once

#include "pat/sim/date.hpp"
#include "pat/sim/economy/money.hpp"
#include "pat/sim/entity_ids.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

// World-owned family state (E8-T02, docs/dev/marriage-succession-model.md sections 4.1-4.10).
// Sparse tables contain rows in ascending key order. Outcomes keep the last 64 entries in ascending
// sequence order. World owns all mutations, bounds and reference checks; these types contain no rules.

namespace pat::sim::family {

using NegotiationId = std::uint32_t;
using BetrothalId = std::uint32_t;
using MarriageId = std::uint32_t;
using OutcomeSequence = std::uint32_t;

inline constexpr std::size_t kMaxNegotiations = 512;
inline constexpr std::size_t kMaxBetrothals = 1024;
inline constexpr std::size_t kMaxMarriageRecords = 4096;
inline constexpr std::size_t kNegotiationOutcomeRingSize = 64;
inline constexpr std::size_t kNegotiationScoreTerms = 8;
inline constexpr std::size_t kMaxDisputeClaimants = 4;

// Numeric values are stable: the E8 save codec will store their underlying bytes.
enum class NegotiationState : std::uint8_t { Pending = 1 };

enum class NegotiationResult : std::uint8_t {
    Accepted = 1,
    Refused = 2,
    Withdrawn = 3,
    Void = 4,
};

enum class BetrothalOrigin : std::uint8_t { Negotiation = 1, Command = 2 };

enum class MarriageState : std::uint8_t { Active = 1, Widowed = 2, Closed = 3 };

enum class MarriageEndReason : std::uint8_t {
    None = 0,
    Widowhood = 1,
    Tonsure = 2,
    Annulment = 3,
    BothDead = 4,
};

inline constexpr std::uint8_t kPersonTonsured = 1U << 0;
inline constexpr std::uint8_t kPersonDisowned = 1U << 1;
inline constexpr std::uint8_t kPersonStatusFlags = kPersonTonsured | kPersonDisowned;

enum class RegencyKind : std::uint8_t {
    Mother = 1,
    Guardian = 2,
    Uncle = 3,
    Council = 4,
    Imperial = 5,
};

enum class DisputeReason : std::uint8_t {
    CloseScores = 1,
    UnpopularHeir = 2,
    NoAppointment = 3,
};

/// An ongoing negotiation, keyed by id. Completed negotiations leave this table.
struct Negotiation {
    NegotiationId id = 0;
    DynastyId initiator;
    CharacterId groom;
    CharacterId bride;
    DynastyId groom_house;
    DynastyId bride_house;
    Date sent = kNoDate;
    Date answer_due = kNoDate;
    economy::Kopecks cost_kop = 0;
    economy::Kopecks dowry_kop = 0;
    DynastyId dowry_source;
    std::uint8_t dispensation = 0;
    std::uint8_t dowry_tier = 0;  ///< reserved for M2, always zero in M1
    NegotiationState state = NegotiationState::Pending;

    [[nodiscard]] bool operator==(const Negotiation&) const = default;
};

/// A preserved explanation of a completed negotiation, ordered by seq in the outcome ring.
struct NegotiationOutcome {
    OutcomeSequence seq = 0;
    NegotiationId negotiation = 0;
    DynastyId initiator;
    CharacterId groom;
    CharacterId bride;
    DynastyId responder;
    Date sent = kNoDate;
    Date answered = kNoDate;
    NegotiationResult result = NegotiationResult::Accepted;
    std::uint16_t reason = 0;
    std::int32_t score = 0;
    std::int32_t threshold = 0;
    std::array<std::int32_t, kNegotiationScoreTerms> terms{};

    [[nodiscard]] bool operator==(const NegotiationOutcome&) const = default;
};

/// An active betrothal, keyed by id. No dowry has been transferred yet.
struct Betrothal {
    BetrothalId id = 0;
    CharacterId groom;
    CharacterId bride;
    DynastyId groom_house;
    DynastyId bride_house;
    Date contracted = kNoDate;
    Date wedding_not_before = kNoDate;
    Date wedding_deadline = kNoDate;
    economy::Kopecks dowry_kop = 0;
    DynastyId dowry_source;
    MarriageId claim_marriage = 0;
    std::uint8_t dispensation = 0;
    BetrothalOrigin origin = BetrothalOrigin::Negotiation;

    [[nodiscard]] bool operator==(const Betrothal&) const = default;
};

/// A marriage recorded by E8, keyed by id; closed rows can remain for financial history.
struct Marriage {
    MarriageId id = 0;
    CharacterId groom;
    CharacterId bride;
    DynastyId groom_house;
    DynastyId bride_house;
    Date wedding = kNoDate;
    economy::Kopecks dowry_kop = 0;
    MarriageState state = MarriageState::Active;
    Date ended = kNoDate;
    MarriageEndReason end_reason = MarriageEndReason::None;
    economy::Kopecks claim_kop = 0;
    economy::Kopecks refunded_kop = 0;

    [[nodiscard]] bool operator==(const Marriage&) const = default;
};

/// Nondefault status facts, keyed by character; tonsure and disownment survive death.
struct PersonStatus {
    CharacterId character;
    std::uint8_t flags = 0;
    Date tonsured_on = kNoDate;
    Date disowned_on = kNoDate;
    Date widowed_on = kNoDate;

    [[nodiscard]] bool operator==(const PersonStatus&) const = default;
};

/// Nondefault succession choices, keyed by dynasty. The law and head remain in E7's dynasty table.
struct SuccessionState {
    DynastyId dynasty;
    CharacterId designated_heir;
    CharacterId guardian;
    Date law_changed_on = kNoDate;

    [[nodiscard]] bool operator==(const SuccessionState&) const = default;
};

/// One active regency per dynasty; an Imperial regency has no character in regent.
struct Regency {
    DynastyId dynasty;
    CharacterId ward;
    CharacterId regent;
    RegencyKind kind = RegencyKind::Mother;
    Date started = kNoDate;
    std::uint8_t checks_done = 0;
    std::uint16_t last_risk_permille = 0;
    CharacterId excluded;

    [[nodiscard]] bool operator==(const Regency&) const = default;
};

struct DisputeClaimant {
    CharacterId character;
    std::int32_t score = 0;

    [[nodiscard]] bool operator==(const DisputeClaimant&) const = default;
};

/// One active dispute per dynasty. The saved claimant order also determines temporary authority.
struct Dispute {
    DynastyId dynasty;
    CharacterId predecessor;
    DisputeReason reason = DisputeReason::CloseScores;
    Date opened = kNoDate;
    Date resolve_by = kNoDate;
    std::uint8_t claimants_n = 0;
    std::array<DisputeClaimant, kMaxDisputeClaimants> claimants{};

    [[nodiscard]] bool operator==(const Dispute&) const = default;
};

/// One grant per province, keyed by province; the province remains owned by its dynasty.
struct LandGrant {
    ProvinceId province;
    CharacterId holder;
    Date granted_on = kNoDate;

    [[nodiscard]] bool operator==(const LandGrant&) const = default;
};

using NegotiationTable = std::vector<Negotiation>;
using NegotiationOutcomeRing = std::vector<NegotiationOutcome>;
using BetrothalTable = std::vector<Betrothal>;
using MarriageTable = std::vector<Marriage>;
using PersonStatusTable = std::vector<PersonStatus>;
using SuccessionStateTable = std::vector<SuccessionState>;
using RegencyTable = std::vector<Regency>;
using DisputeTable = std::vector<Dispute>;
using LandGrantTable = std::vector<LandGrant>;

/// Primary family state only. Engagement and person-to-marriage indexes are derived by World.
struct State {
    NegotiationTable negotiations;
    NegotiationOutcomeRing outcomes;
    BetrothalTable betrothals;
    MarriageTable marriages;
    PersonStatusTable person_statuses;
    SuccessionStateTable succession;
    RegencyTable regencies;
    DisputeTable disputes;
    LandGrantTable land_grants;
    NegotiationId next_negotiation_id = 1;
    BetrothalId next_betrothal_id = 1;
    MarriageId next_marriage_id = 1;
    OutcomeSequence next_outcome_seq = 1;
    std::uint32_t godparent_cursor = 0;

    [[nodiscard]] bool operator==(const State&) const = default;
};

}  // namespace pat::sim::family
