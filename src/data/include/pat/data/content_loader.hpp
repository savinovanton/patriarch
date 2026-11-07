#pragma once

#include "pat/data/content_registry.hpp"
#include "pat/data/content_source.hpp"
#include "pat/data/pd_parser.hpp"
#include "pat/data/schema.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

// Content loader (E4-T04, docs/dev/data-loader.md): reads all `*.pd` files of an ordered list of
// roots, parses and validates them, applies overrides, resolves cross references and builds the
// ContentRegistry. Every problem is collected; the result lists them all, sorted deterministically.

namespace pat::data {

/// Upgrades a parsed file by exactly one format version (from N to N + 1) in place. Returns an
/// error message on failure (reported as PD114 at the header).
using MigrationStep = std::function<std::optional<std::string>(ParsedFile&)>;

/// Chain of per-version migrations. Version 1 is current today, so the registry is empty; the first
/// grammar change registers `add(1, ...)` here.
class MigrationRegistry {
public:
    /// Registers the step that turns a version-`from` file into a version-`from + 1` file.
    MigrationRegistry& add(std::int64_t from, MigrationStep step);
    [[nodiscard]] bool has(std::int64_t from) const;
    [[nodiscard]] const MigrationStep* find(std::int64_t from) const;

private:
    std::map<std::int64_t, MigrationStep> steps_;
};

struct LoadOptions {
    /// The format version the schemas and the rest of the engine understand.
    std::int64_t current_version = kCurrentFormatVersion;
    MigrationRegistry migrations;
};

/// A problem found while loading, with the root and file it belongs to.
struct LoadError {
    std::uint32_t root = 0;  // index in the list of roots
    std::string root_name;   // ContentSource::name()
    std::string file;        // path relative to the root; empty for root-level problems
    Diagnostic diagnostic;   // position {0, 0} when there is no position (unreadable file or root)

    /// `[root 1 'mods/a'] traits/core.pd:3:5: error[PD111]: message`
    [[nodiscard]] std::string format() const;
};

struct LoadResult {
    ContentRegistry registry;
    std::vector<LoadError> errors;  // sorted by (root, file, position, code, message)

    [[nodiscard]] bool ok() const noexcept { return errors.empty(); }
};

/// Loads `roots` in order: roots[0] is the base game, later roots are mods and override earlier
/// ones. Roots must not be null and must outlive the call.
///
/// Per root: the files are those with the exact extension `.pd`, visited in bytewise path order.
/// Every file is parsed, its `format_version` checked (newer than current or without a migration
/// path: PD021; failed migration: PD114), migrated, and each top-level object validated against
/// `schemas`. A later root replaces an earlier object of the same kind and id completely; the same
/// (kind, id) twice in ONE root is PD112 (the first one in path/file order is kept). References
/// declared by the schemas are resolved against the effective content (PD111). Unreadable roots or
/// files are PD113. On errors the registry holds everything that was valid.
[[nodiscard]] LoadResult load_content(std::span<const ContentSource* const> roots,
                                      const SchemaSet& schemas,
                                      const LoadOptions& options = {});

/// Convenience for a single root.
[[nodiscard]] LoadResult
load_content(const ContentSource& root, const SchemaSet& schemas, const LoadOptions& options = {});

}  // namespace pat::data
