#pragma once
#include "pat/content/load_errors.hpp"
#include "pat/core/result.hpp"
#include "pat/data/content_source.hpp"
#include "pat/sim/army/rules.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>
namespace pat::content {
// Definition keys remain symbolic until T20 resolves them to World ids. No World or save mutations.
struct MilitaryFortress {
    std::int32_t level = 0, garrison = 0, stores_days = 0, wall_pm = 1000;
    bool rebels = false;
};
struct MilitaryProvince {
    std::string province;
    sim::army::Terrain terrain = sim::army::Terrain::Plain;
    MilitaryFortress fortress;
    bool has_fortress = false;
};
enum class MilitaryOwner : std::uint8_t { Crown = 1, Dynasty = 2, Rebels = 3 };
enum class MilitaryTag : std::uint8_t { Regular = 1, Levy = 2, Guard = 3, Insurgents = 4 };
enum class MilitaryFrontState : std::uint8_t { Peace = 1, Tension = 2, War = 3, Truce = 4 };
struct MilitaryArmy {
    std::string id, name_key, dynasty, province, commander_office;
    MilitaryOwner owner = MilitaryOwner::Crown;
    MilitaryTag tag = MilitaryTag::Regular;
    std::array<std::int32_t, 4> comp{};
    std::int32_t quality = 0, experience = 0, morale_x100 = 0, supply_x100 = 0;
};
struct MilitaryFront {
    std::string id, name_key;
    MilitaryFrontState state = MilitaryFrontState::Peace;
    std::int32_t pressure_x100 = 0, enemy_x100 = 0, fatigue_x100 = 0;
};
struct MilitarySetup {
    std::string capital;
    std::int32_t crown_manpower = 0;
    std::vector<MilitaryProvince> provinces;
    std::vector<MilitaryArmy> armies;
    MilitaryFront front;
};
struct ArmyContent {
    sim::army::ArmyRules rules;
    MilitarySetup military;
    /// Typed rules only (summary key 24 at T20); independent of positions, formatting and object order.
    std::uint64_t rules_hash = 0;
};
using ArmyRulesResult = core::Result<sim::army::ArmyRules, std::vector<data::LoadError>>;
using MilitarySetupResult = core::Result<MilitarySetup, std::vector<data::LoadError>>;
using ArmyContentResult = core::Result<ArmyContent, std::vector<data::LoadError>>;
// Builders require an error-free registry from content_schemas(). Scenario loading is separate.
[[nodiscard]] ArmyRulesResult build_army_rules(const data::ContentRegistry& registry);
[[nodiscard]] MilitarySetupResult build_military_setup(const data::ContentRegistry& registry,
                                                       const sim::army::ArmyRules& rules);
[[nodiscard]] ArmyContentResult load_army_content(std::span<const data::ContentSource* const> roots);
[[nodiscard]] ArmyContentResult load_army_content(const data::ContentSource& root);
}  // namespace pat::content
