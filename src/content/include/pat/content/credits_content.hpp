#pragma once

#include "pat/content/load_errors.hpp"
#include "pat/core/result.hpp"
#include "pat/data/content_loader.hpp"
#include "pat/data/content_registry.hpp"
#include "pat/data/content_source.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

// Credits screen entries (UI-213, docs/dev/credits.md): turns the `credit` objects loaded from
// data/credits/*.pd into a sorted list. The entries come from two files: data/credits/generated.pd,
// which tools/gen_credits.py writes from the geodata manifest and the asset registry, and
// data/credits/credits.pd with the hand-written ones (authors, the National Library of Russia, ...).
//
// This module only assembles and validates the list; the window and the text of the sections belong to
// pat::ui and pat::game. Every problem is a pat::data::LoadError with `[root] file:line:column`; bad
// data never crashes, the result is then an error list and no entries.

namespace pat::content {

/// Sections of the credits screen, in the order they are shown.
enum class CreditSection : std::uint8_t { Authors, Data, Assets };

/// One line of the credits. Exactly one of `name_key` / `name` is set; `text_key` and `text` are never
/// both set. A key is read from the string tables (data/loc), a plain `name` / `text` is shown as it is.
struct CreditEntry {
    std::string id;
    CreditSection section = CreditSection::Authors;
    std::int32_t order = 100;
    std::string name_key;
    std::string name;
    std::string text_key;
    std::string text;
    std::string provider;
    std::string license;
    std::string source;  // id in data-sources/geodata.manifest.json, empty when none
    std::string asset;   // path in assets/LICENSES.csv, empty when none
};

/// Entries ordered by section, then `order`, then id.
struct CreditsList {
    std::vector<CreditEntry> entries;
    /// ContentRegistry::content_hash() of the whole loaded content.
    std::uint64_t content_hash = 0;
};

using CreditEntriesResult = core::Result<std::vector<CreditEntry>, std::vector<data::LoadError>>;
using CreditsResult = core::Result<CreditsList, std::vector<data::LoadError>>;

/// Builds the sorted entries from an already loaded registry. A mod may replace an entry by repeating
/// its id (the registry keeps the last one). The registry must come from a load without errors.
[[nodiscard]] CreditEntriesResult build_credits(const data::ContentRegistry& registry);

/// Loads `roots` (roots[0] is the base game, later roots are mods) with content_schemas() and builds
/// the list. Errors of both stages come back in one list.
[[nodiscard]] CreditsResult load_credits_content(std::span<const data::ContentSource* const> roots);

/// Convenience for a single root.
[[nodiscard]] CreditsResult load_credits_content(const data::ContentSource& root);

}  // namespace pat::content
