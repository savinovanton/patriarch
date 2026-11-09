#pragma once

#include "pat/content/load_errors.hpp"
#include "pat/core/result.hpp"
#include "pat/data/content_loader.hpp"
#include "pat/data/content_registry.hpp"
#include "pat/data/content_source.hpp"
#include "pat/sim/economy/rules.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Economy content (E9-T18, docs/dev/economy-model.md sections 11 and 15): turns the objects loaded
// from data/economy/*.pd into pat::sim::economy::EconomyRules.
//
// Layering. pat::data (the .pd pipeline and the schemas of the economy kinds) depends on core only;
// pat::sim must not depend on pat::data. This small module sits above both, so neither of them has to
// know the other. The simulation still receives a plain EconomyRules value as an argument.
//
// Every problem is reported as a pat::data::LoadError with `[root] file:line:column`, either by the
// loader (syntax, types, ranges, dangling references: PD0xx...PD114) or by the builder here
// (PD115: a rule that spans objects, for example two goods with the same slot). Bad data never
// crashes: the result is then an error list and no rules.
//
// The values in data/economy/ equal EconomyRules::defaults() (a test checks it field by field), so
// defaults() stays the fallback for tests and tools that do not load files.

namespace pat::content {

/// Rules together with the hash of the content they were built from.
struct EconomyContent {
    sim::economy::EconomyRules rules;
    /// ContentRegistry::content_hash() of the whole loaded content (what a save compares).
    std::uint64_t content_hash = 0;
};

using EconomyRulesResult = core::Result<sim::economy::EconomyRules, std::vector<data::LoadError>>;
using EconomyContentResult = core::Result<EconomyContent, std::vector<data::LoadError>>;

/// Resolves a content key through the same table used to load province-kind rules.
[[nodiscard]] std::optional<sim::economy::ProvinceKind> province_kind_from_key(std::string_view key);

/// Builds the rules from an already loaded registry (one economy_rules object, all goods, groups,
/// province kinds, tax and recruit levels, buildings), checks the cross-object rules and runs
/// sim::economy::validate() on the result. The registry must come from a load without errors.
[[nodiscard]] EconomyRulesResult build_economy_rules(const data::ContentRegistry& registry);

/// Loads `roots` (roots[0] is the base game, later roots are mods) with content_schemas() and builds
/// the rules. Errors of both stages come back in one list.
[[nodiscard]] EconomyContentResult load_economy_content(std::span<const data::ContentSource* const> roots);

/// Convenience for a single root.
[[nodiscard]] EconomyContentResult load_economy_content(const data::ContentSource& root);

}  // namespace pat::content
