#include "pat/sim/names.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <utility>

namespace pat::sim {

namespace {

using Check = core::Result<void, NamesError>;

constexpr std::array<LifeStage, 5> kStages{
    LifeStage::Child, LifeStage::Adolescent, LifeStage::Prime, LifeStage::Mature, LifeStage::Elder};

const char* sex_name(Sex sex) noexcept {
    return sex == Sex::Male ? "male" : "female";
}

const char* stage_name(LifeStage stage) noexcept {
    switch (stage) {
        case LifeStage::Child:
            return "child";
        case LifeStage::Adolescent:
            return "adolescent";
        case LifeStage::Prime:
            return "prime";
        case LifeStage::Mature:
            return "mature";
        case LifeStage::Elder:
            return "elder";
    }
    return "?";
}

Check fail(NamesErrorCode code, std::string message) {
    return Check::err(NamesError{code, std::move(message)});
}

/// (draw * total) >> 32: one draw, no rejection, no floating point. total <= 2^32 - 1.
std::uint32_t scale_draw(std::uint32_t draw, std::uint64_t total) noexcept {
    return static_cast<std::uint32_t>((static_cast<std::uint64_t>(draw) * total) >> 32U);
}

bool contains(std::span<const std::string_view> ids, std::string_view id) noexcept {
    return std::ranges::find(ids, id) != ids.end();
}

/// Weight of `name` for a child of this context: the base weight plus the family bonuses.
std::uint64_t effective_weight(const NamePools& pools, const GivenName& name, const NamingContext& context) {
    std::uint64_t weight = static_cast<std::uint64_t>(std::max(name.weight, 0));
    if (!context.parent_name.empty() && name.id == context.parent_name) {
        weight += static_cast<std::uint64_t>(std::max(pools.rules.parent_bonus, 0));
    }
    if (!context.grandparent_name.empty() && name.id == context.grandparent_name) {
        weight += static_cast<std::uint64_t>(std::max(pools.rules.grandparent_bonus, 0));
    }
    return weight;
}

}  // namespace

// ---- validation --------------------------------------------------------------------------------

core::Result<void, NamesError> validate(const NamePools& pools) {
    for (const auto& [value, name] : {std::pair{pools.rules.parent_bonus, "parent_bonus"},
                                      std::pair{pools.rules.grandparent_bonus, "grandparent_bonus"}}) {
        if (value < 0 || value > NamePools::kMaxBonus) {
            return fail(NamesErrorCode::Rules,
                        std::format("{} {} is outside 0...{}", name, value, NamePools::kMaxBonus));
        }
    }

    std::array<std::size_t, 2> names_per_sex{};
    for (std::size_t i = 0; i < pools.given_names.size(); ++i) {
        const GivenName& name = pools.given_names[i];
        if (name.id.empty() || name.name_key.empty()) {
            return fail(NamesErrorCode::GivenName,
                        std::format("given name #{} has an empty id or name key", i));
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (pools.given_names[j].id == name.id) {
                return fail(NamesErrorCode::GivenName, std::format("given name '{}' is repeated", name.id));
            }
        }
        if (name.weight < 1 || name.weight > NamePools::kMaxNameWeight) {
            return fail(NamesErrorCode::GivenName,
                        std::format("given name '{}': weight {} is outside 1...{}",
                                    name.id,
                                    name.weight,
                                    NamePools::kMaxNameWeight));
        }
        const bool has_son = !name.patronymic_son_key.empty();
        const bool has_daughter = !name.patronymic_daughter_key.empty();
        if (name.sex == Sex::Male && !(has_son && has_daughter)) {
            return fail(
                NamesErrorCode::Patronymic,
                std::format("man's name '{}' needs both patronymic forms (son and daughter)", name.id));
        }
        if (name.sex == Sex::Female && (has_son || has_daughter)) {
            return fail(NamesErrorCode::Patronymic,
                        std::format("woman's name '{}' must not have patronymic forms", name.id));
        }
        ++names_per_sex[name.sex == Sex::Male ? 0 : 1];
    }

    for (std::size_t i = 0; i < pools.surnames.size(); ++i) {
        const SurnameForms& surname = pools.surnames[i];
        if (surname.dynasty_id.empty() || surname.male_key.empty() || surname.female_key.empty()) {
            return fail(NamesErrorCode::Surname,
                        std::format("surname #{} needs a dynasty and both forms (male and female)", i));
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (pools.surnames[j].dynasty_id == surname.dynasty_id) {
                return fail(NamesErrorCode::Surname,
                            std::format("dynasty '{}' has two surname entries", surname.dynasty_id));
            }
        }
    }

    for (std::size_t i = 0; i < pools.portraits.size(); ++i) {
        const PortraitEntry& portrait = pools.portraits[i];
        if (portrait.id == 0) {
            return fail(NamesErrorCode::Portrait, "portrait id 0 is reserved for \"no portrait\"");
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (pools.portraits[j].id == portrait.id) {
                return fail(NamesErrorCode::Portrait, std::format("portrait id {} is repeated", portrait.id));
            }
        }
    }

    for (const Sex sex : {Sex::Male, Sex::Female}) {
        if (names_per_sex[sex == Sex::Male ? 0 : 1] == 0) {
            return fail(NamesErrorCode::PoolCoverage,
                        std::format("no {} given name in the pool", sex_name(sex)));
        }
        for (const LifeStage stage : kStages) {
            const bool found = std::ranges::any_of(
                pools.portraits, [&](const PortraitEntry& p) { return p.sex == sex && p.stage == stage; });
            if (!found) {
                return fail(
                    NamesErrorCode::PoolCoverage,
                    std::format("no portrait for sex {} and stage {}", sex_name(sex), stage_name(stage)));
            }
        }
    }
    return Check::ok();
}

// ---- choosing ----------------------------------------------------------------------------------

std::size_t choose_given_name(const NamePools& pools, core::Pcg32& rng, const NamingContext& context) {
    const std::uint32_t draw = rng.next_u32();  // the one and only draw, whatever happens below

    for (const bool respect_siblings : {true, false}) {
        std::uint64_t total = 0;
        for (const GivenName& name : pools.given_names) {
            if (name.sex != context.sex || (respect_siblings && contains(context.living_siblings, name.id))) {
                continue;
            }
            total += effective_weight(pools, name, context);
        }
        if (total == 0) {
            continue;  // everything taken (or no name of the sex): retry without the exclusion
        }
        std::uint64_t target = scale_draw(draw, total);
        for (std::size_t i = 0; i < pools.given_names.size(); ++i) {
            const GivenName& name = pools.given_names[i];
            if (name.sex != context.sex || (respect_siblings && contains(context.living_siblings, name.id))) {
                continue;
            }
            const std::uint64_t weight = effective_weight(pools, name, context);
            if (target < weight) {
                return i;
            }
            target -= weight;
        }
    }
    return kNoName;
}

std::uint16_t choose_portrait(const NamePools& pools, core::Pcg32& rng, Sex sex, LifeStage stage) {
    const std::uint32_t draw = rng.next_u32();  // the one and only draw

    // Nearest stage that has a portrait of this sex; on a tie the younger one.
    const auto stage_index = static_cast<int>(stage);
    int best_distance = -1;
    int best_stage = stage_index;
    for (const PortraitEntry& portrait : pools.portraits) {
        if (portrait.sex != sex) {
            continue;
        }
        const int other = static_cast<int>(portrait.stage);
        const int distance = other > stage_index ? other - stage_index : stage_index - other;
        if (best_distance < 0 || distance < best_distance ||
            (distance == best_distance && other < best_stage)) {
            best_distance = distance;
            best_stage = other;
        }
    }
    if (best_distance < 0) {
        return 0;
    }

    std::uint64_t count = 0;
    for (const PortraitEntry& portrait : pools.portraits) {
        if (portrait.sex == sex && static_cast<int>(portrait.stage) == best_stage) {
            ++count;
        }
    }
    std::uint32_t target = scale_draw(draw, count);
    for (const PortraitEntry& portrait : pools.portraits) {
        if (portrait.sex == sex && static_cast<int>(portrait.stage) == best_stage) {
            if (target == 0) {
                return portrait.id;
            }
            --target;
        }
    }
    return 0;
}

// ---- composing ---------------------------------------------------------------------------------

const GivenName* find_given_name(const NamePools& pools, std::string_view id) noexcept {
    if (id.empty()) {
        return nullptr;
    }
    const auto it = std::ranges::find_if(pools.given_names, [&](const GivenName& n) { return n.id == id; });
    return it == pools.given_names.end() ? nullptr : &*it;
}

const SurnameForms* find_surname(const NamePools& pools, std::string_view dynasty_id) noexcept {
    if (dynasty_id.empty()) {
        return nullptr;
    }
    const auto it = std::ranges::find_if(pools.surnames,
                                         [&](const SurnameForms& s) { return s.dynasty_id == dynasty_id; });
    return it == pools.surnames.end() ? nullptr : &*it;
}

PersonNameKeys person_name_keys(const NamePools& pools,
                                std::string_view given_name,
                                std::string_view father_given_name,
                                std::string_view dynasty_id,
                                Sex sex) {
    PersonNameKeys keys;
    if (const GivenName* own = find_given_name(pools, given_name)) {
        keys.given = own->name_key;
    }
    // Only a man's name has patronymic forms; a woman's name as "father's name" yields none.
    if (const GivenName* father = find_given_name(pools, father_given_name);
        father != nullptr && father->sex == Sex::Male) {
        keys.patronymic = sex == Sex::Male ? father->patronymic_son_key : father->patronymic_daughter_key;
    }
    if (const SurnameForms* surname = find_surname(pools, dynasty_id)) {
        keys.surname = sex == Sex::Male ? surname->male_key : surname->female_key;
    }
    return keys;
}

}  // namespace pat::sim
