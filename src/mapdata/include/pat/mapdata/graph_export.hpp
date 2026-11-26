#pragma once

#include "pat/mapdata/map_data.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Export of the province graph for the simulation (E16-T04, docs/dev/army-model.md section 5).
//
// `sim` does not read maps: the adjacency reaches it as content, data/scenarios/<id>/graph.pd, which
// `mapgen --export-graph` writes with format_graph_pd(). The same export_province_graph() is what the
// content test compares the shipped file with, so the file and the map cannot drift apart.
//
// Coordinates: centroid pixels times the horizontal scale (metres per pixel), rounded to whole km.
// Length of an edge: ceil(sqrt(dx^2 + dy^2)) of the km coordinates, at least kMinGraphEdgeKm, so an edge
// is never shorter than the straight line (the A* heuristic stays admissible). Roads are not in the map:
// they are added to the file by hand (`road = tract`) and ignored by the comparison.

namespace pat::mapdata {

inline constexpr std::int32_t kMinGraphEdgeKm = 10;

struct GraphExportNode {
    std::string key;  ///< Province key; PMD1 maps use p0001...pNNNN (owner decision #201).
    std::int32_t x_km = 0;
    std::int32_t y_km = 0;
    [[nodiscard]] bool operator==(const GraphExportNode&) const = default;
};

struct GraphExportEdge {
    std::uint16_t a = 0;  ///< Index into GraphExport::nodes, a < b.
    std::uint16_t b = 0;
    std::int32_t length_km = 0;
    [[nodiscard]] bool operator==(const GraphExportEdge&) const = default;
};

/// Nodes in province id order, edges sorted by (a, b).
struct GraphExport {
    std::vector<GraphExportNode> nodes;
    std::vector<GraphExportEdge> edges;
    [[nodiscard]] bool operator==(const GraphExport&) const = default;
};

/// Key of a province of a PMD1 map: "p" and the id with at least four digits.
[[nodiscard]] std::string pmd1_province_key(std::uint16_t id);

/// Nodes and edges of the map's province table (neighbours are symmetric in a valid map).
/// `metres_per_px` must be positive.
[[nodiscard]] GraphExport export_province_graph(const MapData& map, std::uint32_t metres_per_px);

/// The text of a graph.pd file with one `province_graph "<graph_id>"` object. The header comment names
/// the scale, so the file says how to regenerate it; the text depends on nothing else.
[[nodiscard]] std::string
format_graph_pd(const GraphExport& graph, std::string_view graph_id, std::uint32_t metres_per_px);

}  // namespace pat::mapdata
