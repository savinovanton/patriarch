#pragma once

#include "pat/core/result.hpp"
#include "pat/sim/army/province_graph.hpp"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string_view>
#include <vector>

// Path finding on the province graph (E16-T04, docs/dev/army-model.md section 5).
//
// The cost of an edge is a whole number of "equivalent metres": length_km times the cost of one km into
// the destination, per_km = ceil(1'000'000 / s) with s = enter_move_pm(dest) * road_move_pm(road) / 1000
// (no season: a path does not depend on the date, the season is applied per edge when the army leaves,
// section 9.1). The heuristic is h = straight_km(node, goal) * min_per_km, where min_per_km is the
// cheapest km of the network; every edge is at least as long as the straight line between its nodes
// (ProvinceGraph::build checks it), so h is admissible and consistent and A* returns a cheapest path.
//
// Determinism: integer costs only; the open list is a binary heap keyed (f, g, node); neighbours are
// visited in node order and a node's parent changes only on a strictly smaller g. Two equally cheap
// paths are therefore resolved by the node indices (the province keys), never by addresses or timing.
//
// Passage (section 9.8, passage_allowed): a per-node "allowed" mask that the caller derives from the
// army's owner; the start node is always allowed. The cache is derived data and is never saved: a
// path found through the cache equals the path found without it.

namespace pat::sim::army {

/// Per-mille movement factors of the network (data/balance/army.pd of E16-T05 supplies the values).
struct MoveCosts {
    /// Factor of entering each node (terrain move of the destination), one per graph node; empty means
    /// 1000 everywhere. Values must be > 0.
    std::vector<std::int32_t> enter_move_pm;
    /// Factor of each road kind, indexed by Road; values must be > 0.
    std::array<std::int32_t, kRoadCount> road_move_pm{1000, 1250};
};

/// The graph with the integer cost of every directed edge. Built once per graph and cost table;
/// version() changes with either of them and keys the path cache.
class PathNetwork {
public:
    /// Fails (core::Error) when enter_move_pm has a wrong size or a factor is not positive.
    [[nodiscard]] static core::Result<PathNetwork> build(const ProvinceGraph& graph, const MoveCosts& costs);

    [[nodiscard]] const ProvinceGraph& graph() const noexcept { return *graph_; }
    /// Cost of the directed edge to neighbour slot `slot` of `from` (aligned with graph().neighbors()).
    [[nodiscard]] std::int64_t edge_cost(NodeIndex from, std::size_t slot) const;
    /// Cost of one km into `to` along `road`.
    [[nodiscard]] std::int64_t per_km_cost(NodeIndex to, Road road) const;
    [[nodiscard]] std::int64_t min_per_km_cost() const noexcept { return min_per_km_; }
    [[nodiscard]] std::uint64_t version() const noexcept { return version_; }

private:
    const ProvinceGraph* graph_ = nullptr;  ///< Must outlive the network.
    std::vector<std::int64_t> cost_;        ///< Aligned with the CSR adjacency of the graph.
    std::vector<std::uint32_t> first_;
    std::vector<std::int32_t> enter_move_pm_;
    std::array<std::int32_t, kRoadCount> road_move_pm_{};
    std::int64_t min_per_km_ = 1;
    std::uint64_t version_ = 0;
};

/// Which nodes an army may enter: `allowed[i] != 0`; an empty span allows every node. `passage_class`
/// names the mask for the cache (for example the owner); the same class must always mean the same
/// mask until PathCache::invalidate_class().
struct Passage {
    std::span<const std::uint8_t> allowed;
    std::uint64_t passage_class = 0;
};

enum class PathFailure : std::uint8_t {
    BadNode = 1,           ///< `from` or `to` is not a node of the graph.
    TargetNotAllowed = 2,  ///< The goal itself is closed by the passage mask (ForeignTerritory).
    Disconnected = 3,      ///< The goal is in another connected component of the graph.
    NoPath = 4,            ///< Every route to the goal crosses a closed node (NoPath, code 25).
};

struct PathError {
    PathFailure reason = PathFailure::NoPath;
    NodeIndex node = kNoNode;  ///< The node the reason is about (the bad node or the goal).
    [[nodiscard]] bool operator==(const PathError&) const = default;
};

[[nodiscard]] std::string_view path_failure_name(PathFailure reason) noexcept;

struct Path {
    std::vector<NodeIndex> nodes;  ///< from ... to inclusive; one node when from == to.
    std::int64_t cost = 0;         ///< Sum of PathNetwork::edge_cost along the path.
    [[nodiscard]] std::size_t steps() const noexcept { return nodes.empty() ? 0 : nodes.size() - 1; }
    [[nodiscard]] bool operator==(const Path&) const = default;
};

using PathResult = core::Result<Path, PathError>;

/// A* from `from` to `to` (the cheapest path; ties resolved by node index).
[[nodiscard]] PathResult
find_path(const PathNetwork& network, NodeIndex from, NodeIndex to, const Passage& passage = {});

inline constexpr std::size_t kPathCacheCapacity = 256;

/// Cache of results keyed (from, to, passage class) for one network version. Evicts the least recently
/// used entry (ties by key). Changing the network (another version) clears it; a change of passability
/// for an owner calls invalidate_class(), any wider change clear().
class PathCache {
public:
    explicit PathCache(std::size_t capacity = kPathCacheCapacity) : capacity_(capacity == 0 ? 1 : capacity) {}

    [[nodiscard]] PathResult
    find(const PathNetwork& network, NodeIndex from, NodeIndex to, const Passage& passage = {});
    void invalidate_class(std::uint64_t passage_class);
    void clear() noexcept;

    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    [[nodiscard]] std::uint64_t hits() const noexcept { return hits_; }
    [[nodiscard]] std::uint64_t misses() const noexcept { return misses_; }

private:
    struct Key {
        std::uint64_t passage_class = 0;
        NodeIndex from = kNoNode;
        NodeIndex to = kNoNode;
        [[nodiscard]] auto operator<=>(const Key&) const = default;
    };
    struct Entry {
        PathResult result = PathResult::err(PathError{});
        std::uint64_t last_use = 0;
    };

    std::size_t capacity_;
    std::map<Key, Entry> entries_;
    std::uint64_t version_ = 0;
    std::uint64_t clock_ = 0;
    std::uint64_t hits_ = 0;
    std::uint64_t misses_ = 0;
};

}  // namespace pat::sim::army
