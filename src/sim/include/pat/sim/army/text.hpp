#pragma once
#include "pat/sim/army/rules.hpp"

#include <array>
#include <cstdint>
#include <string_view>
namespace pat::sim::army {
struct DenialText {
    std::uint8_t code;
    std::string_view key;
};
// Codes are reserved by army-model.md §8.3. Code 10 is unissued (money uses existing code 43).
inline constexpr std::array<DenialText, 39> kDenialTexts{{{1, "army.denial.army_off"},
                                                          {2, "army.denial.unknown_army"},
                                                          {3, "army.denial.not_owner"},
                                                          {4, "army.denial.not_system"},
                                                          {5, "army.denial.world_army_limit"},
                                                          {6, "army.denial.owner_army_limit"},
                                                          {7, "army.denial.size_too_small"},
                                                          {8, "army.denial.size_too_large"},
                                                          {9, "army.denial.no_manpower"},
                                                          {11, "army.denial.tag_not_allowed"},
                                                          {12, "army.denial.unknown_province"},
                                                          {13, "army.denial.not_own_province"},
                                                          {14, "army.denial.levy_cooldown"},
                                                          {15, "army.denial.levy_no_nobles"},
                                                          {16, "army.denial.training"},
                                                          {17, "army.denial.unreliable"},
                                                          {18, "army.denial.not_idle"},
                                                          {19, "army.denial.different_province"},
                                                          {20, "army.denial.different_owner"},
                                                          {21, "army.denial.commander_invalid"},
                                                          {22, "army.denial.commander_unknown"},
                                                          {23, "army.denial.commander_busy"},
                                                          {24, "army.denial.commander_not_office"},
                                                          {25, "army.denial.no_path"},
                                                          {26, "army.denial.foreign_territory"},
                                                          {27, "army.denial.war_forbidden"},
                                                          {28, "army.denial.no_hostile_fortress"},
                                                          {29, "army.denial.front_not_war"},
                                                          {30, "army.denial.front_min_size"},
                                                          {31, "army.denial.already_on_front"},
                                                          {32, "army.denial.not_on_front"},
                                                          {33, "army.denial.siege_exists"},
                                                          {34, "army.denial.assault_not_ready"},
                                                          {35, "army.denial.rebels_not_orderable"},
                                                          {36, "army.denial.insurgent_limit"},
                                                          {37, "army.denial.table_full"},
                                                          {38, "army.denial.empty_army"},
                                                          {39, "army.denial.resting"},
                                                          {40, "army.denial.bad_argument"}}};
[[nodiscard]] constexpr std::string_view denial_key(std::uint8_t code) noexcept {
    for (const auto& item : kDenialTexts)
        if (item.code == code)
            return item.key;
    return {};
}
// Presentation metadata shared by military snapshots, reports and UI. Unknown values return empty.
[[nodiscard]] constexpr std::string_view branch_key(Branch value) noexcept {
    switch (value) {
        case Branch::Infantry:
            return "army.branch.infantry";
        case Branch::Cavalry:
            return "army.branch.cavalry";
        case Branch::Artillery:
            return "army.branch.artillery";
        case Branch::Irregular:
            return "army.branch.irregular";
    }
    return {};
}
[[nodiscard]] constexpr std::string_view terrain_key(Terrain value) noexcept {
    switch (value) {
        case Terrain::Plain:
            return "army.terrain.plain";
        case Terrain::Steppe:
            return "army.terrain.steppe";
        case Terrain::Forest:
            return "army.terrain.forest";
        case Terrain::Swamp:
            return "army.terrain.swamp";
        case Terrain::Hills:
            return "army.terrain.hills";
        case Terrain::Mountain:
            return "army.terrain.mountain";
    }
    return {};
}
// Stable tag slugs follow army-model.md; no dependency on World tables.
[[nodiscard]] constexpr std::string_view tag_key(std::string_view slug) noexcept {
    if (slug == "regular")
        return "army.tag.regular";
    if (slug == "levy")
        return "army.tag.levy";
    if (slug == "guard")
        return "army.tag.guard";
    if (slug == "insurgents")
        return "army.tag.insurgents";
    return {};
}
// Stable front slugs follow army-model.md; no dependency on World tables.
[[nodiscard]] constexpr std::string_view front_state_key(std::string_view slug) noexcept {
    if (slug == "peace")
        return "army.front.peace";
    if (slug == "tension")
        return "army.front.tension";
    if (slug == "war")
        return "army.front.war";
    if (slug == "truce")
        return "army.front.truce";
    return {};
}
// Stable siege slugs follow army-model.md; no dependency on World tables.
[[nodiscard]] constexpr std::string_view siege_phase_key(std::string_view slug) noexcept {
    if (slug == "investing")
        return "army.siege.investing";
    if (slug == "breached")
        return "army.siege.breached";
    if (slug == "starving")
        return "army.siege.starving";
    return {};
}
inline constexpr std::string_view kNorthFrontKey = "army.front.north";
inline constexpr std::string_view kInitialCrownGuardKey = "army.initial.crown_guard";
inline constexpr std::string_view kInitialCrownFieldKey = "army.initial.crown_field";
inline constexpr std::string_view kTermCompositionKey = "army.term.composition";
inline constexpr std::string_view kTermQualityKey = "army.term.quality";
inline constexpr std::string_view kTermExperienceKey = "army.term.experience";
inline constexpr std::string_view kTermMoraleKey = "army.term.morale";
inline constexpr std::string_view kTermSupplyKey = "army.term.supply";
inline constexpr std::string_view kTermTerrainKey = "army.term.terrain";
inline constexpr std::string_view kTermCommanderKey = "army.term.commander";
inline constexpr std::string_view kTermCounterKey = "army.term.counter";
inline constexpr std::string_view kTermFortressKey = "army.term.fortress";
inline constexpr std::string_view kTermSeasonKey = "army.term.season";
inline constexpr std::string_view kTermRoadKey = "army.term.road";
inline constexpr std::string_view kTermUpkeepKey = "army.term.upkeep";
inline constexpr std::string_view kTermLossesKey = "army.term.losses";
inline constexpr std::string_view kTermPressureKey = "army.term.pressure";
inline constexpr std::string_view kTermWallKey = "army.term.wall";
inline constexpr std::string_view kTermProgressKey = "army.term.progress";
inline constexpr std::string_view kTermStoresKey = "army.term.stores";
inline constexpr std::string_view kUiListKey = "ui.army.list";
inline constexpr std::string_view kUiDetailsKey = "ui.army.details";
inline constexpr std::string_view kUiCommanderKey = "ui.army.commander";
inline constexpr std::string_view kUiFrontsKey = "ui.army.fronts";
inline constexpr std::string_view kUiBattleReportKey = "ui.army.battle_report";
inline constexpr std::string_view kUiSiegeKey = "ui.army.siege";
inline constexpr std::string_view kUiAssaultKey = "ui.army.assault";
inline constexpr std::string_view kUiRaiseKey = "ui.army.raise";
inline constexpr std::string_view kUiLevyKey = "ui.army.levy";
inline constexpr std::string_view kUiDisbandKey = "ui.army.disband";
inline constexpr std::string_view kUiSendToFrontKey = "ui.army.send_to_front";
inline constexpr std::string_view kUiRecallKey = "ui.army.recall";
}  // namespace pat::sim::army
