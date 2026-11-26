#pragma once

#include "pat/sim/command.hpp"
#include "pat/sim/succession.hpp"

namespace pat::sim {

/// Stable specification section 7.4 codes. New structural reasons append after the issued block.
enum class SuccessionDenial : std::uint16_t {
    NoActingHead = 1,
    NotYourHouse = 2,
    DisputeOpen = 3,
    RegencyActive = 4,
    DynastyExtinct = 5,
    LawNotAppointment = 6,
    SameLaw = 7,
    LawNotSelectable = 8,
    ReformCooldown = 9,
    InsufficientFunds = 10,
    HeirNotMember = 11,
    HeirNotAlive = 12,
    HeirIneligible = 13,
    HeirIsHead = 14,
    HeirRequired = 15,
    CannotDisownHead = 16,
    AlreadyDisowned = 17,
    AlreadyTonsured = 18,
    NoRegency = 20,
    NoSupport = 21,
    ProvinceNotOwned = 22,
    HolderNotAdult = 23,
    HolderIneligible = 24,
    ProvinceHasNoEconomy = 25,
    TableFull = 26,
    InvalidGuardian = 27,
    ActiveRole = 28,
    NotAlive = 29,
    DynastyModeRequired = 30,
    MoneyPostingBlocked = 31,
};
using SuccessionDenials = std::vector<SuccessionDenial>;
using SuccessionCommand = std::variant<cmd::SetHeir, cmd::ChangeSuccessionLaw, cmd::Disown>;

inline constexpr StandingReason kStandingReasonLawReform = 201;
inline constexpr StandingReason kStandingReasonDisown = 202;
inline constexpr StandingReason kStandingReasonAppointmentSkip = 207;
inline constexpr OpinionReason kOpinionReasonLawReformLoser = 201;
inline constexpr OpinionReason kOpinionReasonDisowned = 202;

struct ReformLoser {
    CharacterId character;
    std::int32_t opinion_delta = 0;
    [[nodiscard]] bool operator==(const ReformLoser&) const = default;
};

struct ReformCost {
    std::int32_t honor_delta = 0;
    std::int32_t unity_delta = 0;
    economy::Kopecks money_kop = 0;
    [[nodiscard]] bool operator==(const ReformCost&) const = default;
};

struct LawChangePreview {
    SuccessionLaw from = SuccessionLaw::Majorat;
    SuccessionLaw to = SuccessionLaw::Majorat;
    ReformCost cost;
    std::vector<ReformLoser> losers;
    SuccessionResult before;
    SuccessionResult after;
    SuccessionDenials denials;
    bool needs_heir = false;
};

/// Pure complete sorted denials; validate and UI share exactly this request path.
[[nodiscard]] SuccessionDenials check_succession_command(const World& world,
                                                         const SuccessionRules& rules,
                                                         DynastyId actor,
                                                         const SuccessionCommand& command);
[[nodiscard]] core::Result<void, CommandError> validate_succession_command(const World& world,
                                                                           const SuccessionRules& rules,
                                                                           DynastyId actor,
                                                                           const SuccessionCommand& command);

/// Uses the resolver's law/designation override; never mutates or clones World.
/// Money and points have separate additive trees because their units cannot be summed.
[[nodiscard]] LawChangePreview preview_law_change(const World& world,
                                                  const SuccessionRules& rules,
                                                  DynastyId actor,
                                                  SuccessionLaw law,
                                                  CharacterId heir = {},
                                                  economy::Breakdown* points = nullptr,
                                                  economy::Breakdown* money = nullptr);

/// Bookkeeping only, after validation against the same snapshot.
void apply_succession_command(World& world,
                              const SuccessionRules& rules,
                              DynastyId actor,
                              const cmd::SetHeir& command);
void apply_succession_command(World& world,
                              const SuccessionRules& rules,
                              DynastyId actor,
                              const cmd::ChangeSuccessionLaw& command);
void apply_succession_command(World& world,
                              const SuccessionRules& rules,
                              DynastyId actor,
                              const cmd::Disown& command);
[[nodiscard]] std::string_view succession_denial_key(SuccessionDenial denial) noexcept;

}  // namespace pat::sim
