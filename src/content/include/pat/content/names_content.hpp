#pragma once

#include "pat/content/load_errors.hpp"
#include "pat/core/result.hpp"
#include "pat/data/content_loader.hpp"
#include "pat/data/content_registry.hpp"
#include "pat/data/content_source.hpp"
#include "pat/sim/names.hpp"

#include <cstdint>
#include <span>
#include <vector>

// Name and portrait pools (E7-T09, docs/gdd/01-dynasty.md section 2.1, docs/dev/names.md): turns the
// given_name, surname, portrait and naming_rules objects loaded from data/names/*.pd into
// pat::sim::NamePools. Same layering as the demography content (demography_content.hpp): pat::data knows
// the schemas but not the pools, pat::sim knows the pools but not the files, and this module sees both.
//
// Every problem is reported as a pat::data::LoadError with `[root] file:line:column`, either by the
// loader (syntax, types, ranges, references: PD0xx...PD111) or by the builder here (PD115,
// ErrorCode::ContentRule: patronymic forms that do not match the sex, a repeated portrait id or dynasty
// surname, a dynasty without a surname, a missing or repeated naming_rules object, a pool that does not
// cover every sex and life stage). Bad data never crashes: the result is then an error list and no pools.
//
// The text behind every key lives in data/loc/<language>/names.pd; the keys are checked by
// contentlint like all other keys. Nothing in the simulation consumes the pools yet: the births of
// E7-T12 take them as an argument.

namespace pat::content {

/// Pools together with the hash of the content they were built from.
struct NamesContent {
    sim::NamePools pools;
    /// ContentRegistry::content_hash() of the whole loaded content.
    std::uint64_t content_hash = 0;
};

using NamePoolsResult = core::Result<sim::NamePools, std::vector<data::LoadError>>;
using NamesContentResult = core::Result<NamesContent, std::vector<data::LoadError>>;

/// Builds the pools from an already loaded registry and runs sim::validate() on the result. Names are
/// in the order of their ids, portraits in the order of their portrait ids. The registry must come
/// from a load without errors.
[[nodiscard]] NamePoolsResult build_name_pools(const data::ContentRegistry& registry);

/// Loads `roots` (roots[0] is the base game, later roots are mods) with content_schemas() and builds
/// the pools. Errors of both stages come back in one list.
[[nodiscard]] NamesContentResult load_names_content(std::span<const data::ContentSource* const> roots);

/// Convenience for a single root.
[[nodiscard]] NamesContentResult load_names_content(const data::ContentSource& root);

}  // namespace pat::content
