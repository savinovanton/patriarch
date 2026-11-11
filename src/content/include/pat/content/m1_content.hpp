#pragma once

#include "pat/data/content_loader.hpp"
#include "pat/data/content_registry.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace pat::content {

/// Checks cross-object rules of the M1 definitions (E4-T05). The registry must come from an
/// error-free load with content_schemas(). Empty M1 tables are allowed for older or economy-only
/// content. Every problem is a source-located PD115, sorted like loader errors.
[[nodiscard]] std::vector<data::LoadError> validate_m1_definitions(const data::ContentRegistry& registry);

struct HonorRank {
    std::string id;
    std::int32_t min_honor;
    std::string name_key;
};

/// Builds the ascending rank table after validate_m1_definitions has succeeded.
[[nodiscard]] std::vector<HonorRank> build_honor_ranks(const data::ContentRegistry& registry);

/// Uses the greatest inclusive threshold not above honor. Empty tables have no rank.
[[nodiscard]] const HonorRank* honor_rank(std::span<const HonorRank> ranks, std::int32_t honor);

}  // namespace pat::content
