#pragma once

#include "pat/data/schema.hpp"

#include <string>

// Markdown reference of a SchemaSet (E4-T09, docs/dev/data-authoring.md, docs/dev/data-reference.md).
//
// The reference is generated from the schemas themselves, so it cannot drift from the code: the tool
// tools/schemadoc prints it, CI and a unit test compare the committed docs/dev/data-reference.md with
// the freshly generated text.
//
// Determinism: the output depends only on the schema set. Kinds are sorted by name (byte order),
// fields keep their declaration order, numbers are printed from the exact fixed-point raw values with
// integer arithmetic (no floating point), line endings are LF, there are no timestamps or paths.

namespace pat::data {

/// Index of kinds plus one section per kind (and per nested block), without the document preamble.
/// Always ends with exactly one '\n'.
[[nodiscard]] std::string render_reference_body(const SchemaSet& schemas);

/// The full text of docs/dev/data-reference.md: the "generated, do not edit" banner, the Russian
/// introduction and render_reference_body(schemas).
[[nodiscard]] std::string render_reference_document(const SchemaSet& schemas);

}  // namespace pat::data
