#pragma once

#include "pat/sim/events/catalog.hpp"

#include <string>

namespace pat::sim::events {

/// The Markdown reference of the trigger, value, effect and state-operation entries of `catalog`
/// (docs/dev/event-catalog-reference.md): code, name, scope, arguments, tooltip keys, example and the
/// stub mark. Deterministic (entries by code, LF line endings); `schemadoc --catalog` prints it and
/// `schemadoc --catalog --check FILE` verifies the committed file (E11-T07).
[[nodiscard]] std::string render_catalog_reference(const Catalog& catalog);

}  // namespace pat::sim::events
