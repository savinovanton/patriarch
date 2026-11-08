#pragma once

#include "pat/data/schema.hpp"

namespace pat::data {

/// Adds the M1 definition kinds: province, dynasty, trait and skill (E4-T05).
/// Cross-object constraints and map correspondence are checked by the content layer.
void add_m1_schemas(SchemaSet& schemas);

}  // namespace pat::data
