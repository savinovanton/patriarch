#include "pat/sim/army/province_graph.hpp"

#include "pat/core/detail/wide_math.hpp"
#include "pat/core/hash_stream.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace pat::sim::army {

namespace {

core::Result<ProvinceGraph, GraphError> fail(GraphIssue issue, std::size_t index) {
    return core::Result<ProvinceGraph, GraphError>::err(GraphError{issue, index});
}

std::int64_t square(std::int64_t v) noexcept {
    return v * v;
}

}  // namespace

std::string_view graph_issue_name(GraphIssue issue) noexcept {
    switch (issue) {
        case GraphIssue::Empty:
            return "the graph has no nodes";
        case GraphIssue::TooManyNodes:
            return "too many nodes";
        case GraphIssue::BadCoordinate:
            return "node coordinate out of range";
        case GraphIssue::BadNode:
            return "edge end is not a node";
        case GraphIssue::SelfLoop:
            return "edge connects a node to itself";
        case GraphIssue::DuplicateEdge:
            return "duplicate edge";
        case GraphIssue::BadLength:
            return "edge length out of range";
        case GraphIssue::ShorterThanLine:
            return "edge is shorter than the straight line between its nodes";
        case GraphIssue::BadRoad:
            return "unknown road";
    }
    return "unknown graph issue";
}

std::int64_t integer_distance(std::int64_t dx, std::int64_t dy) noexcept {
    const auto sum = static_cast<std::uint64_t>(square(dx)) + static_cast<std::uint64_t>(square(dy));
    return static_cast<std::int64_t>(core::detail::isqrt_u128(core::detail::U128{0, sum}));
}

core::Result<ProvinceGraph, GraphError> ProvinceGraph::build(std::vector<GraphNode> nodes,
                                                             std::vector<GraphEdge> edges) {
    if (nodes.empty()) {
        return fail(GraphIssue::Empty, 0);
    }
    if (nodes.size() > kMaxGraphNodes) {
        return fail(GraphIssue::TooManyNodes, kMaxGraphNodes);
    }
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        const auto& n = nodes[i];
        if (n.x_km < -kMaxCoordinateKm || n.x_km > kMaxCoordinateKm || n.y_km < -kMaxCoordinateKm ||
            n.y_km > kMaxCoordinateKm) {
            return fail(GraphIssue::BadCoordinate, i);
        }
    }

    // Normalize (a < b), check, then sort by (a, b) and look for duplicates; the reported index is the
    // position in the input.
    std::vector<std::pair<GraphEdge, std::size_t>> order;
    order.reserve(edges.size());
    for (std::size_t i = 0; i < edges.size(); ++i) {
        GraphEdge e = edges[i];
        if (e.a >= nodes.size() || e.b >= nodes.size()) {
            return fail(GraphIssue::BadNode, i);
        }
        if (e.a == e.b) {
            return fail(GraphIssue::SelfLoop, i);
        }
        if (e.length_km < kMinEdgeLengthKm || e.length_km > kMaxEdgeLengthKm) {
            return fail(GraphIssue::BadLength, i);
        }
        if (static_cast<std::size_t>(e.road) >= kRoadCount) {
            return fail(GraphIssue::BadRoad, i);
        }
        const GraphNode& na = nodes[e.a];
        const GraphNode& nb = nodes[e.b];
        // length^2 >= dx^2 + dy^2 keeps the A* heuristic admissible and consistent.
        if (square(e.length_km) <
            square(std::int64_t{na.x_km} - nb.x_km) + square(std::int64_t{na.y_km} - nb.y_km)) {
            return fail(GraphIssue::ShorterThanLine, i);
        }
        if (e.a > e.b) {
            std::swap(e.a, e.b);
        }
        order.emplace_back(e, i);
    }
    std::sort(order.begin(), order.end(), [](const auto& l, const auto& r) {
        return std::pair(l.first.a, l.first.b) != std::pair(r.first.a, r.first.b)
                   ? std::pair(l.first.a, l.first.b) < std::pair(r.first.a, r.first.b)
                   : l.second < r.second;
    });
    for (std::size_t i = 1; i < order.size(); ++i) {
        if (order[i].first.a == order[i - 1].first.a && order[i].first.b == order[i - 1].first.b) {
            return fail(GraphIssue::DuplicateEdge, order[i].second);
        }
    }

    ProvinceGraph g;
    g.nodes_ = std::move(nodes);
    g.edges_.reserve(order.size());
    for (const auto& [edge, index] : order) {
        g.edges_.push_back(edge);
    }

    // Adjacency (CSR): count, prefix sums, fill, sort each list by neighbour.
    const std::size_t n = g.nodes_.size();
    g.first_.assign(n + 1, 0);
    for (const GraphEdge& e : g.edges_) {
        ++g.first_[e.a + 1U];
        ++g.first_[e.b + 1U];
    }
    for (std::size_t i = 0; i < n; ++i) {
        g.first_[i + 1] += g.first_[i];
    }
    g.neighbors_.resize(g.first_[n]);
    std::vector<std::uint32_t> fill(g.first_.begin(), g.first_.end() - 1);
    for (std::size_t i = 0; i < g.edges_.size(); ++i) {
        const GraphEdge& e = g.edges_[i];
        const auto edge = static_cast<std::uint32_t>(i);
        g.neighbors_[fill[e.a]++] = Neighbor{e.b, e.length_km, e.road, edge};
        g.neighbors_[fill[e.b]++] = Neighbor{e.a, e.length_km, e.road, edge};
    }
    for (std::size_t i = 0; i < n; ++i) {
        std::sort(g.neighbors_.begin() + g.first_[i],
                  g.neighbors_.begin() + g.first_[i + 1],
                  [](const Neighbor& l, const Neighbor& r) { return l.node < r.node; });
    }

    // Components by an explicit stack, numbered in node order.
    constexpr auto kUnset = std::numeric_limits<std::uint16_t>::max();
    g.component_.assign(n, kUnset);
    std::vector<NodeIndex> stack;
    std::uint16_t next = 0;
    for (std::size_t start = 0; start < n; ++start) {
        if (g.component_[start] != kUnset) {
            continue;
        }
        g.component_[start] = next;
        stack.push_back(static_cast<NodeIndex>(start));
        while (!stack.empty()) {
            const NodeIndex at = stack.back();
            stack.pop_back();
            for (const Neighbor& nb : g.neighbors(at)) {
                if (g.component_[nb.node] == kUnset) {
                    g.component_[nb.node] = next;
                    stack.push_back(nb.node);
                }
            }
        }
        ++next;
    }
    g.component_count_ = next;

    core::HashStream h;
    h.u32(static_cast<std::uint32_t>(n));
    for (const GraphNode& node : g.nodes_) {
        h.i32(node.x_km);
        h.i32(node.y_km);
    }
    h.u32(static_cast<std::uint32_t>(g.edges_.size()));
    for (const GraphEdge& e : g.edges_) {
        h.u16(e.a);
        h.u16(e.b);
        h.i32(e.length_km);
        h.u8(static_cast<std::uint8_t>(e.road));
    }
    g.hash_ = h.value();
    return core::Result<ProvinceGraph, GraphError>::ok(std::move(g));
}

std::span<const Neighbor> ProvinceGraph::neighbors(NodeIndex index) const noexcept {
    if (index >= nodes_.size()) {
        return {};
    }
    return std::span<const Neighbor>(neighbors_).subspan(first_[index], first_[index + 1U] - first_[index]);
}

const GraphEdge* ProvinceGraph::find_edge(NodeIndex a, NodeIndex b) const noexcept {
    for (const Neighbor& nb : neighbors(a)) {
        if (nb.node == b) {
            return &edges_[nb.edge];
        }
    }
    return nullptr;
}

NodeIndex ProvinceGraph::find_node(core::StringId definition) const noexcept {
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        if (nodes_[i].definition == definition) {
            return static_cast<NodeIndex>(i);
        }
    }
    return kNoNode;
}

std::int64_t ProvinceGraph::straight_km(NodeIndex a, NodeIndex b) const {
    return integer_distance(std::int64_t{nodes_[a].x_km} - nodes_[b].x_km,
                            std::int64_t{nodes_[a].y_km} - nodes_[b].y_km);
}

EdgeTime edge_days(std::int32_t length_km,
                   std::int32_t speed_kmd,
                   std::int32_t terrain_move_pm,
                   std::int32_t season_move_pm,
                   std::int32_t road_move_pm) noexcept {
    EdgeTime t;
    const auto step = [](std::int64_t v, std::int32_t pm) { return v * std::max(pm, 0) / 1000; };
    t.v_base = std::int64_t{std::max(speed_kmd, 0)} * 1000;
    t.v_terrain = step(t.v_base, terrain_move_pm);
    t.v_season = step(t.v_terrain, season_move_pm);
    t.v_road = std::max<std::int64_t>(1, step(t.v_season, road_move_pm));
    const std::int64_t distance = std::int64_t{std::max(length_km, 0)} * 1000;
    const std::int64_t days = (distance + t.v_road - 1) / t.v_road;
    t.days = static_cast<std::int32_t>(
        std::clamp<std::int64_t>(days, 1, std::numeric_limits<std::int32_t>::max()));
    return t;
}

}  // namespace pat::sim::army
