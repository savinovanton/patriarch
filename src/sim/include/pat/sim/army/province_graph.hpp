#pragma once

#include "pat/core/result.hpp"
#include "pat/core/string_interner.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

// Province graph of the simulation (E16-T04, docs/dev/army-model.md section 5, ADR-0012 D1/D16).
//
// `sim` does not depend on `mapdata`: the adjacency of the map reaches the simulation as CONTENT,
// data/scenarios/m1/graph.pd, which `mapgen --export-graph` writes from the PMD map and which
// pat::content::build_province_graph() turns into this value (a test keeps the file equal to the map).
// The graph is rules, not state: it is not part of the World, it is not saved, and a save never depends
// on it (a path of an order is saved as a list of provinces, army-model.md section 3).
//
// Nodes are dense indices 0..N-1 in the order of the province keys; every node remembers the content
// definition of its province, so a scenario can bind nodes to its World provinces. Edges are undirected,
// stored once with a < b and mirrored into sorted adjacency lists. Lengths are whole kilometres. No
// floating point anywhere: distances are integer square roots.

namespace pat::sim::army {

using NodeIndex = std::uint16_t;
inline constexpr NodeIndex kNoNode = 0xFFFF;
/// Room above the 400 provinces of the full map (ADR-0002 P3); NodeIndex stays 16-bit.
inline constexpr std::size_t kMaxGraphNodes = 4096;
/// Shortest edge allowed (army-model.md section 5: "not less than 10 km").
inline constexpr std::int32_t kMinEdgeLengthKm = 10;
inline constexpr std::int32_t kMaxEdgeLengthKm = 10'000;
/// Coordinates stay within this many km of the origin (squares of differences fit in int64).
inline constexpr std::int32_t kMaxCoordinateKm = 100'000;

/// Road along an edge; stable numbers (army-model.md section 3: 0 none, 1 tract, 2 reserved).
enum class Road : std::uint8_t { None = 0, Tract = 1 };
inline constexpr std::size_t kRoadCount = 2;

struct GraphNode {
    core::StringId definition;  ///< Province definition in the content registry (default in tests).
    std::int32_t x_km = 0;      ///< Position for the A* heuristic; x grows east, y grows south.
    std::int32_t y_km = 0;
    [[nodiscard]] bool operator==(const GraphNode&) const = default;
};

struct GraphEdge {
    NodeIndex a = kNoNode;  ///< a < b after build().
    NodeIndex b = kNoNode;
    std::int32_t length_km = 0;
    Road road = Road::None;
    [[nodiscard]] bool operator==(const GraphEdge&) const = default;
};

/// One entry of an adjacency list: the other end of an edge.
struct Neighbor {
    NodeIndex node = kNoNode;
    std::int32_t length_km = 0;
    Road road = Road::None;
    std::uint32_t edge = 0;  ///< Index into ProvinceGraph::edges().
    [[nodiscard]] bool operator==(const Neighbor&) const = default;
};

/// Why a graph could not be built; `index` is the node or edge at fault.
enum class GraphIssue : std::uint8_t {
    Empty = 1,            ///< No nodes.
    TooManyNodes = 2,     ///< More than kMaxGraphNodes.
    BadCoordinate = 3,    ///< Node: |x| or |y| above kMaxCoordinateKm.
    BadNode = 4,          ///< Edge: an end is not a node.
    SelfLoop = 5,         ///< Edge: both ends are the same node.
    DuplicateEdge = 6,    ///< Edge: the pair is already connected.
    BadLength = 7,        ///< Edge: length outside kMinEdgeLengthKm..kMaxEdgeLengthKm.
    ShorterThanLine = 8,  ///< Edge: length below the straight line between its nodes (A* needs it).
    BadRoad = 9,          ///< Edge: road value outside Road.
};

struct GraphError {
    GraphIssue issue = GraphIssue::Empty;
    std::size_t index = 0;
    [[nodiscard]] bool operator==(const GraphError&) const = default;
};

[[nodiscard]] std::string_view graph_issue_name(GraphIssue issue) noexcept;

class ProvinceGraph {
public:
    ProvinceGraph() = default;

    /// Checks the nodes and edges (GraphIssue) and builds the adjacency lists and the connected
    /// components. Edges may come in any order and orientation; they are stored with a < b, sorted.
    [[nodiscard]] static core::Result<ProvinceGraph, GraphError> build(std::vector<GraphNode> nodes,
                                                                       std::vector<GraphEdge> edges);

    [[nodiscard]] std::size_t node_count() const noexcept { return nodes_.size(); }
    [[nodiscard]] std::span<const GraphNode> nodes() const noexcept { return nodes_; }
    [[nodiscard]] const GraphNode& node(NodeIndex index) const { return nodes_[index]; }
    [[nodiscard]] std::span<const GraphEdge> edges() const noexcept { return edges_; }
    /// Neighbours of a node sorted by node index (deterministic iteration for the search).
    [[nodiscard]] std::span<const Neighbor> neighbors(NodeIndex index) const noexcept;
    /// The edge between two nodes (either order), or nullptr.
    [[nodiscard]] const GraphEdge* find_edge(NodeIndex a, NodeIndex b) const noexcept;
    /// Node with this content definition, or kNoNode.
    [[nodiscard]] NodeIndex find_node(core::StringId definition) const noexcept;

    /// Connected component of a node (0-based, numbered in node order).
    [[nodiscard]] std::uint16_t component(NodeIndex index) const { return component_[index]; }
    [[nodiscard]] std::size_t component_count() const noexcept { return component_count_; }
    [[nodiscard]] bool connected() const noexcept { return component_count_ == 1; }

    /// floor(sqrt(dx^2 + dy^2)) in km between two nodes: the heuristic distance of A*.
    [[nodiscard]] std::int64_t straight_km(NodeIndex a, NodeIndex b) const;

    /// Canonical hash of positions and edges (not of the content definitions, whose ids depend on the
    /// interning order): the version of the graph in the path cache and the rules summary.
    [[nodiscard]] std::uint64_t hash() const noexcept { return hash_; }

    [[nodiscard]] bool operator==(const ProvinceGraph&) const = default;

private:
    std::vector<GraphNode> nodes_;
    std::vector<GraphEdge> edges_;
    std::vector<std::uint32_t> first_;  ///< CSR offsets into neighbors_, size N + 1.
    std::vector<Neighbor> neighbors_;
    std::vector<std::uint16_t> component_;
    std::size_t component_count_ = 0;
    std::uint64_t hash_ = 0;
};

/// floor(sqrt(dx^2 + dy^2)) without floating point.
[[nodiscard]] std::int64_t integer_distance(std::int64_t dx, std::int64_t dy) noexcept;

// ---- days on an edge (army-model.md section 9.1, MIL-A1) ---------------------------------------------

/// Every step of the movement formula, so a tooltip (Breakdown 18200, E16-T07) shows the applied terms:
/// v = speed*1000 -> *terrain/1000 -> *season/1000 -> *road/1000 (truncated after each step),
/// days = max(1, ceil(length*1000 / v)).
struct EdgeTime {
    std::int64_t v_base = 0;     ///< speed_kmd * 1000
    std::int64_t v_terrain = 0;  ///< after the terrain of the destination
    std::int64_t v_season = 0;   ///< after the season of the month of departure
    std::int64_t v_road = 0;     ///< after the road of the edge: the speed used, km/day * 1000 (>= 1)
    std::int32_t days = 0;
    [[nodiscard]] bool operator==(const EdgeTime&) const = default;
};

/// Pure function of the movement rules: days to cross an edge. All factors are per-mille of one;
/// non-positive inputs are clamped (speed to 1 m/day) so the result is always at least one day.
[[nodiscard]] EdgeTime edge_days(std::int32_t length_km,
                                 std::int32_t speed_kmd,
                                 std::int32_t terrain_move_pm,
                                 std::int32_t season_move_pm,
                                 std::int32_t road_move_pm) noexcept;

}  // namespace pat::sim::army
