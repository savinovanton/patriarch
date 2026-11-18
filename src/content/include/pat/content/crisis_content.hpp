#pragma once

#include "pat/content/load_errors.hpp"
#include "pat/core/result.hpp"
#include "pat/data/content_loader.hpp"
#include "pat/data/content_registry.hpp"
#include "pat/data/content_source.hpp"
#include "pat/sim/crisis_rules.hpp"

#include <cstdint>
#include <span>
#include <vector>

// Crisis content (E12-T04, docs/dev/throne-crisis-model.md section 11): turns the objects of
// data/balance/crisis.pd (`crisis_balance`, `claim_rules`) and data/crisis/projects.pd (`emperor_project`)
// into pat::sim::crisis::CrisisRules.
//
// Layering as in court_content.hpp: pat::data knows the schemas, pat::sim does not know about data files,
// this module sees both. Every problem is a pat::data::LoadError with `[root] file:line:column`, either
// from the loader (syntax, types, representation ranges) or from the builder here (PD115: a rule that
// spans objects, such as a wrong number of projects or a number outside the corridor of
// sim::crisis::validate()).
//
// The values in data/ equal CrisisRules::defaults() (a test checks it field by field), so defaults()
// stays the fallback for tests and tools that do not load files. The three kinds are not part of the
// content hash of the M1 scenario (E12-T22 switches them on deliberately).

namespace pat::content {

struct CrisisContent {
    sim::crisis::CrisisRules rules;
    /// ContentRegistry::content_hash() of the whole loaded content (what a save compares).
    std::uint64_t content_hash = 0;
};

using CrisisRulesResult = core::Result<sim::crisis::CrisisRules, std::vector<data::LoadError>>;
using CrisisContentResult = core::Result<CrisisContent, std::vector<data::LoadError>>;

/// Builds the rules from an already loaded registry: exactly one `crisis_balance` and one `claim_rules`
/// and the `emperor_project` objects, with sim::crisis::validate() on the result. The registry must come
/// from a load without errors.
[[nodiscard]] CrisisRulesResult build_crisis_rules(const data::ContentRegistry& registry);

/// Loads `roots` (roots[0] is the base game, later roots are mods) with content_schemas() and builds the
/// rules. Errors of both stages come back in one list.
[[nodiscard]] CrisisContentResult load_crisis_content(std::span<const data::ContentSource* const> roots);
[[nodiscard]] CrisisContentResult load_crisis_content(const data::ContentSource& root);

}  // namespace pat::content
