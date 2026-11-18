#include "pat/sim/marriage_text.hpp"

namespace pat::sim {

std::string_view marriage_negotiation_result_key(family::NegotiationResult result) noexcept {
    switch (result) {
        case family::NegotiationResult::Accepted:
            return "marriage.negotiation.accepted";
        case family::NegotiationResult::Refused:
            return "marriage.negotiation.refused";
        case family::NegotiationResult::Withdrawn:
            return "marriage.negotiation.withdrawn";
        case family::NegotiationResult::Void:
            return "marriage.negotiation.void";
    }
    return {};
}

std::string_view marriage_betrothal_origin_key(family::BetrothalOrigin origin) noexcept {
    switch (origin) {
        case family::BetrothalOrigin::Negotiation:
            return "marriage.betrothal.negotiation";
        case family::BetrothalOrigin::Command:
            return "marriage.betrothal.command";
    }
    return {};
}

std::string_view marriage_state_key(family::MarriageState state) noexcept {
    switch (state) {
        case family::MarriageState::Active:
            return "marriage.state.active";
        case family::MarriageState::Widowed:
            return "marriage.state.widowed";
        case family::MarriageState::Closed:
            return "marriage.state.closed";
    }
    return {};
}

std::string_view marriage_end_reason_key(family::MarriageEndReason reason) noexcept {
    switch (reason) {
        case family::MarriageEndReason::None:
            return "marriage.end.none";
        case family::MarriageEndReason::Widowhood:
            return "marriage.end.widowhood";
        case family::MarriageEndReason::Tonsure:
            return "marriage.end.tonsure";
        case family::MarriageEndReason::Annulment:
            return "marriage.end.annulment";
        case family::MarriageEndReason::BothDead:
            return "marriage.end.both_dead";
    }
    return {};
}

std::string_view marriage_chronicle_key(std::uint16_t param) noexcept {
    switch (param) {
        case 201:
            return "chronicle.family.envoy_sent";
        case 202:
            return "chronicle.family.marriage_accepted";
        case 203:
            return "chronicle.family.marriage_refused";
        case 204:
            return "chronicle.family.betrothed";
        case 205:
            return "chronicle.family.betrothal_broken";
        case 206:
            return "chronicle.family.married";
        case 207:
            return "chronicle.family.tonsured";
        case 208:
            return "chronicle.family.widowed";
        case kChronicleBetrothalPartyDied:
            return "chronicle.family.betrothal_party_died";
    }
    return {};
}

}  // namespace pat::sim
