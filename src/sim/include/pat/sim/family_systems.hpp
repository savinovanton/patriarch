#pragma once

#include "pat/sim/family/tables.hpp"
#include "pat/sim/system.hpp"

#include <memory>
#include <optional>
#include <string_view>

namespace pat::sim {

class FaithPolicy;
struct MarriageRules;
class Simulation;

inline constexpr std::string_view kMarriageNegotiationsSystemName = "marriage.negotiations";
inline constexpr std::string_view kMarriageAnsweredActionName = "on_marriage_answered";

/// E11 on_action input projected from the authoritative, applied negotiation outcome. Void and
/// withdrawn negotiations do not answer a proposal and therefore produce no action fact.
struct MarriageAnswered {
    DynastyId initiator;
    DynastyId responder;
    CharacterId groom;
    CharacterId bride;
    bool accepted = false;
    std::uint16_t reason = 0;
    [[nodiscard]] bool operator==(const MarriageAnswered&) const = default;
};

[[nodiscard]] std::optional<MarriageAnswered>
marriage_answered_action(const family::NegotiationOutcome& outcome) noexcept;

/// Daily, ascending-id scan of pending negotiations, including invalidated membership in legacy
/// snapshots. Due answers are emitted for the next tick boundary; their payload preserves today's
/// decision and explanation.
[[nodiscard]] std::unique_ptr<System>
make_marriage_negotiations_system(std::shared_ptr<const MarriageRules> rules,
                                  std::shared_ptr<const FaithPolicy> faith = {});

/// Registers E8 systems separately from dynasty.v1, after dynasty.* and before economy.*. The same
/// rules and faith owners are retained by the system and the simulation's command context.
/// Null rules use MarriageRules::defaults(). Call before the first tick.
void install_family_systems(Simulation& simulation,
                            std::shared_ptr<const MarriageRules> rules,
                            std::shared_ptr<const FaithPolicy> faith = {});

}  // namespace pat::sim
