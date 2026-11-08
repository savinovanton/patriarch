#pragma once

#include "pat/content/load_errors.hpp"
#include "pat/core/result.hpp"
#include "pat/data/content_loader.hpp"
#include "pat/data/content_registry.hpp"
#include "pat/data/content_source.hpp"
#include "pat/sim/demography.hpp"

#include <cstdint>
#include <span>
#include <vector>

// Demography content (E7-T07, docs/gdd/01-dynasty.md section 3): turns the demography_rules object
// loaded from data/balance/demography.pd into pat::sim::DemographyRules. Same layering as the economy
// content (economy_content.hpp): pat::data knows the schema but not the rules, pat::sim knows the rules
// but not the files, and this module sees both.
//
// Every problem is reported as a pat::data::LoadError with `[root] file:line:column`, either by the
// loader (syntax, types, ranges: PD0xx...PD110) or by the builder here (PD115, ErrorCode::ContentRule:
// a rule that spans fields, found by sim::validate(const DemographyRules&), or a missing or repeated
// demography_rules object). Bad data never crashes: the result is then an error list and no rules.
//
// The values in data/balance/demography.pd equal sim::kDefaultDemography (a test checks it field by
// field), so the constant stays the fallback for tests and tools that do not load files. Nothing in the
// simulation consumes these rules yet: E7-T10 and E7-T12 take them as an argument.

namespace pat::content {

/// Rules together with the hash of the content they were built from.
struct DemographyContent {
    sim::DemographyRules rules;
    /// ContentRegistry::content_hash() of the whole loaded content (what a save compares); it changes
    /// with every value of the rules.
    std::uint64_t content_hash = 0;
};

using DemographyRulesResult = core::Result<sim::DemographyRules, std::vector<data::LoadError>>;
using DemographyContentResult = core::Result<DemographyContent, std::vector<data::LoadError>>;

/// Builds the rules from an already loaded registry (exactly one demography_rules object) and runs
/// sim::validate() on the result. The registry must come from a load without errors.
[[nodiscard]] DemographyRulesResult build_demography_rules(const data::ContentRegistry& registry);

/// Loads `roots` (roots[0] is the base game, later roots are mods) with content_schemas() and builds
/// the rules. Errors of both stages come back in one list.
[[nodiscard]] DemographyContentResult
load_demography_content(std::span<const data::ContentSource* const> roots);

/// Convenience for a single root.
[[nodiscard]] DemographyContentResult load_demography_content(const data::ContentSource& root);

}  // namespace pat::content
