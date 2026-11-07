#pragma once

#include "pat/data/schema.hpp"

// Documented example schemas (E4-T03). They show how object kinds are described and are used by the
// schema tests; the real content schemas of milestone M1 arrive with E4-T05.

namespace pat::data {

/// Schema of the example kind `trait`:
///
///   trait "greedy" {
///       name_key = "trait.greedy.name"        # string, required
///       cost = 3                              # int 0..10, required
///       group = vice                          # enum vice | virtue | neutral, default neutral
///       opposite = trait:honest               # reference to a trait, optional
///       weight = 1.5                          # fixed32 0..100, default 1
///       inheritable = yes                     # bool, default no
///       since = 1703.01.01                    # Julian date, optional
///       tags = [ "court", "money" ]           # list of string, optional
///       skills = { stewardship = 2 }          # map identifier -> int -10..10, optional
///       note = "first"                        # string, may repeat
///       chance { base = 0.05  max = 0.5 }     # nested block: base (fixed64 0..1, required), max (default 1)
///   }
[[nodiscard]] ObjectSchema make_example_trait_schema();

/// A set holding the example kinds (currently only `trait`).
[[nodiscard]] SchemaSet make_example_schema_set();

}  // namespace pat::data
