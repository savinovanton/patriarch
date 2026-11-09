#pragma once

#include "pat/data/schema.hpp"

#include <string_view>

// Schemas of the name and portrait pools (E7-T09, docs/gdd/01-dynasty.md section 2.1,
// docs/dev/names.md, docs/dev/data-reference.md). They describe data/names/*.pd and are registered by
// content_schemas(). The module depends on core only: turning the loaded objects into pat::sim::NamePools
// is the job of the pat::content module (src/content), which sees both.
//
// The value corridors written here mirror pat::sim::validate(const NamePools&) (names.cpp) so that most
// mistakes are reported with file:line:column by the schema (PD104); validate() stays the final check of
// the assembled pools (rules that span objects: patronymic forms for men only, unique portrait ids,
// a portrait for every sex and life stage).

namespace pat::data {

/// A name of the pool. Many objects.
inline constexpr std::string_view kGivenNameKind = "given_name";
/// The written forms of a house's surname. At most one per dynasty.
inline constexpr std::string_view kSurnameKind = "surname";
/// One portrait id of the pool. Many objects.
inline constexpr std::string_view kPortraitKind = "portrait";
/// How strongly families repeat the names of their elders. Exactly one object; a mod replaces it
/// whole by repeating the same id.
inline constexpr std::string_view kNamingRulesKind = "naming_rules";

/// Registers given_name, surname, portrait and naming_rules in `set`.
void add_names_schemas(SchemaSet& set);

}  // namespace pat::data
