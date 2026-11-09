#pragma once

#include "pat/core/result.hpp"
#include "pat/core/rng.hpp"
#include "pat/sim/demography.hpp"
#include "pat/sim/world.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Names, patronymics and portraits (E7-T09, docs/gdd/01-dynasty.md section 2.1, docs/dev/names.md).
//
// This header is pure data plus pure functions, like demography.hpp: nothing here reads or writes the
// World. The pools come from `data/names/*.pd` through pat::content::build_name_pools (E7-T09), or
// from code in tests. The only random input is a generator passed by the caller; the caller is
// responsible for taking it from `world.rng().stream("demography.names")` (ADR-0006, epic E7 section 3
// row 8), so one stream serves both functions and the result depends only on that stream.
//
// What a character's name is made of:
//   * the given name: the id of a `given_name` object, stored in the character's `name` column as an
//     interned string (the same convention as a dynasty's name, which is the id of its definition);
//   * the patronymic: derived from the FATHER's given name, in the form for the child's sex. The
//     forms are written out in the data (no morphology algorithm);
//   * the surname: derived from the house, in the male or the female form, also written out.
// The functions below return localisation KEYS, not texts: the UI resolves them through pat::loc.
//
// There is no floating point; weights are integers.

namespace pat::sim {

/// One entry of the name pool.
struct GivenName {
    std::string id;
    Sex sex = Sex::Male;
    /// Relative frequency in the pool, 1...kMaxNameWeight.
    std::int32_t weight = 1;
    std::string name_key;
    /// Patronymic forms (men only): "Ivanovich" for a son and "Ivanovna" for a daughter of a man
    /// with this name. Empty for women's names.
    std::string patronymic_son_key;
    std::string patronymic_daughter_key;

    [[nodiscard]] bool operator==(const GivenName&) const = default;
};

/// The two written forms of a house's surname.
struct SurnameForms {
    std::string dynasty_id;  ///< id of the dynasty definition the forms belong to
    std::string male_key;
    std::string female_key;

    [[nodiscard]] bool operator==(const SurnameForms&) const = default;
};

/// One portrait of the pool. `id` is what the `portrait` column holds (0 is reserved for "none").
struct PortraitEntry {
    std::uint16_t id = 0;
    Sex sex = Sex::Male;
    LifeStage stage = LifeStage::Child;

    [[nodiscard]] bool operator==(const PortraitEntry&) const = default;
};

/// How strongly a family repeats the names of its elders. Additions to the weight of a name; 0 turns
/// the effect off.
struct NamingRules {
    std::int32_t parent_bonus = 0;
    std::int32_t grandparent_bonus = 0;

    [[nodiscard]] bool operator==(const NamingRules&) const = default;
};

struct NamePools {
    static constexpr std::int32_t kMaxNameWeight = 1000;
    static constexpr std::int32_t kMaxBonus = 100000;

    NamingRules rules;
    std::vector<GivenName> given_names;
    std::vector<SurnameForms> surnames;
    std::vector<PortraitEntry> portraits;

    [[nodiscard]] bool operator==(const NamePools&) const = default;
};

// ---- validation --------------------------------------------------------------------------------

enum class NamesErrorCode : std::uint8_t {
    Rules,         ///< a bonus outside 0...kMaxBonus
    GivenName,     ///< empty or repeated id, weight out of range, missing key
    Patronymic,    ///< a man's name without both patronymic forms, or a woman's name with one
    Surname,       ///< empty or repeated dynasty, or a missing form
    Portrait,      ///< id 0 or a repeated id
    PoolCoverage,  ///< no name for a sex, or no portrait for a sex and life stage
};

struct NamesError {
    NamesErrorCode code = NamesErrorCode::GivenName;
    std::string message;

    [[nodiscard]] bool operator==(const NamesError&) const = default;
};

/// Checks the pools: ids unique, weights and bonuses in range, patronymic forms exactly for men,
/// both surname forms present, portrait ids unique and non-zero, at least one name per sex and at
/// least one portrait per sex and life stage (so the choice functions below never fall back).
/// Returns the first problem found.
[[nodiscard]] core::Result<void, NamesError> validate(const NamePools& pools);

// ---- choosing a name ---------------------------------------------------------------------------

/// Everything the choice of a name depends on besides the pool and the random number.
struct NamingContext {
    Sex sex = Sex::Male;
    /// Given-name id of the parent whose name a family likes to repeat (empty when unknown): the
    /// father for a son, the mother for a daughter [predl.]. Names of the other sex are ignored.
    std::string_view parent_name;
    /// Same for the grandparent of that line (father's father, mother's mother) [predl.].
    std::string_view grandparent_name;
    /// Given-name ids of the living siblings of the same sex (brothers for a boy, sisters for a girl):
    /// none of them is repeated while another name is available.
    std::span<const std::string_view> living_siblings;
};

inline constexpr std::size_t kNoName = static_cast<std::size_t>(-1);

/// Picks a given name for a newborn: among the names of `context.sex`, weighted by `weight`, plus
/// the bonuses for the parent's and the grandparent's name, excluding the names of the living
/// siblings (if every name is taken, the exclusion is dropped). Returns the index into
/// `pools.given_names`, or kNoName when the pool has no name of that sex.
///
/// Draws EXACTLY ONE number from `rng`, in every branch (epic E7 section 3 row 8), and uses it
/// without rejection: index = (draw * total_weight) >> 32.
[[nodiscard]] std::size_t
choose_given_name(const NamePools& pools, core::Pcg32& rng, const NamingContext& context);

/// Picks a portrait id for a character of this sex and life stage: uniformly among the portraits of
/// exactly that sex and stage; if there are none, among the ones of the same sex and the nearest
/// stage (the younger of two equally near); 0 ("no portrait") if there are none of that sex.
/// Draws EXACTLY ONE number from `rng`, in every branch. The caller calls it again when the
/// character moves to another life stage.
[[nodiscard]] std::uint16_t
choose_portrait(const NamePools& pools, core::Pcg32& rng, Sex sex, LifeStage stage);

// ---- composing a name --------------------------------------------------------------------------

/// Localisation keys of the parts of a full name; a part that cannot be built is empty. The views
/// point into the pools, which must outlive them.
struct PersonNameKeys {
    std::string_view given;
    std::string_view patronymic;
    std::string_view surname;
};

/// Builds the keys of "Anna Petrovna Menshikova": the person's own given name, the patronymic from
/// the father's given name in the form for `sex` (empty when the father's name is unknown or is not
/// a man's name of the pool), and the surname of `dynasty_id` in the form for `sex`. Unknown ids give
/// empty parts, never an error.
[[nodiscard]] PersonNameKeys person_name_keys(const NamePools& pools,
                                              std::string_view given_name,
                                              std::string_view father_given_name,
                                              std::string_view dynasty_id,
                                              Sex sex);

/// The given name with this id, or nullptr.
[[nodiscard]] const GivenName* find_given_name(const NamePools& pools, std::string_view id) noexcept;

/// The surname forms of this dynasty, or nullptr.
[[nodiscard]] const SurnameForms* find_surname(const NamePools& pools, std::string_view dynasty_id) noexcept;

}  // namespace pat::sim
