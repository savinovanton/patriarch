#pragma once

#include "pat/data/schema.hpp"

#include <array>
#include <string_view>

// Schema of the province graph (E16-T04, docs/dev/army-model.md section 5, docs/dev/data-reference.md).
// It describes data/scenarios/<id>/graph.pd, which `mapgen --export-graph` writes from the map, and is
// registered by content_schemas(). The module depends on core only: turning the object into
// pat::sim::army::ProvinceGraph is the job of pat::content (src/content/graph_content.cpp), which also
// checks the cross-object rules (nodes are provinces, no duplicate edges, the graph is connected).

namespace pat::data {

inline constexpr std::string_view kProvinceGraphKind = "province_graph";

/// The graph kinds. They stay out of the simulation compatibility hash of M1 until the army systems
/// are installed in the scenario (E16-T20); ProvinceGraph::hash() is the rules hash of the graph.
inline constexpr std::array<std::string_view, 1> kGraphKinds{kProvinceGraphKind};

/// Adds `province_graph` (interned last: its ids never shift the definition ids of the world).
void add_graph_schemas(SchemaSet& schemas);

}  // namespace pat::data
