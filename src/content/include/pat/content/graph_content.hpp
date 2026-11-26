#pragma once

#include "pat/core/result.hpp"
#include "pat/data/content_loader.hpp"
#include "pat/data/content_registry.hpp"
#include "pat/data/content_source.hpp"
#include "pat/mapdata/graph_export.hpp"
#include "pat/sim/army/province_graph.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Province graph content (E16-T04, docs/dev/army-model.md section 5): turns a `province_graph` object of
// data/scenarios/<id>/graph.pd into pat::sim::army::ProvinceGraph.
//
// Layering as in crisis_content.hpp: pat::data knows the schema, pat::sim does not know about data files,
// this module sees both. The file is written by `mapgen --export-graph` from the map (pat::mapdata); a
// test compares graph_topology() of the loaded file with mapdata::export_province_graph() of the M1 map.
// Cross-object problems are PD115 errors at the node or edge: a node that is not a province, an edge to a
// province that is not a node, a loop, a duplicate edge, an edge shorter than the straight line, and a
// graph that is not connected. The kind is outside the content hash of the M1 scenario until E16-T20;
// ProvinceGraph::hash() is its own rules hash.

namespace pat::content {

/// Horizontal scale of the procedural M1 map (m1_map_params()) in graph.pd: 5 km per pixel [predl.].
inline constexpr std::uint32_t kM1GraphMetresPerPx = 5000;
inline constexpr std::string_view kM1GraphId = "m1";

struct ProvinceGraphContent {
    sim::army::ProvinceGraph graph;
    std::vector<std::string> keys;  ///< Province key of every node, in node order (sorted).
    /// ContentRegistry::content_hash() of the whole loaded content.
    std::uint64_t content_hash = 0;
};

using ProvinceGraphResult = core::Result<ProvinceGraphContent, std::vector<data::LoadError>>;

/// Builds the graph `graph_id` from an error-free registry. Nodes are ordered by province key.
[[nodiscard]] ProvinceGraphResult build_province_graph(const data::ContentRegistry& registry,
                                                       std::string_view graph_id);

/// Checks every `province_graph` object of the registry (contentlint); empty when all are valid or
/// there are none.
[[nodiscard]] std::vector<data::LoadError> validate_province_graphs(const data::ContentRegistry& registry);

/// Loads `roots` with content_schemas() and builds the graph `graph_id`.
[[nodiscard]] ProvinceGraphResult
load_province_graph_content(std::span<const data::ContentSource* const> roots, std::string_view graph_id);
[[nodiscard]] ProvinceGraphResult load_province_graph_content(const data::ContentSource& root,
                                                              std::string_view graph_id);

/// Nodes, edges and lengths of a loaded graph in the export form (roads are not part of the map).
[[nodiscard]] mapdata::GraphExport graph_topology(const ProvinceGraphContent& content);

}  // namespace pat::content
