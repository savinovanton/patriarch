#pragma once

#include "pat/content/load_errors.hpp"
#include "pat/core/result.hpp"
#include "pat/data/content_loader.hpp"
#include "pat/data/content_registry.hpp"
#include "pat/data/content_source.hpp"
#include "pat/sim/marriage_rules.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pat::content {

/// Sorted stable dynasty keys, independent of registry interned ids and World dynasty ids.
using MarriageFaith = std::vector<std::pair<std::string, std::string>>;

struct MarriageContent {
    sim::MarriageRules rules;
    MarriageFaith faith_by_dynasty;
    std::uint64_t content_hash = 0;
};

using MarriageRulesResult = core::Result<sim::MarriageRules, std::vector<data::LoadError>>;
using MarriageContentResult = core::Result<MarriageContent, std::vector<data::LoadError>>;

/// Requires an error-free registry; exactly one complete marriage_rules object is required.
[[nodiscard]] MarriageRulesResult build_marriage_rules(const data::ContentRegistry& registry);
[[nodiscard]] MarriageFaith build_marriage_faith(const data::ContentRegistry& registry);

/// Maps stable dynasty keys at their World ids (id zero is valid) for FaithPolicy.
/// `faith` is sorted by stable dynasty key, as returned by build_marriage_faith().
/// Missing definitions use orthodox. Returned views borrow `faith`, which must remain alive and
/// unchanged while the views or any FaithPolicy constructed from them are used.
[[nodiscard]] std::vector<std::string_view>
bind_marriage_faith(const MarriageFaith& faith, std::span<const std::string_view> dynasty_keys_by_id);

[[nodiscard]] MarriageContentResult load_marriage_content(std::span<const data::ContentSource* const> roots);
[[nodiscard]] MarriageContentResult load_marriage_content(const data::ContentSource& root);

}  // namespace pat::content
