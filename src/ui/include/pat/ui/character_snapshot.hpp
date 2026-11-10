#pragma once

#include "pat/loc/localizer.hpp"
#include "pat/ui/explanation.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// What the character window shows (E3-T07). It is a plain value type - a snapshot - so the UI never
// touches the simulation: the game fills it from an immutable world snapshot, tests from a generator.

namespace pat::ui {

inline constexpr std::size_t kSkillCount = 5;
inline constexpr int kSkillMax = 20;

/// String-table keys of the display names of the five skills (GDD 01 section 2): economy, diplomacy,
/// martial, intrigue, learning.
inline constexpr std::array<const char*, kSkillCount> kSkillKeys = {
    "ui.skill.economy", "ui.skill.diplomacy", "ui.skill.martial", "ui.skill.intrigue", "ui.skill.learning"};

struct CharacterSnapshot {
    std::string given_name;
    std::string patronymic;
    std::string surname;
    std::string role;    ///< string-table key, e.g. "ui.character.role.stolnik"
    std::string health;  ///< string-table key, e.g. "ui.health.strong"
    int age = 0;
    std::array<int, kSkillCount> skills{};
    std::vector<std::string> traits;  ///< content names, ready to show
    std::vector<std::string> family;  ///< one line per relative, written in the language it was built in
    /// How each skill value is composed (for tooltips); the root value equals the skill value.
    std::array<Explanation, kSkillCount> skill_explanations;
    std::vector<std::string> relations;  ///< one line per acquaintance (the 1000-row list of the spike)

    [[nodiscard]] std::string full_name() const;
    [[nodiscard]] bool operator==(const CharacterSnapshot&) const = default;
};

/// Deterministic fictional character (real clan names, invented people), for the spike and tests. The
/// phrases of the family and relation lines and of the skill breakdowns come from `loc`; names and
/// traits are placeholder content until the dynasty systems provide them.
[[nodiscard]] CharacterSnapshot
make_test_character(const loc::Localizer& loc, std::uint32_t seed, std::size_t relation_count = 1000);

}  // namespace pat::ui
