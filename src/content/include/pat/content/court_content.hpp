#pragma once

#include "pat/content/load_errors.hpp"
#include "pat/core/result.hpp"
#include "pat/data/content_loader.hpp"
#include "pat/data/content_registry.hpp"
#include "pat/data/content_source.hpp"
#include "pat/sim/court_rules.hpp"

#include <cstdint>
#include <span>
#include <vector>

// Court content (E10-T04, docs/dev/court-model.md section 11): turns the objects of data/court/*.pd and
// data/balance/court.pd into pat::sim::CourtRules.
//
// Layering as in economy_content.hpp: pat::data knows the schemas, pat::sim does not know about data
// files, this module sees both. Every problem is a pat::data::LoadError with
// `[root] file:line:column`, either from the loader (syntax, types, ranges, dangling references to
// traits, skills and province kinds) or from the builder here (PD115: a rule that spans objects, for
// example two offices with the same slot, or a number outside the corridor of sim::validate()).
//
// The values in data/ equal CourtRules::defaults() (a test checks it field by field), so defaults()
// stays the fallback for tests and tools that do not load files.

namespace pat::content {

struct CourtContent {
    sim::CourtRules rules;
    /// ContentRegistry::content_hash() of the whole loaded content (what a save compares).
    std::uint64_t content_hash = 0;
};

using CourtRulesResult = core::Result<sim::CourtRules, std::vector<data::LoadError>>;
using CourtContentResult = core::Result<CourtContent, std::vector<data::LoadError>>;

/// Builds the rules from an already loaded registry: all `office`, `servant_role` and `court_party`
/// objects and exactly one `court_balance`, with the cross-object checks and sim::validate() on the
/// result. The registry must come from a load without errors.
[[nodiscard]] CourtRulesResult build_court_rules(const data::ContentRegistry& registry);

/// Loads `roots` (roots[0] is the base game, later roots are mods) with content_schemas() and builds
/// the rules. Errors of both stages come back in one list.
[[nodiscard]] CourtContentResult load_court_content(std::span<const data::ContentSource* const> roots);
[[nodiscard]] CourtContentResult load_court_content(const data::ContentSource& root);

}  // namespace pat::content
