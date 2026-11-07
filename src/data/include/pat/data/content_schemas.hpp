#pragma once

#include "pat/data/schema.hpp"

// The schemas of the real game content (E4-T09).
//
// This is the single place where object kinds of the shipped game are registered. Everything that
// needs "the content schemas" calls content_schemas(): the loader front-ends, the future contentlint
// (E4-T08) and the reference generator tools/schemadoc, which turns the set into
// docs/dev/data-reference.md (checked in CI, so the reference never drifts from the code).
//
// Adding a kind (provinces, dynasties, traits, skills in E4-T05; later events, armies, ... in E7/E9):
// write a make_<kind>_schema() function and add it in content_schemas(); describe every field with
// ObjectSchema::describe(...). The reference then regenerates automatically: run
// `schemadoc --write docs/dev/data-reference.md` (CI fails while the committed file is stale).

namespace pat::data {

/// Schemas of all object kinds of the game. For now it holds only the documented example kind
/// `trait` (make_example_trait_schema); the real M1 kinds arrive with E4-T05.
[[nodiscard]] SchemaSet content_schemas();

}  // namespace pat::data
