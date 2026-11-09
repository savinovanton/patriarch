#pragma once

#include "pat/data/content_loader.hpp"
#include "pat/data/content_registry.hpp"

#include <vector>

namespace pat::content {

/// Checks cross-object rules of the M1 definitions (E4-T05). The registry must come from an
/// error-free load with content_schemas(). Empty M1 tables are allowed for older or economy-only
/// content. Every problem is a source-located PD115, sorted like loader errors.
[[nodiscard]] std::vector<data::LoadError> validate_m1_definitions(const data::ContentRegistry& registry);

}  // namespace pat::content
