#pragma once

#include "pat/data/schema.hpp"

#include <string_view>

// Schema of the credits screen entries (UI-213, docs/dev/credits.md). It describes data/credits/*.pd and
// is registered by content_schemas(). The module depends on core only: turning the loaded objects into
// pat::content::CreditsList is the job of the pat::content module (src/content).

namespace pat::data {

/// One entry of the credits screen. Many objects.
inline constexpr std::string_view kCreditKind = "credit";

/// Sections of the credits screen, in the order they are shown.
inline constexpr std::string_view kCreditSections[] = {"authors", "data", "assets"};

/// Registers `credit` in `set`.
void add_credits_schemas(SchemaSet& set);

}  // namespace pat::data
