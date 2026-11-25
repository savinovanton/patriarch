#pragma once

#include "pat/sim/economy/breakdown.hpp"
#include "pat/sim/family/tables.hpp"
#include "pat/sim/succession_rules.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace pat::sim {

class World;

/// E7's fallback for a vacant house after its head dies. E8 supplies the succession laws.
/// Returns an invalid id if the house has no eligible adult member.
[[nodiscard]] CharacterId fallback_successor(const World& world, DynastyId dynasty, CharacterId predecessor);

/// Stable reason codes (specification section 8.5); used by the later Succeed command and UI.
enum class HeirReason : std::uint8_t {
    EldestSon = 1,
    YoungestSon = 2,
    Appointed = 3,
    CouncilChoice = 4,
    SonLine = 5,
    Daughter = 6,
    DaughterLine = 7,
    Brother = 8,
    Nephew = 9,
    Kin = 10,
};

namespace succession_term {
inline constexpr std::uint16_t kCouncilScore = 11000;
inline constexpr std::uint16_t kSkills = 11001;
inline constexpr std::uint16_t kAge = 11002;
inline constexpr std::uint16_t kKinship = 11003;
inline constexpr std::uint16_t kOpinion = 11004;
inline constexpr std::uint16_t kLoyalty = 11005;
inline constexpr std::uint16_t kPiety = 11006;
inline constexpr std::uint16_t kStatus = 11007;
inline constexpr std::uint16_t kClaimantScore = 11100;
inline constexpr std::uint16_t kClaimantCouncil = 11101;
inline constexpr std::uint16_t kClaimantLawful = 11102;
inline constexpr std::uint16_t kClaimantAppointment = 11103;
}  // namespace succession_term

inline constexpr std::size_t kCouncilScoreTerms = 7;

/// Compact explanation, shared by a standalone score, the resolver and UI. Each weighted term
/// truncates toward zero; opinion_mean includes zero opinions of all other living adult members.
struct CouncilScore {
    std::array<std::int32_t, kCouncilScoreTerms> terms{};
    std::int32_t total = 0;
    std::int32_t opinion_mean = 0;
    std::int32_t support = 0;

    [[nodiscard]] bool operator==(const CouncilScore&) const = default;
};

struct SuccessionCandidate {
    CharacterId character;
    HeirReason reason = HeirReason::Kin;
    CouncilScore council;
    bool minor = false;
    bool female = false;

    [[nodiscard]] bool operator==(const SuccessionCandidate&) const = default;
};

struct SuccessionClaimant {
    CharacterId character;
    std::int32_t council_score = 0;
    std::int32_t lawful_bonus = 0;
    std::int32_t appointment_bonus = 0;
    std::int32_t score = 0;

    [[nodiscard]] bool operator==(const SuccessionClaimant&) const = default;
};

/// A proposal, not a family::Dispute row: NoAppointment may have just one claimant. T16 owns
/// the dates, persistence and temporary authority of an applied dispute.
struct SuccessionDispute {
    family::DisputeReason reason = family::DisputeReason::CloseScores;
    std::vector<SuccessionClaimant> claimants;

    [[nodiscard]] bool operator==(const SuccessionDispute&) const = default;
};

enum class SuccessionKind : std::uint8_t { NoCandidate, Successor, Dispute };

struct SuccessionResult {
    CharacterId predecessor;  ///< Explicit predecessor, or the last dead head in the chronicle.
    CharacterId winner;       ///< Invalid while a dispute is needed or no candidate is eligible.
    CharacterId lawful_heir;  ///< First in the lineage order, before council/designation overrides.
    std::vector<SuccessionCandidate> order;
    SuccessionKind kind = SuccessionKind::NoCandidate;
    std::optional<SuccessionDispute> dispute;
    bool minor = false;
    bool female_head = false;
    std::int32_t honor_delta = 0;

    [[nodiscard]] bool operator==(const SuccessionResult&) const = default;
};

/// Pure queries: no commands, state, random draws or policy installation. Partition ranks like
/// Majorat; E8-T18 allocates estates. Invalid inputs return an empty NoCandidate result.
[[nodiscard]] SuccessionResult resolve_successor(const World& world,
                                                 const SuccessionRules& rules,
                                                 DynastyId dynasty,
                                                 CharacterId predecessor = {});

/// Scores any living member, including a member currently excluded from inheriting; unknown,
/// archived or foreign candidates return zero. The resolver uses this same calculation in bulk.
[[nodiscard]] CouncilScore council_score(const World& world,
                                         const SuccessionRules& rules,
                                         DynastyId dynasty,
                                         CharacterId predecessor,
                                         CharacterId candidate,
                                         economy::Breakdown* explain = nullptr);
void explain_council_score(const CouncilScore& score, economy::Breakdown& explain);
void explain_claimant_score(const SuccessionClaimant& claimant, economy::Breakdown& explain);
[[nodiscard]] std::string_view succession_term_key(std::uint16_t code) noexcept;

/// Optional Simulation boundary (installation and tick integration belong to E8-T16).
class SuccessionPolicy {
public:
    virtual ~SuccessionPolicy() = default;
    [[nodiscard]] virtual SuccessionResult
    resolve(const World& world, DynastyId dynasty, CharacterId predecessor) const = 0;
};

class LawSuccessionPolicy final : public SuccessionPolicy {
public:
    explicit LawSuccessionPolicy(SuccessionRules rules) : rules_(rules) {}
    [[nodiscard]] SuccessionResult
    resolve(const World& world, DynastyId dynasty, CharacterId predecessor) const override;

private:
    SuccessionRules rules_;
};

}  // namespace pat::sim
