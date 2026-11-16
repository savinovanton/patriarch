#pragma once

#include "pat/sim/family/tables.hpp"
#include "pat/sim/marriage_check.hpp"

namespace pat::sim {

/// The living regent (or Imperial ward), dispute caretaker, or formal head, in that order.
/// The dynasty remains the command actor even when its regent belongs to another house.
[[nodiscard]] CharacterId family_acting_head(const World& world, DynastyId house);

/// Pure, complete, sorted eligibility and negotiation checks.
/// Rules must pass validate(), including the mandatory M1 structural restrictions.
/// Participants are checked for betrothal age; their sexes determine the stored groom and bride.
/// An invalid actor is the system: the groom's house initiates and pays, while both participants'
/// disownment is checked. Without economy the cost is zero and funds are never a refusal.
[[nodiscard]] MarriageDenials can_send_envoy(const World& world,
                                             const MarriageRules& rules,
                                             DynastyId actor,
                                             CharacterId groom,
                                             CharacterId bride,
                                             bool dispensation,
                                             const FaithPolicy* faith = nullptr);

/// Only the initiator may withdraw a pending negotiation; the system may withdraw any one.
/// Being due for an answer does not close a negotiation: the first applied command wins.
[[nodiscard]] MarriageDenials
can_withdraw_envoy(const World& world, DynastyId actor, family::NegotiationId negotiation);

/// Apply bookkeeping after successful validation against the same World. No repeated eligibility
/// query, random draw or response: snapshots the terms and posts the cost exactly once.
[[nodiscard]] family::NegotiationId send_marriage_envoy(World& world,
                                                        const MarriageRules& rules,
                                                        DynastyId actor,
                                                        CharacterId groom,
                                                        CharacterId bride,
                                                        bool dispensation,
                                                        const FaithPolicy* faith = nullptr);

/// Apply a validated withdrawal: retain a neutral Withdrawn outcome and post the configured refund.
void withdraw_marriage_envoy(World& world, const MarriageRules& rules, family::NegotiationId negotiation);

}  // namespace pat::sim
