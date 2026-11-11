#pragma once

#include "pat/core/result.hpp"
#include "pat/core/static_vec.hpp"
#include "pat/sim/world.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace pat::sim {

enum class TraitCategory : std::uint8_t { Personality, Education, Health, Life, Congenital, Religious };

enum class CharacterAttribute : std::uint8_t {
    Health,
    Fertility,
    Stress,
    Piety,
    Ambition,
    LoyaltyHead,
    LoyaltyEmperor,
    Count
};
inline constexpr std::size_t kCharacterAttributeCount = static_cast<std::size_t>(CharacterAttribute::Count);

struct TraitDef {
    std::uint16_t id = 0;
    std::string key;
    std::string name_key;
    std::string desc_key;
    TraitCategory category = TraitCategory::Personality;
    std::uint16_t opposite = 0;
    std::array<core::Fixed32, kSkillCount> skill_modifiers{};
    std::array<core::Fixed32, kCharacterAttributeCount> attribute_modifiers{};
    std::int32_t health_multiplier_permille = 1000;
    std::int32_t fertility_multiplier_permille = 1000;
    std::int32_t mortality_multiplier_permille = 1000;
    core::Fixed32 inherit_chance;
    std::vector<std::pair<std::string, std::int32_t>> ai_hint;
};

/// Immutable during a simulation; definitions use stable ids in ascending order.
struct TraitCatalog {
    std::vector<TraitDef> definitions;

    [[nodiscard]] const TraitDef* find(std::uint16_t id) const noexcept;
};

[[nodiscard]] core::Result<void> validate(const TraitCatalog& catalog);

enum class TraitStatus : std::uint8_t {
    Ok,
    NotAlive,
    Unknown,
    AlreadyPresent,
    Missing,
    SlotsFull,
    Opposite,
    CongenitalLimit,
    InvalidSet
};

/// Input order does not matter; zeros represent unused slots. No world data is changed.
[[nodiscard]] TraitStatus check_trait_set(const TraitCatalog& catalog, std::span<const std::uint16_t> traits);
[[nodiscard]] TraitStatus check_grant_trait(const World& world,
                                            const TraitCatalog& catalog,
                                            CharacterId character,
                                            std::uint16_t trait);
[[nodiscard]] TraitStatus check_revoke_trait(const World& world,
                                             const TraitCatalog& catalog,
                                             CharacterId character,
                                             std::uint16_t trait);
[[nodiscard]] TraitStatus
grant_trait(World& world, const TraitCatalog& catalog, CharacterId character, std::uint16_t trait);
[[nodiscard]] TraitStatus
revoke_trait(World& world, const TraitCatalog& catalog, CharacterId character, std::uint16_t trait);

enum class CharacterTermKind : std::uint8_t { Base, Trait, Multiplier, Clamp };

struct CharacterTerm {
    CharacterTermKind kind = CharacterTermKind::Base;
    std::uint16_t trait = 0;
    core::Fixed32 value;
    std::int32_t factor_permille = 0;
};

/// Multiplier and clamp terms are deltas, so every explanation sums exactly to total.
struct CharacterBreakdown {
    core::StaticVec<CharacterTerm, 2 * kTraitSlots + 2> terms;
    core::Fixed32 total;

    [[nodiscard]] core::Fixed32 sum_of_terms() const noexcept;
};

/// Effective values are pure projections of the stored base columns and the trait catalogue.
/// Dead or unknown characters, and unknown skills/attributes, return an empty breakdown.
[[nodiscard]] CharacterBreakdown
skill_breakdown(const World& world, const TraitCatalog& catalog, CharacterId character, Skill skill);
[[nodiscard]] CharacterBreakdown attribute_breakdown(const World& world,
                                                     const TraitCatalog& catalog,
                                                     CharacterId character,
                                                     CharacterAttribute attribute);
[[nodiscard]] std::int32_t trait_mortality_multiplier(const TraitCatalog& catalog,
                                                      std::span<const std::uint16_t> traits);

}  // namespace pat::sim
