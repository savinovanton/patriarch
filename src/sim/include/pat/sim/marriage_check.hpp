#pragma once

#include "pat/sim/kinship.hpp"
#include "pat/sim/marriage_rules.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace pat::sim {

/// Stable reason codes (marriage-succession-model section 7.3). Only append new codes.
enum class MarriageDenial : std::uint16_t {
    NotAlive = 1,
    SamePerson = 2,
    SameSex = 3,
    SameHouse = 4,
    GroomTooYoung = 10,
    BrideTooYoung = 11,
    TooYoungToBetroth = 12,
    AlreadyMarried = 20,
    Betrothed = 21,
    InNegotiation = 22,
    Tonsured = 23,
    Disowned = 24,
    MarriageLimit = 25,
    PreviouslyMarried = 26,
    Mourning = 27,
    Pregnant = 28,
    BloodKinship = 30,
    BloodKinshipDispensable = 31,
    Affinity = 32,
    SpiritualKinship = 33,
    FaithMismatch = 40,
    ChurchConsentRequired = 41,  // Reserved for E31.
    NotYourMember = 60,
    HouseBusy = 61,
    InsufficientFunds = 62,
    DuplicateNegotiation = 63,
    RecentlyRefused = 64,
    NegotiationNotFound = 65,
    NotInitiator = 66,
    NoActingHead = 67,
    TableFull = 68,
    BetrothalNotFound = 70,
    NotBetrothalParty = 71,
    NotMarried = 80,
    CannotTonsureHead = 81,
    NotYourSpouse = 82,
};

/// Unique reasons in ascending numeric order. No reason is lost to a fixed-capacity buffer.
using MarriageDenials = std::vector<MarriageDenial>;

/// Read-only content adapter for E31. Missing/empty house faith entries mean "orthodox".
/// The caller owns the strings and the span for the lifetime of this policy. A future policy can
/// override allows(); it is not persistent simulation state.
class FaithPolicy {
public:
    explicit FaithPolicy(std::span<const std::string_view> faith_by_house = {})
        : faith_by_house_(faith_by_house) {}
    virtual ~FaithPolicy() = default;

    [[nodiscard]] virtual bool allows(const World& world, CharacterId a, CharacterId b) const;
    [[nodiscard]] virtual std::string_view faith_of(DynastyId house) const noexcept;

private:
    std::span<const std::string_view> faith_by_house_;
};

struct MarriageCheckOptions {
    bool as_betrothal = false;
    bool dispensation = false;
    /// Invalid = a system checking both houses. A house checks Disowned only for its own members.
    DynastyId actor;
};

/// Reuses the fixed member's eligibility and bounded kinship facts across a read-only request.
/// check() uses the same denial rules as check_marriage(); the fixed member is the groom when male,
/// otherwise the bride. The World, rules and faith policy must remain unchanged while in use.
class MarriageCheckContext {
public:
    MarriageCheckContext(const World& world,
                         const MarriageRules& rules,
                         CharacterId member,
                         MarriageCheckOptions options = {},
                         const FaithPolicy* faith = nullptr);
    ~MarriageCheckContext();
    MarriageCheckContext(const MarriageCheckContext&) = delete;
    MarriageCheckContext& operator=(const MarriageCheckContext&) = delete;
    [[nodiscard]] MarriageDenials check(CharacterId other, KinResult* kinship = nullptr);
    /// Whole years in this snapshot, reused by check(); zero for unknown ids.
    [[nodiscard]] std::int32_t age_of(CharacterId person);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// Bounded family queries, including archived relatives. Zero degrees mean no detected relation.
[[nodiscard]] KinResult kinship_for_marriage(const World& world, CharacterId a, CharacterId b);

/// Pure, complete eligibility query. Known archived people still contribute their retained facts;
/// unknown ids contribute NotAlive and are never indexed. No random draws, payments or mutations.
/// Rules must pass validate(); M1 keeps the structural sex and house restrictions enabled.
/// With dispensation only BloodKinshipDispensable is removed; envoy_cost() prices it separately.
/// When supplied, kinship receives the same result used for the denials (zero for unknown people).
[[nodiscard]] MarriageDenials check_marriage(const World& world,
                                             const MarriageRules& rules,
                                             CharacterId groom,
                                             CharacterId bride,
                                             MarriageCheckOptions options = {},
                                             const FaithPolicy* faith = nullptr,
                                             KinResult* kinship = nullptr);

/// The unavailable-candidate label prioritizes engagement, marriage, mourning and tonsure, then
/// chooses the smallest remaining code. The complete set remains unchanged.
[[nodiscard]] std::optional<MarriageDenial> candidate_denial(const MarriageDenials& denials);

/// Stable localisation key; an unknown/unissued number has no key. Texts are provided by E8-T07.
[[nodiscard]] std::string_view marriage_denial_key(MarriageDenial reason) noexcept;

}  // namespace pat::sim
