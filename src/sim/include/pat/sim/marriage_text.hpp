#pragma once

#include "pat/sim/family/tables.hpp"

#include <cstdint>
#include <string_view>

namespace pat::sim {

// Shared presentation metadata for negotiation commands, the chronicle and family UI snapshots.
inline constexpr std::string_view kMarriageNegotiationPendingKey = "marriage.negotiation.pending";
inline constexpr std::string_view kMarriageBetrothalActiveKey = "marriage.betrothal.active";
inline constexpr std::string_view kMarriageBetrothalReadyKey = "marriage.betrothal.ready";
inline constexpr std::string_view kMarriageBetrothalBrokenKey = "marriage.betrothal.broken";

// Stable localisation keys; unknown enumeration values return an empty view.
[[nodiscard]] std::string_view marriage_negotiation_result_key(family::NegotiationResult result) noexcept;
[[nodiscard]] std::string_view marriage_betrothal_origin_key(family::BetrothalOrigin origin) noexcept;
[[nodiscard]] std::string_view marriage_state_key(family::MarriageState state) noexcept;
[[nodiscard]] std::string_view marriage_end_reason_key(family::MarriageEndReason reason) noexcept;

inline constexpr std::uint16_t kChronicleBetrothalPartyDied = 209;

// ChronicleKind::Note parameters 201...216 (marriage-succession-model.md section 10.2).
[[nodiscard]] std::string_view marriage_chronicle_key(std::uint16_t param) noexcept;

}  // namespace pat::sim
