#include "pat/sim/crisis_text.hpp"

#include <array>
#include <cstddef>

namespace pat::sim::crisis {

namespace {

constexpr std::array<std::string_view, kComponentCount> kComponents{"crisis.component.fiscal",
                                                                    "crisis.component.war",
                                                                    "crisis.component.recruit",
                                                                    "crisis.component.mood",
                                                                    "crisis.component.court",
                                                                    "crisis.component.church",
                                                                    "crisis.component.clarity",
                                                                    "crisis.component.authority"};

constexpr std::array<std::string_view, kStabilityReasonCount> kReasons{
    "crisis.reason.taxes_heavy",       "crisis.reason.arrears",
    "crisis.reason.deficit",           "crisis.reason.war_fatigue",
    "crisis.reason.recruit_heavy",     "crisis.reason.recruit_short",
    "crisis.reason.estates_unhappy",   "crisis.reason.court_opposition",
    "crisis.reason.known_plots",       "crisis.reason.purge_heat",
    "crisis.reason.church_conflict",   "crisis.reason.throne_vacant",
    "crisis.reason.throne_disputed",   "crisis.reason.heir_appointed",
    "crisis.reason.heir_lawful",       "crisis.reason.heir_minor",
    "crisis.reason.no_heirs",          "crisis.reason.rival_claims",
    "crisis.reason.emperor_authority", "crisis.reason.legacy"};

constexpr std::array<std::string_view, kPhaseCount> kPhaseNames{"crisis.phase.smoulder.name",
                                                                "crisis.phase.unrest.name",
                                                                "crisis.phase.plots.name",
                                                                "crisis.phase.coup_window.name"};
constexpr std::array<std::string_view, kPhaseCount> kPhaseHints{"crisis.phase.smoulder.hint",
                                                                "crisis.phase.unrest.hint",
                                                                "crisis.phase.plots.hint",
                                                                "crisis.phase.coup_window.hint"};
constexpr std::array<std::string_view, 4> kBands{
    "crisis.band.order", "crisis.band.tension", "crisis.band.crisis", "crisis.band.collapse"};

}  // namespace

std::string_view component_key(Component component) noexcept {
    const auto index = static_cast<std::size_t>(component);
    return index < kComponents.size() ? kComponents[index] : std::string_view{};
}

std::string_view legacy_component_key() noexcept {
    return "crisis.component.legacy";
}

std::string_view stability_reason_key(StabilityReason reason) noexcept {
    const auto code = static_cast<std::size_t>(reason);
    return code >= 1 && code <= kReasons.size() ? kReasons[code - 1] : std::string_view{};
}

std::string_view phase_name_key(std::uint8_t phase) noexcept {
    return phase < kPhaseNames.size() ? kPhaseNames[phase] : std::string_view{};
}

std::string_view phase_hint_key(std::uint8_t phase) noexcept {
    return phase < kPhaseHints.size() ? kPhaseHints[phase] : std::string_view{};
}

std::string_view band_key(std::uint8_t band) noexcept {
    return band < kBands.size() ? kBands[band] : std::string_view{};
}

}  // namespace pat::sim::crisis
