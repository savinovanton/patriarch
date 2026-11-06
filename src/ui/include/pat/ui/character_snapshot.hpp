#pragma once

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

/// Display names of the five skills (GDD 01 section 2): economy, diplomacy, martial, intrigue, learning.
inline constexpr std::array<const char*, kSkillCount> kSkillNames = {
    "Хозяйство", "Дипломатия", "Воинское дело", "Интриги", "Учёность"};

struct CharacterSnapshot {
    std::string given_name;
    std::string patronymic;
    std::string surname;
    std::string role;    ///< e.g. "стольник"
    std::string health;  ///< e.g. "крепкое"
    int age = 0;
    std::array<int, kSkillCount> skills{};
    std::vector<std::string> traits;
    std::vector<std::string> family;     ///< one line per relative
    std::vector<std::string> relations;  ///< one line per acquaintance (the 1000-row list of the spike)

    [[nodiscard]] std::string full_name() const;
    [[nodiscard]] bool operator==(const CharacterSnapshot&) const = default;
};

/// Deterministic fictional character (real clan names, invented people), for the spike and tests.
[[nodiscard]] CharacterSnapshot make_test_character(std::uint32_t seed, std::size_t relation_count = 1000);

}  // namespace pat::ui
