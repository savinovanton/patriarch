#include "pat/sim/army/path_finder.hpp"

#include "pat/core/hash_stream.hpp"

#include <algorithm>
#include <format>
#include <functional>
#include <limits>
#include <queue>
#include <tuple>

namespace pat::sim::army {

namespace {

constexpr std::int64_t kPerKmScale = 1'000'000;
constexpr std::int64_t kInfinity = std::numeric_limits<std::int64_t>::max();

/// ceil(kPerKmScale / s), s = enter * road / 1000 truncated, at least 1.
std::int64_t per_km(std::int32_t enter_move_pm, std::int32_t road_move_pm) {
    const std::int64_t s = std::max<std::int64_t>(1, std::int64_t{enter_move_pm} * road_move_pm / 1000);
    return (kPerKmScale + s - 1) / s;
}

}  // namespace

std::string_view path_failure_name(PathFailure reason) noexcept {
    switch (reason) {
        case PathFailure::BadNode:
            return "not a node of the graph";
        case PathFailure::TargetNotAllowed:
            return "the goal is closed to this army";
        case PathFailure::Disconnected:
            return "the goal is not connected to the start";
        case PathFailure::NoPath:
            return "every route to the goal is closed";
    }
    return "unknown path failure";
}

core::Result<PathNetwork> PathNetwork::build(const ProvinceGraph& graph, const MoveCosts& costs) {
    const std::size_t n = graph.node_count();
    if (!costs.enter_move_pm.empty() && costs.enter_move_pm.size() != n) {
        return core::Result<PathNetwork>::err(core::Error{
            1, std::format("enter_move_pm has {} entries for {} nodes", costs.enter_move_pm.size(), n)});
    }
    for (std::size_t i = 0; i < costs.enter_move_pm.size(); ++i) {
        if (costs.enter_move_pm[i] <= 0) {
            return core::Result<PathNetwork>::err(
                core::Error{2, std::format("enter_move_pm of node {} is not positive", i)});
        }
    }
    for (std::size_t r = 0; r < kRoadCount; ++r) {
        if (costs.road_move_pm[r] <= 0) {
            return core::Result<PathNetwork>::err(
                core::Error{2, std::format("road_move_pm of road {} is not positive", r)});
        }
    }

    PathNetwork net;
    net.graph_ = &graph;
    net.enter_move_pm_ =
        costs.enter_move_pm.empty() ? std::vector<std::int32_t>(n, 1000) : costs.enter_move_pm;
    net.road_move_pm_ = costs.road_move_pm;
    net.first_.reserve(n + 1);
    net.min_per_km_ = kInfinity;
    for (std::size_t i = 0; i < n; ++i) {
        net.first_.push_back(static_cast<std::uint32_t>(net.cost_.size()));
        for (const Neighbor& nb : graph.neighbors(static_cast<NodeIndex>(i))) {
            const std::int64_t km = net.per_km_cost(nb.node, nb.road);
            net.min_per_km_ = std::min(net.min_per_km_, km);
            net.cost_.push_back(km * nb.length_km);
        }
    }
    net.first_.push_back(static_cast<std::uint32_t>(net.cost_.size()));
    if (net.min_per_km_ == kInfinity) {
        net.min_per_km_ = 1;  // no edges: the heuristic is never used for a move
    }

    core::HashStream h;
    h.u64(graph.hash());
    for (const std::int32_t pm : net.enter_move_pm_) {
        h.i32(pm);
    }
    for (const std::int32_t pm : net.road_move_pm_) {
        h.i32(pm);
    }
    net.version_ = h.value();
    return core::Result<PathNetwork>::ok(std::move(net));
}

std::int64_t PathNetwork::edge_cost(NodeIndex from, std::size_t slot) const {
    return cost_[first_[from] + slot];
}

std::int64_t PathNetwork::per_km_cost(NodeIndex to, Road road) const {
    return per_km(enter_move_pm_[to], road_move_pm_[static_cast<std::size_t>(road)]);
}

PathResult find_path(const PathNetwork& network, NodeIndex from, NodeIndex to, const Passage& passage) {
    const ProvinceGraph& graph = network.graph();
    const std::size_t n = graph.node_count();
    if (from >= n) {
        return PathResult::err(PathError{PathFailure::BadNode, from});
    }
    if (to >= n) {
        return PathResult::err(PathError{PathFailure::BadNode, to});
    }
    const auto allowed = [&](NodeIndex node) {
        return node == from || passage.allowed.empty() ||
               (node < passage.allowed.size() && passage.allowed[node] != 0);
    };
    if (!allowed(to)) {
        return PathResult::err(PathError{PathFailure::TargetNotAllowed, to});
    }
    if (graph.component(from) != graph.component(to)) {
        return PathResult::err(PathError{PathFailure::Disconnected, to});
    }
    if (from == to) {
        return PathResult::ok(Path{{from}, 0});
    }

    const std::int64_t unit = network.min_per_km_cost();
    const auto h = [&](NodeIndex node) { return graph.straight_km(node, to) * unit; };

    std::vector<std::int64_t> g(n, kInfinity);
    std::vector<NodeIndex> parent(n, kNoNode);
    std::vector<std::uint8_t> closed(n, 0);
    using Item = std::tuple<std::int64_t, std::int64_t, NodeIndex>;  // (f, g, node)
    std::priority_queue<Item, std::vector<Item>, std::greater<>> open;
    g[from] = 0;
    open.emplace(h(from), 0, from);
    while (!open.empty()) {
        const auto [f, cost, at] = open.top();
        open.pop();
        if (closed[at] != 0 || cost != g[at]) {
            continue;  // a stale entry
        }
        if (at == to) {
            break;
        }
        closed[at] = 1;
        const auto neighbors = graph.neighbors(at);
        for (std::size_t slot = 0; slot < neighbors.size(); ++slot) {
            const NodeIndex next = neighbors[slot].node;
            if (closed[next] != 0 || !allowed(next)) {
                continue;
            }
            const std::int64_t candidate = cost + network.edge_cost(at, slot);
            if (candidate < g[next]) {
                g[next] = candidate;
                parent[next] = at;
                open.emplace(candidate + h(next), candidate, next);
            }
        }
    }
    if (g[to] == kInfinity) {
        return PathResult::err(PathError{PathFailure::NoPath, to});
    }
    Path path;
    path.cost = g[to];
    for (NodeIndex at = to; at != kNoNode; at = parent[at]) {
        path.nodes.push_back(at);
    }
    std::reverse(path.nodes.begin(), path.nodes.end());
    return PathResult::ok(std::move(path));
}

PathResult PathCache::find(const PathNetwork& network, NodeIndex from, NodeIndex to, const Passage& passage) {
    if (network.version() != version_) {
        entries_.clear();
        version_ = network.version();
    }
    ++clock_;
    const Key key{passage.passage_class, from, to};
    if (const auto it = entries_.find(key); it != entries_.end()) {
        ++hits_;
        it->second.last_use = clock_;
        return it->second.result;
    }
    ++misses_;
    PathResult result = find_path(network, from, to, passage);
    if (entries_.size() >= capacity_) {
        // Least recently used; last_use values are unique, so the key never decides in practice.
        auto victim = entries_.begin();
        for (auto it = entries_.begin(); it != entries_.end(); ++it) {
            if (it->second.last_use < victim->second.last_use) {
                victim = it;
            }
        }
        entries_.erase(victim);
    }
    entries_.insert_or_assign(key, Entry{result, clock_});
    return result;
}

void PathCache::invalidate_class(std::uint64_t passage_class) {
    for (auto it = entries_.begin(); it != entries_.end();) {
        it = it->first.passage_class == passage_class ? entries_.erase(it) : std::next(it);
    }
}

void PathCache::clear() noexcept {
    entries_.clear();
}

}  // namespace pat::sim::army
