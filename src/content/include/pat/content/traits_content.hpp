#pragma once

#include "pat/core/result.hpp"
#include "pat/data/content_loader.hpp"
#include "pat/data/content_registry.hpp"
#include "pat/sim/traits.hpp"

#include <vector>

namespace pat::content {

using TraitCatalogResult = core::Result<sim::TraitCatalog, std::vector<data::LoadError>>;

/// Builds an immutable-ready catalogue from a successfully loaded registry. Trait ids are the
/// stable nonzero trait slots; skill effects use the referenced skill definition's slot. Missing
/// trait tables produce an empty catalogue. Cross-object errors retain their source position.
[[nodiscard]] TraitCatalogResult build_trait_catalog(const data::ContentRegistry& registry);

}  // namespace pat::content
