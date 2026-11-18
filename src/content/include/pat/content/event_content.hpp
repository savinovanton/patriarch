#pragma once

#include "pat/content/load_errors.hpp"
#include "pat/core/result.hpp"
#include "pat/data/content_loader.hpp"
#include "pat/data/content_registry.hpp"
#include "pat/data/content_source.hpp"
#include "pat/sim/events/catalog.hpp"
#include "pat/sim/events/event_set.hpp"

#include <cstdint>
#include <functional>
#include <span>
#include <string_view>
#include <vector>

// Event content (E11-T06, docs/dev/event-engine.md sections 4 .. 7 and 16, ADR-0008): turns the
// `event`, `event_modifier` and `event_settings` objects of data/events/ and data/balance/events.pd into
// a pat::sim::events::EventSet, compiling every rule block to a bytecode Program.
//
// Layering as in court_content.hpp: pat::data knows the schemas and the opaque rule blocks
// (Type::rules()), pat::sim knows the catalogs and the machine, this module sees both. Every problem
// is a pat::data::LoadError with `[root] file:line:column`, either from the loader (syntax, types,
// ranges, duplicate ids: PD112) or from the compiler (EV101..EV114); all of them are reported at once.
//
// The events stay out of the content hash of the simulation (ADR-0008 D8): the hash of the compiled
// rules is EventSet::hash(), and event_content_hash() below is the content hash without the event kinds.

namespace pat::content {

struct EventBuildOptions {
    /// True when the localization key exists; unset skips the check (EV111).
    std::function<bool(std::string_view)> has_loc_key;
    /// True when the image pool exists; unset skips the check (EV104).
    std::function<bool(std::string_view)> has_image;
    /// Replaces the 64-bit key function (FNV-1a of the id). Tests use it to provoke a collision (EV105).
    std::function<std::uint64_t(std::string_view)> key_function;
};

struct EventContent {
    sim::events::EventSet set;
    /// ContentRegistry::content_hash_excluding(event kinds): what the simulation compares.
    std::uint64_t content_hash = 0;
};

using EventSetResult = core::Result<sim::events::EventSet, std::vector<data::LoadError>>;
using EventContentResult = core::Result<EventContent, std::vector<data::LoadError>>;

/// Builds the set from an already loaded registry: every `event` and `event_modifier`, and the single
/// `event_settings` "main" (defaults when absent). The registry must come from a load without errors.
[[nodiscard]] EventSetResult build_event_set(const data::ContentRegistry& registry,
                                             const sim::events::Catalog& catalog,
                                             const EventBuildOptions& options = {});

/// The content hash without the event kinds.
[[nodiscard]] std::uint64_t event_content_hash(const data::ContentRegistry& registry);

/// Loads `roots` (roots[0] is the base game, later roots are mods) with content_schemas() and builds
/// the set. Errors of both stages come back in one list.
[[nodiscard]] EventContentResult load_event_content(std::span<const data::ContentSource* const> roots,
                                                    const sim::events::Catalog& catalog,
                                                    const EventBuildOptions& options = {});
[[nodiscard]] EventContentResult load_event_content(const data::ContentSource& root,
                                                    const sim::events::Catalog& catalog,
                                                    const EventBuildOptions& options = {});

}  // namespace pat::content
