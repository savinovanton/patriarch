#pragma once

#include "pat/data/schema.hpp"

#include <array>
#include <string>
#include <string_view>
#include <vector>

// Schemas of the event kinds (E11-T06, docs/dev/event-engine.md section 4, ADR-0008 D2): `event`,
// `event_modifier` and the single `event_settings` object. The schemas only describe the shape of the
// fields; the rule blocks (`trigger`, `immediate`, `effect`, the accumulators, the durations) are
// Type::rules(): the grammar of .pd carries them unchanged and pat::content compiles them against the
// catalogs of the event engine, with `file:line:column` positions in the errors (EV101..EV114).
//
// Events stay out of the content hash used by the simulation compatibility hash (ADR-0008 D8): their
// own hash is pat::sim::events::EventSet::hash(). Every kind is interned after the others so that
// adding events never shifts the ids of the definitions.

namespace pat::data {

inline constexpr std::string_view kEventKind = "event";
inline constexpr std::string_view kEventModifierKind = "event_modifier";
inline constexpr std::string_view kEventSettingsKind = "event_settings";

/// The kinds of the event engine (callers that pin the simulation hash pass them to
/// ContentRegistry::content_hash_excluding, see m1_scenario.cpp).
inline constexpr std::array<std::string_view, 3> kEventKinds{
    kEventKind, kEventModifierKind, kEventSettingsKind};

/// The values of `event.type` (event-engine.md section 4.2).
[[nodiscard]] const std::vector<std::string>& event_type_names();

/// Registers event, event_modifier and event_settings in `set`.
void add_event_schemas(SchemaSet& set);

}  // namespace pat::data
