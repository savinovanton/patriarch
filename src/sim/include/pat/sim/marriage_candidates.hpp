#pragma once

#include "pat/core/string_interner.hpp"
#include "pat/sim/marriage_check.hpp"
#include "pat/sim/marriage_score.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace pat::sim {

enum class CandidateAvailability : std::uint8_t { All, Available, Unavailable };
enum class CandidateSort : std::uint8_t { MatchValue, Relation, Age };

/// Read-only search inputs. The proposing house is the house of member. By default the pool is
/// every other living, housed person of the opposite sex, including unavailable people. An explicit
/// sex replaces that pool filter. No estate column exists: these dynasty-mode people are nobles.
/// Optional age/rank bounds are inclusive; inverted bounds yield an empty list.
struct CandidateQuery {
    CharacterId member;
    std::optional<Sex> sex;
    DynastyId house;  ///< Invalid = all houses.
    std::optional<std::int32_t> min_age;
    std::optional<std::int32_t> max_age;
    std::optional<std::int32_t> min_rank;
    std::optional<std::int32_t> max_rank;
    /// Substring of a person's name or house name. ASCII and Russian letters are compared without
    /// case or locale dependence. Other characters are compared exactly. No name table = no match
    /// for a nonempty text filter; invalid StringIds are treated as empty names.
    std::string_view text;
    const core::StringInterner* names = nullptr;
    /// Empty = all faiths. The default policy gives every house the faith "orthodox".
    std::string_view faith;
    const FaithPolicy* faith_policy = nullptr;
    bool unrelated_only = false;  ///< No detected blood, affinity or spiritual relation.
    bool as_betrothal = false;
    bool dispensation = false;
    CandidateAvailability availability = CandidateAvailability::All;
    CandidateSort sort = CandidateSort::MatchValue;
    bool ascending = false;  ///< Applies to the selected key; ties always use ascending person id.
};

struct MarriageCandidate {
    CharacterId person;
    DynastyId house;
    std::int32_t age = 0;
    std::int32_t rank = 0;
    /// Refers to the policy's caller-owned strings or to static default storage.
    std::string_view faith;
    KinResult kinship;
    MarriageDenials denials;
    std::optional<MarriageDenial> denial;  ///< candidate_denial(denials), for the primary badge.
    bool dispensable = false;
    std::int32_t relation = 0;
    economy::Kopecks dowry_kop = 0;
    /// The proposing house's complete eight terms; explain_accept_score() builds its tooltip.
    AcceptScore score;
    std::int32_t match_value = 0;           ///< Score from the proposing member's house.
    Forecast forecast = Forecast::Unclear;  ///< Public forecast of the answering house's score.

    [[nodiscard]] bool available() const noexcept { return denials.empty(); }
    [[nodiscard]] bool operator==(const MarriageCandidate&) const = default;
};

struct CandidateList {
    std::vector<MarriageCandidate> rows;
    /// Counts after the sex/house/text/faith/age/rank/kinship filters, before the availability filter.
    std::size_t total = 0;
    std::size_t available_count = 0;

    [[nodiscard]] bool operator==(const CandidateList&) const = default;
};

/// Pure query: no commands, persistent caches, RNG draws or changes to World/rules/name tables.
/// Invalid/dead/unhoused members and non-dynasty worlds return an empty list. Each row retains the
/// complete check_marriage result with actor = the member's house and the query's options/policy.
/// Expensive house relations are reused within this call. Explanations are built on demand from
/// the stored score by explain_accept_score(), rather than allocating a Breakdown for every row.
[[nodiscard]] CandidateList
find_candidates(const World& world, const MarriageRules& rules, const CandidateQuery& query);

}  // namespace pat::sim
