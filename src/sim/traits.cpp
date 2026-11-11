#include "pat/sim/traits.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <string_view>

namespace pat::sim {
namespace {

using core::Fixed32;

constexpr std::int32_t kMaxMultiplierPermille = 10000;
constexpr std::size_t kMaxCongenitalTraits = 3;

bool is_congenital(const TraitDef& trait) noexcept {
    return trait.category == TraitCategory::Congenital || trait.inherit_chance > Fixed32::zero();
}

bool valid_multiplier(std::int32_t value) noexcept {
    return value >= 0 && value <= kMaxMultiplierPermille;
}

std::int32_t accumulate_multiplier(std::int32_t accumulated, std::int32_t factor) noexcept {
    const auto product = static_cast<std::int64_t>(accumulated) * factor / 1000;
    return static_cast<std::int32_t>(
        std::clamp(product, std::int64_t{0}, std::int64_t{kMaxMultiplierPermille}));
}

core::Result<void> invalid_trait(const TraitDef& trait, std::string_view message) {
    return core::Result<void>::err({1, std::format("Trait {} ({}): {}", trait.id, trait.key, message)});
}

void add_term(CharacterBreakdown& breakdown,
              CharacterTermKind kind,
              std::uint16_t trait,
              Fixed32 value,
              std::int32_t factor = 0) {
    breakdown.terms.push_back({kind, trait, value, factor});
    breakdown.total += value;
}

void clamp_total(CharacterBreakdown& breakdown, Fixed32 minimum, Fixed32 maximum) {
    const Fixed32 clamped = std::clamp(breakdown.total, minimum, maximum);
    if (clamped != breakdown.total) {
        add_term(breakdown, CharacterTermKind::Clamp, 0, clamped - breakdown.total);
    }
}

Fixed32 base_attribute(const CharacterTable& characters, std::size_t row, CharacterAttribute attribute) {
    switch (attribute) {
        case CharacterAttribute::Health:
            return characters.health[row];
        case CharacterAttribute::Fertility:
            return characters.fertility[row];
        case CharacterAttribute::Stress:
            return characters.stress[row];
        case CharacterAttribute::Piety:
            return characters.piety[row];
        case CharacterAttribute::Ambition:
            return characters.ambition[row];
        case CharacterAttribute::LoyaltyHead:
            return characters.loyalty_head[row];
        case CharacterAttribute::LoyaltyEmperor:
            return characters.loyalty_emperor[row];
        case CharacterAttribute::Count:
            break;
    }
    return Fixed32::zero();
}

}  // namespace

const TraitDef* TraitCatalog::find(std::uint16_t id) const noexcept {
    if (id == 0) {
        return nullptr;
    }
    const auto found = std::lower_bound(
        definitions.begin(), definitions.end(), id, [](const TraitDef& trait, std::uint16_t sought) {
            return trait.id < sought;
        });
    return found != definitions.end() && found->id == id ? &*found : nullptr;
}

core::Result<void> validate(const TraitCatalog& catalog) {
    std::uint16_t previous_id = 0;
    for (std::size_t i = 0; i < catalog.definitions.size(); ++i) {
        const TraitDef& trait = catalog.definitions[i];
        if (trait.id == 0 || trait.id <= previous_id) {
            return invalid_trait(trait, "ids must be nonzero and strictly ascending");
        }
        previous_id = trait.id;
        if (trait.key.empty() || trait.name_key.empty() || trait.desc_key.empty()) {
            return invalid_trait(trait, "key and localisation keys must not be empty");
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (catalog.definitions[j].key == trait.key) {
                return invalid_trait(trait, "duplicate key");
            }
        }
        if (trait.category > TraitCategory::Religious) {
            return invalid_trait(trait, "unknown category");
        }
        for (Fixed32 modifier : trait.skill_modifiers) {
            if (modifier < Fixed32::from_int(-10) || modifier > Fixed32::from_int(10)) {
                return invalid_trait(trait, "skill modifier outside [-10, 10]");
            }
        }
        for (Fixed32 modifier : trait.attribute_modifiers) {
            if (modifier < -kScaleMax || modifier > kScaleMax) {
                return invalid_trait(trait, "attribute modifier outside [-100, 100]");
            }
        }
        if (!valid_multiplier(trait.health_multiplier_permille) ||
            !valid_multiplier(trait.fertility_multiplier_permille) ||
            !valid_multiplier(trait.mortality_multiplier_permille)) {
            return invalid_trait(trait, "multiplier outside [0, 10000]");
        }
        if (trait.inherit_chance < Fixed32::zero() || trait.inherit_chance > Fixed32::one()) {
            return invalid_trait(trait, "inheritance chance outside [0, 1]");
        }
        if (trait.opposite != 0) {
            const TraitDef* opposite = catalog.find(trait.opposite);
            if (opposite == nullptr || opposite == &trait || opposite->opposite != trait.id) {
                return invalid_trait(trait, "opposites must be distinct, existing and symmetric");
            }
        }
        for (std::size_t j = 0; j < trait.ai_hint.size(); ++j) {
            if (trait.ai_hint[j].first.empty()) {
                return invalid_trait(trait, "AI hint key must not be empty");
            }
            for (std::size_t k = 0; k < j; ++k) {
                if (trait.ai_hint[k].first == trait.ai_hint[j].first) {
                    return invalid_trait(trait, "duplicate AI hint key");
                }
            }
        }
    }
    return core::Result<void>::ok();
}

TraitStatus check_trait_set(const TraitCatalog& catalog, std::span<const std::uint16_t> traits) {
    std::size_t count = 0;
    std::size_t congenital_count = 0;
    for (std::size_t i = 0; i < traits.size(); ++i) {
        const std::uint16_t id = traits[i];
        if (id == 0) {
            continue;
        }
        if (++count > kTraitSlots) {
            return TraitStatus::SlotsFull;
        }
        const TraitDef* trait = catalog.find(id);
        if (trait == nullptr) {
            return TraitStatus::Unknown;
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (traits[j] == id) {
                return TraitStatus::InvalidSet;
            }
            if (trait->opposite != 0 && traits[j] == trait->opposite) {
                return TraitStatus::Opposite;
            }
        }
        if (is_congenital(*trait) && ++congenital_count > kMaxCongenitalTraits) {
            return TraitStatus::CongenitalLimit;
        }
    }
    return TraitStatus::Ok;
}

TraitStatus check_grant_trait(const World& world,
                              const TraitCatalog& catalog,
                              CharacterId character,
                              std::uint16_t trait) {
    if (!world.is_alive(character)) {
        return TraitStatus::NotAlive;
    }
    if (catalog.find(trait) == nullptr) {
        return TraitStatus::Unknown;
    }
    const auto& current = world.characters().traits[character.index()];
    const TraitStatus status = check_trait_set(catalog, current);
    if (status != TraitStatus::Ok) {
        return status;
    }
    if (std::find(current.begin(), current.end(), trait) != current.end()) {
        return TraitStatus::AlreadyPresent;
    }
    std::array<std::uint16_t, kTraitSlots + 1> candidate{};
    std::copy(current.begin(), current.end(), candidate.begin());
    candidate.back() = trait;
    return check_trait_set(catalog, candidate);
}

TraitStatus check_revoke_trait(const World& world,
                               const TraitCatalog& catalog,
                               CharacterId character,
                               std::uint16_t trait) {
    if (!world.is_alive(character)) {
        return TraitStatus::NotAlive;
    }
    if (catalog.find(trait) == nullptr) {
        return TraitStatus::Unknown;
    }
    const auto& current = world.characters().traits[character.index()];
    const TraitStatus status = check_trait_set(catalog, current);
    if (status != TraitStatus::Ok) {
        return status;
    }
    return std::find(current.begin(), current.end(), trait) == current.end() ? TraitStatus::Missing
                                                                             : TraitStatus::Ok;
}

TraitStatus
grant_trait(World& world, const TraitCatalog& catalog, CharacterId character, std::uint16_t trait) {
    const TraitStatus status = check_grant_trait(world, catalog, character, trait);
    if (status != TraitStatus::Ok) {
        return status;
    }
    auto traits = world.characters().traits[character.index()];
    *std::find(traits.begin(), traits.end(), std::uint16_t{0}) = trait;
    return world.set_character_traits(character, traits) ? TraitStatus::Ok : TraitStatus::InvalidSet;
}

TraitStatus
revoke_trait(World& world, const TraitCatalog& catalog, CharacterId character, std::uint16_t trait) {
    const TraitStatus status = check_revoke_trait(world, catalog, character, trait);
    if (status != TraitStatus::Ok) {
        return status;
    }
    auto traits = world.characters().traits[character.index()];
    *std::find(traits.begin(), traits.end(), trait) = 0;
    return world.set_character_traits(character, traits) ? TraitStatus::Ok : TraitStatus::InvalidSet;
}

Fixed32 CharacterBreakdown::sum_of_terms() const noexcept {
    Fixed32 result;
    for (const CharacterTerm& term : terms) {
        result += term.value;
    }
    return result;
}

CharacterBreakdown
skill_breakdown(const World& world, const TraitCatalog& catalog, CharacterId character, Skill skill) {
    CharacterBreakdown breakdown;
    const std::size_t index = static_cast<std::size_t>(skill);
    if (!world.is_alive(character) || index >= kSkillCount) {
        return breakdown;
    }
    const CharacterTable& characters = world.characters();
    const std::size_t row = character.index();
    add_term(breakdown, CharacterTermKind::Base, 0, characters.skills[row][index]);
    for (std::uint16_t id : characters.traits[row]) {
        const TraitDef* trait = catalog.find(id);
        if (trait != nullptr && trait->skill_modifiers[index] != Fixed32::zero()) {
            add_term(breakdown, CharacterTermKind::Trait, id, trait->skill_modifiers[index]);
        }
    }
    clamp_total(breakdown, Fixed32::zero(), kSkillMax);
    return breakdown;
}

CharacterBreakdown attribute_breakdown(const World& world,
                                       const TraitCatalog& catalog,
                                       CharacterId character,
                                       CharacterAttribute attribute) {
    CharacterBreakdown breakdown;
    const std::size_t index = static_cast<std::size_t>(attribute);
    if (!world.is_alive(character) || index >= kCharacterAttributeCount) {
        return breakdown;
    }
    const CharacterTable& characters = world.characters();
    const std::size_t row = character.index();
    add_term(breakdown, CharacterTermKind::Base, 0, base_attribute(characters, row, attribute));
    for (std::uint16_t id : characters.traits[row]) {
        const TraitDef* trait = catalog.find(id);
        if (trait != nullptr && trait->attribute_modifiers[index] != Fixed32::zero()) {
            add_term(breakdown, CharacterTermKind::Trait, id, trait->attribute_modifiers[index]);
        }
    }
    if (attribute == CharacterAttribute::Health || attribute == CharacterAttribute::Fertility) {
        // Bound the cumulative factor, not each successive value: the additive subtotal is at
        // most 900 points, so every intermediate value stays within 9000 points in Fixed32.
        const Fixed32 additive = breakdown.total;
        std::int32_t accumulated = 1000;
        for (std::uint16_t id : characters.traits[row]) {
            const TraitDef* trait = catalog.find(id);
            if (trait == nullptr) {
                continue;
            }
            const std::int32_t factor = attribute == CharacterAttribute::Health
                                            ? trait->health_multiplier_permille
                                            : trait->fertility_multiplier_permille;
            if (factor != 1000) {
                accumulated = accumulate_multiplier(accumulated, factor);
                const Fixed32 after = additive.mul_permille(accumulated);
                add_term(breakdown, CharacterTermKind::Multiplier, id, after - breakdown.total, factor);
            }
        }
    }
    const bool signed_scale = attribute == CharacterAttribute::Ambition ||
                              attribute == CharacterAttribute::LoyaltyHead ||
                              attribute == CharacterAttribute::LoyaltyEmperor;
    clamp_total(breakdown, signed_scale ? -kSignedScaleMax : Fixed32::zero(), kScaleMax);
    return breakdown;
}

std::int32_t trait_mortality_multiplier(const TraitCatalog& catalog, std::span<const std::uint16_t> traits) {
    std::int32_t accumulated = 1000;
    // Catalogue order guarantees the same product for every ordering of the input set. Unknown
    // legacy ids and empty slots have no effect; duplicate inputs are applied only once.
    for (const TraitDef& trait : catalog.definitions) {
        if (std::find(traits.begin(), traits.end(), trait.id) != traits.end()) {
            accumulated = accumulate_multiplier(accumulated, trait.mortality_multiplier_permille);
        }
    }
    return accumulated;
}

}  // namespace pat::sim
