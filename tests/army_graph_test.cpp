// Province graph and path finding (E16-T04, docs/dev/army-model.md sections 5 and 9.1).

#include "pat/content/graph_content.hpp"
#include "pat/content/load_errors.hpp"
#include "pat/content/m1_scenario.hpp"
#include "pat/data/content_schemas.hpp"
#include "pat/data/graph_schemas.hpp"
#include "pat/mapdata/graph_export.hpp"
#include "pat/mapdata/map_gen.hpp"
#include "pat/sim/army/path_finder.hpp"
#include "pat/sim/army/province_graph.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <regex>
#include <string>
#include <vector>

#include "court_fixture.hpp"

using namespace pat;
using sim::army::GraphEdge;
using sim::army::GraphIssue;
using sim::army::GraphNode;
using sim::army::NodeIndex;
using sim::army::PathFailure;
using sim::army::ProvinceGraph;
using sim::army::Road;

namespace {

ProvinceGraph build_ok(std::vector<GraphNode> nodes, std::vector<GraphEdge> edges) {
    auto built = ProvinceGraph::build(std::move(nodes), std::move(edges));
    REQUIRE(built.has_value());
    return std::move(built).value();
}

/// A w x h grid, `step` km apart, 4-neighbour edges of exactly `step` km.
ProvinceGraph grid(int w, int h, std::int32_t step = 50) {
    std::vector<GraphNode> nodes;
    std::vector<GraphEdge> edges;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            nodes.push_back({{}, x * step, y * step});
            const auto at = static_cast<NodeIndex>(y * w + x);
            if (x + 1 < w) {
                edges.push_back({at, static_cast<NodeIndex>(at + 1), step, Road::None});
            }
            if (y + 1 < h) {
                edges.push_back({at, static_cast<NodeIndex>(at + w), step, Road::None});
            }
        }
    }
    return build_ok(std::move(nodes), std::move(edges));
}

sim::army::PathNetwork network(const ProvinceGraph& graph, sim::army::MoveCosts costs = {}) {
    auto built = sim::army::PathNetwork::build(graph, costs);
    REQUIRE(built.has_value());
    return std::move(built).value();
}

/// Reference: plain O(N^2) Dijkstra over the same integer costs.
std::int64_t dijkstra(const sim::army::PathNetwork& net,
                      NodeIndex from,
                      NodeIndex to,
                      std::span<const std::uint8_t> allowed = {}) {
    const auto& g = net.graph();
    const std::size_t n = g.node_count();
    constexpr auto kInf = std::numeric_limits<std::int64_t>::max();
    std::vector<std::int64_t> dist(n, kInf);
    std::vector<bool> done(n, false);
    dist[from] = 0;
    for (std::size_t round = 0; round < n; ++round) {
        std::size_t best = n;
        for (std::size_t i = 0; i < n; ++i) {
            if (!done[i] && dist[i] != kInf && (best == n || dist[i] < dist[best])) {
                best = i;
            }
        }
        if (best == n) {
            break;
        }
        done[best] = true;
        const auto nbs = g.neighbors(static_cast<NodeIndex>(best));
        for (std::size_t s = 0; s < nbs.size(); ++s) {
            if (!allowed.empty() && allowed[nbs[s].node] == 0 && nbs[s].node != from) {
                continue;
            }
            dist[nbs[s].node] =
                std::min(dist[nbs[s].node], dist[best] + net.edge_cost(static_cast<NodeIndex>(best), s));
        }
    }
    return dist[to];
}

/// Every step of a path is an edge and the cost adds up.
void check_path(const sim::army::PathNetwork& net, const sim::army::Path& path) {
    std::int64_t cost = 0;
    for (std::size_t i = 1; i < path.nodes.size(); ++i) {
        const NodeIndex a = path.nodes[i - 1];
        const NodeIndex b = path.nodes[i];
        const auto nbs = net.graph().neighbors(a);
        const auto it = std::find_if(nbs.begin(), nbs.end(), [&](const auto& nb) { return nb.node == b; });
        REQUIRE(it != nbs.end());
        cost += net.edge_cost(a, static_cast<std::size_t>(it - nbs.begin()));
    }
    CHECK(cost == path.cost);
}

/// Terrain factors with variety (plain, steppe, forest, swamp, hills).
std::vector<std::int32_t> varied_terrain(std::size_t n) {
    static constexpr std::int32_t kMove[] = {1000, 1100, 700, 500, 800};
    std::vector<std::int32_t> out(n);
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = kMove[(i * 7 + i / 13) % 5];
    }
    return out;
}

}  // namespace

// ---- days on an edge (MIL-A1) -------------------------------------------------------------------------

TEST_CASE("army graph: edge days follow the MIL-A1 table") {
    using sim::army::edge_days;
    CHECK(edge_days(120, 20, 1000, 1000, 1000).days == 6);   // July
    CHECK(edge_days(120, 20, 1000, 550, 1000).days == 11);   // April, thaw
    CHECK(edge_days(120, 20, 1000, 550, 1250).days == 9);    // April, tract
    CHECK(edge_days(120, 20, 700, 550, 1000).days == 16);    // April, forest
    CHECK(edge_days(120, 20, 1000, 1100, 1000).days == 6);   // January, winter road
    CHECK(edge_days(120, 12, 1000, 1000, 1000).days == 10);  // artillery sets the pace
    CHECK(edge_days(120, 30, 1000, 1000, 1000).days == 4);   // cavalry
    const auto t = edge_days(120, 20, 700, 550, 1000);
    CHECK(t.v_base == 20'000);
    CHECK(t.v_terrain == 14'000);
    CHECK(t.v_season == 7'700);
    CHECK(t.v_road == 7'700);
    // Degenerate inputs still give at least one day and never divide by zero.
    CHECK(edge_days(0, 20, 1000, 1000, 1000).days == 1);
    CHECK(edge_days(120, 0, 1000, 1000, 1000).days >= 1);
}

// ---- graph ---------------------------------------------------------------------------------------------

TEST_CASE("army graph: build rejects broken graphs with a reason") {
    const std::vector<GraphNode> two{{{}, 0, 0}, {{}, 30, 40}};  // 50 km apart
    const auto issue = [&](std::vector<GraphNode> nodes, std::vector<GraphEdge> edges) {
        auto built = ProvinceGraph::build(std::move(nodes), std::move(edges));
        REQUIRE_FALSE(built.has_value());
        return built.error();
    };
    CHECK(issue({}, {}).issue == GraphIssue::Empty);
    CHECK(issue(two, {{0, 2, 60, Road::None}}).issue == GraphIssue::BadNode);
    CHECK(issue(two, {{1, 1, 60, Road::None}}).issue == GraphIssue::SelfLoop);
    CHECK(issue(two, {{0, 1, 9, Road::None}}).issue == GraphIssue::BadLength);
    CHECK(issue(two, {{0, 1, 49, Road::None}}).issue == GraphIssue::ShorterThanLine);
    CHECK(issue(two, {{0, 1, 60, static_cast<Road>(7)}}).issue == GraphIssue::BadRoad);
    const auto duplicate = issue(two, {{0, 1, 60, Road::None}, {1, 0, 70, Road::Tract}});
    CHECK(duplicate.issue == GraphIssue::DuplicateEdge);
    CHECK(duplicate.index == 1);
    CHECK(ProvinceGraph::build(two, {{0, 1, 50, Road::None}}).has_value());  // exactly the line is fine
}

TEST_CASE("army graph: adjacency is symmetric, sorted and independent of the input order") {
    const ProvinceGraph g = grid(5, 4);
    for (std::size_t i = 0; i < g.node_count(); ++i) {
        const auto nbs = g.neighbors(static_cast<NodeIndex>(i));
        CHECK(std::is_sorted(
            nbs.begin(), nbs.end(), [](const auto& l, const auto& r) { return l.node < r.node; }));
        for (const auto& nb : nbs) {
            const GraphEdge* back = g.find_edge(nb.node, static_cast<NodeIndex>(i));
            REQUIRE(back != nullptr);
            CHECK(back->length_km == nb.length_km);
        }
    }
    CHECK(g.connected());
    // Reversed and reoriented edges give the same graph and hash.
    std::vector<GraphEdge> edges(g.edges().begin(), g.edges().end());
    std::reverse(edges.begin(), edges.end());
    for (GraphEdge& e : edges) {
        std::swap(e.a, e.b);
    }
    const ProvinceGraph again = build_ok(std::vector<GraphNode>(g.nodes().begin(), g.nodes().end()), edges);
    CHECK(again == g);
    CHECK(again.hash() == g.hash());
}

TEST_CASE("army graph: components and a disconnected goal") {
    // 0-1 and 2-3, two islands.
    const ProvinceGraph g = build_ok({{{}, 0, 0}, {{}, 20, 0}, {{}, 100, 0}, {{}, 120, 0}},
                                     {{0, 1, 20, Road::None}, {2, 3, 20, Road::None}});
    CHECK(g.component_count() == 2);
    CHECK_FALSE(g.connected());
    const auto net = network(g);
    const auto result = sim::army::find_path(net, 0, 3);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == sim::army::PathError{PathFailure::Disconnected, 3});
    CHECK(sim::army::find_path(net, 0, 9).error().reason == PathFailure::BadNode);
}

// ---- A* ------------------------------------------------------------------------------------------------

TEST_CASE("army path: equal costs are resolved by node index, the same every time") {
    // A diamond 0 -> {1, 2} -> 3 with equal lengths: both routes cost the same.
    const ProvinceGraph g = build_ok(
        {{{}, 0, 50}, {{}, 50, 0}, {{}, 50, 100}, {{}, 100, 50}},
        {{2, 3, 80, Road::None}, {0, 2, 80, Road::None}, {1, 3, 80, Road::None}, {0, 1, 80, Road::None}});
    const auto net = network(g);
    const auto first = sim::army::find_path(net, 0, 3);
    REQUIRE(first.has_value());
    CHECK(first.value().nodes == std::vector<NodeIndex>{0, 1, 3});
    for (int i = 0; i < 5; ++i) {
        CHECK(sim::army::find_path(net, 0, 3).value() == first.value());
    }
    // A tract on the other side makes it cheaper and wins.
    const ProvinceGraph roads = build_ok(
        {{{}, 0, 50}, {{}, 50, 0}, {{}, 50, 100}, {{}, 100, 50}},
        {{0, 1, 80, Road::None}, {1, 3, 80, Road::None}, {0, 2, 80, Road::Tract}, {2, 3, 80, Road::Tract}});
    const auto road_net = network(roads);
    CHECK(sim::army::find_path(road_net, 0, 3).value().nodes == std::vector<NodeIndex>{0, 2, 3});
    CHECK(sim::army::find_path(net, 2, 2).value() == sim::army::Path{{2}, 0});
}

TEST_CASE("army path: a 400-province grid gives the cheapest path") {
    const ProvinceGraph g = grid(20, 20);
    REQUIRE(g.node_count() == 400);
    sim::army::MoveCosts costs;
    costs.enter_move_pm = varied_terrain(g.node_count());
    const auto net = network(g, costs);
    for (std::size_t k = 0; k < 40; ++k) {
        const auto from = static_cast<NodeIndex>((k * 37) % 400);
        const auto to = static_cast<NodeIndex>((k * 151 + 199) % 400);
        const auto path = sim::army::find_path(net, from, to);
        REQUIRE(path.has_value());
        CHECK(path.value().nodes.front() == from);
        CHECK(path.value().nodes.back() == to);
        check_path(net, path.value());
        CHECK(path.value().cost == dijkstra(net, from, to));
    }
    // Corner to corner of a uniform grid: 38 steps.
    const auto uniform = network(g);
    CHECK(sim::army::find_path(uniform, 0, 399).value().steps() == 38);
}

TEST_CASE("army path: passage closes nodes; the goal itself and a full wall are reported") {
    const ProvinceGraph g = grid(5, 5);
    const auto net = network(g);
    std::vector<std::uint8_t> allowed(25, 1);
    for (int y = 0; y < 5; ++y) {
        allowed[static_cast<std::size_t>(y * 5 + 2)] = 0;  // a wall in column 2...
    }
    allowed[4 * 5 + 2] = 1;  // ...with a gate at the bottom
    const auto through_gate = sim::army::find_path(net, 0, 4, {allowed, 1});
    REQUIRE(through_gate.has_value());
    CHECK(std::find(through_gate.value().nodes.begin(), through_gate.value().nodes.end(), NodeIndex{22}) !=
          through_gate.value().nodes.end());
    CHECK(through_gate.value().cost == dijkstra(net, 0, 4, allowed));

    allowed[4 * 5 + 2] = 0;  // close the gate
    const auto walled = sim::army::find_path(net, 0, 4, {allowed, 1});
    REQUIRE_FALSE(walled.has_value());
    CHECK(walled.error() == sim::army::PathError{PathFailure::NoPath, 4});
    const auto closed_goal = sim::army::find_path(net, 0, 2, {allowed, 1});
    CHECK(closed_goal.error().reason == PathFailure::TargetNotAllowed);
    // The start is always allowed: an army inside the wall may leave it.
    CHECK(sim::army::find_path(net, 12, 11, {allowed, 1}).has_value());
}

TEST_CASE("army path: network costs validate their inputs") {
    const ProvinceGraph g = grid(2, 2);
    sim::army::MoveCosts wrong_size;
    wrong_size.enter_move_pm = {1000};
    CHECK_FALSE(sim::army::PathNetwork::build(g, wrong_size).has_value());
    sim::army::MoveCosts zero;
    zero.enter_move_pm = {1000, 0, 1000, 1000};
    CHECK_FALSE(sim::army::PathNetwork::build(g, zero).has_value());
    sim::army::MoveCosts road;
    road.road_move_pm = {1000, 0};
    CHECK_FALSE(sim::army::PathNetwork::build(g, road).has_value());
    // per_km = ceil(1e6 / s): plain 1000, forest 1429, steppe tract 728.
    const auto net = network(g);
    CHECK(net.per_km_cost(1, Road::None) == 1000);
    CHECK(net.per_km_cost(1, Road::Tract) == 800);
}

// ---- cache ---------------------------------------------------------------------------------------------

TEST_CASE("army path cache: hits equal fresh searches, classes and versions invalidate") {
    const ProvinceGraph g = grid(6, 6);
    const auto net = network(g);
    sim::army::PathCache cache(4);
    const auto fresh = sim::army::find_path(net, 0, 35);
    CHECK(cache.find(net, 0, 35).value() == fresh.value());
    CHECK(cache.misses() == 1);
    CHECK(cache.find(net, 0, 35).value() == fresh.value());
    CHECK(cache.hits() == 1);

    // Another class is another key; invalidate_class drops only its entries.
    std::vector<std::uint8_t> allowed(36, 1);
    allowed[1] = 0;
    CHECK(cache.find(net, 0, 35, {allowed, 7}).has_value());
    CHECK(cache.size() == 2);
    cache.invalidate_class(7);
    CHECK(cache.size() == 1);

    // Failures are cached too.
    allowed[35] = 0;
    CHECK(cache.find(net, 0, 35, {allowed, 8}).error().reason == PathFailure::TargetNotAllowed);
    CHECK(cache.find(net, 0, 35, {allowed, 8}).error().reason == PathFailure::TargetNotAllowed);

    // Capacity 4: the least recently used entry leaves.
    CHECK(cache.find(net, 1, 2).has_value());
    CHECK(cache.find(net, 1, 3).has_value());
    CHECK(cache.size() == 4);
    (void)cache.find(net, 0, 35);  // touch the oldest one
    CHECK(cache.find(net, 1, 4).has_value());
    CHECK(cache.size() == 4);
    const auto hits = cache.hits();
    (void)cache.find(net, 0, 35);
    CHECK(cache.hits() == hits + 1);  // kept

    // Another network version clears everything.
    sim::army::MoveCosts slow;
    slow.enter_move_pm.assign(36, 500);
    const auto other = network(g, slow);
    CHECK(cache.find(other, 0, 35).value().cost == 2 * fresh.value().cost);
    CHECK(cache.size() == 1);
}

// ---- export from the map and the shipped content ---------------------------------------------------

TEST_CASE("army graph: the M1 graph.pd equals the export of the M1 map") {
    const mapdata::MapData map = mapdata::generate_map(content::m1_map_params());
    const mapdata::GraphExport exported = mapdata::export_province_graph(map, content::kM1GraphMetresPerPx);
    REQUIRE(exported.nodes.size() == 20);

    const data::DirectorySource source(PATRIARCH_DATA_DIR);
    const auto loaded = content::load_province_graph_content(source, content::kM1GraphId);
    if (!loaded) {
        INFO(content::format_errors(loaded.error()));
    }
    REQUIRE(loaded.has_value());
    const auto& graph = loaded.value().graph;
    CHECK(graph.node_count() == 20);
    CHECK(graph.connected());
    CHECK(content::graph_topology(loaded.value()) == exported);

    // Byte for byte, apart from hand-edited roads.
    const std::string shipped = court_fixture::shipped_text("scenarios/m1/graph.pd");
    const std::string without_roads = std::regex_replace(shipped, std::regex(" road = [a-z]+"), "");
    CHECK(without_roads ==
          mapdata::format_graph_pd(exported, content::kM1GraphId, content::kM1GraphMetresPerPx));

    // A path across M1 between the two farthest-apart provinces.
    const auto net = network(graph);
    NodeIndex far_a = 0;
    NodeIndex far_b = 0;
    for (NodeIndex a = 0; a < 20; ++a) {
        for (NodeIndex b = 0; b < 20; ++b) {
            if (graph.straight_km(a, b) > graph.straight_km(far_a, far_b)) {
                far_a = a;
                far_b = b;
            }
        }
    }
    const auto path = sim::army::find_path(net, far_a, far_b);
    REQUIRE(path.has_value());
    CHECK(path.value().steps() >= 2);
    check_path(net, path.value());
    CHECK(path.value().cost == dijkstra(net, far_a, far_b));
    CHECK(loaded.value().keys.front() == "p0001");
}

TEST_CASE("army graph: the graph kind does not move the M1 compatibility hash") {
    auto source = court_fixture::shipped_source(false);
    source.add("provinces/m1.pd", court_fixture::shipped_text("provinces/m1.pd"));
    const auto schemas = data::content_schemas();
    const data::ContentSource* without_roots[] = {&source};
    const auto without = data::load_content(without_roots, schemas);
    REQUIRE(without.ok());
    source.add("scenarios/m1/graph.pd", court_fixture::shipped_text("scenarios/m1/graph.pd"));
    const data::ContentSource* with_roots[] = {&source};
    const auto with = data::load_content(with_roots, schemas);
    REQUIRE(with.ok());
    CHECK(with.registry.content_hash() != without.registry.content_hash());
    CHECK(with.registry.content_hash_excluding(data::kGraphKinds) == without.registry.content_hash());
    // Interned last: the province definition ids do not shift.
    CHECK(with.registry.find("province", "p0007")->id == without.registry.find("province", "p0007")->id);
}

TEST_CASE("army graph: content errors point at the node or edge") {
    const auto load = [](const std::string& graph) {
        auto source = court_fixture::shipped_source(false);
        source.add("provinces/m1.pd", court_fixture::shipped_text("provinces/m1.pd"));
        source.add("scenarios/m1/graph.pd", "format_version = 1\nprovince_graph \"m1\" {\n" + graph + "}\n");
        return content::load_province_graph_content(source, "m1");
    };
    const auto message = [&](const std::string& graph) {
        const auto result = load(graph);
        REQUIRE_FALSE(result.has_value());
        return content::format_errors(result.error());
    };
    const std::string two = "node \"p0001\" { x_km = 0 y_km = 0 }\nnode \"p0002\" { x_km = 30 y_km = 40 }\n";
    const std::string edge = "edge { from = province:p0001 to = province:p0002 length_km = 50 }\n";
    CHECK(load(two + edge).has_value());
    CHECK(message("node \"p9999\" { x_km = 0 y_km = 0 }\n").find("not a province definition") !=
          std::string::npos);
    CHECK(message(two + "edge { from = province:p0001 to = province:p0003 length_km = 50 }\n")
              .find("\"p0003\" is not a node") != std::string::npos);
    CHECK(message(two + edge + "edge { from = province:p0002 to = province:p0001 length_km = 60 }\n")
              .find("duplicate edge") != std::string::npos);
    CHECK(message(two + "edge { from = province:p0001 to = province:p0002 length_km = 40 }\n")
              .find("shorter than the straight line") != std::string::npos);
    const std::string disconnected = message(two + "node \"p0003\" { x_km = 500 y_km = 0 }\n" + edge);
    CHECK(disconnected.find("not connected") != std::string::npos);
    CHECK(disconnected.find("\"p0003\"") != std::string::npos);
    CHECK(disconnected.find("PD115") != std::string::npos);
    CHECK(message(two).find("not connected") != std::string::npos);
    const auto missing = load(two + edge);
    REQUIRE(missing.has_value());
    CHECK(load(two + "edge { from = province:p0001 to = province:p0002 length_km = 50 road = tract }\n")
              .value()
              .graph.edges()[0]
              .road == Road::Tract);
}

TEST_CASE("army graph: PMD1 province keys") {
    CHECK(mapdata::pmd1_province_key(7) == "p0007");
    CHECK(mapdata::pmd1_province_key(400) == "p0400");
    CHECK(mapdata::pmd1_province_key(12345) == "p12345");
}

// ---- benchmark (skipped by default: run with --no-skip -tc="army path: bench*") -------------------------

TEST_CASE("army path: bench: 400 provinces, cold and cached" * doctest::skip()) {
    const mapdata::MapData map = mapdata::generate_map(
        {.width = 2048, .height = 1024, .seed = 3, .province_count = 400, .threads = 0});
    const mapdata::GraphExport exported = mapdata::export_province_graph(map, 2000);
    std::vector<GraphNode> nodes;
    for (const auto& n : exported.nodes) {
        nodes.push_back({{}, n.x_km, n.y_km});
    }
    std::vector<GraphEdge> edges;
    for (const auto& e : exported.edges) {
        edges.push_back({e.a, e.b, e.length_km, Road::None});
    }
    const ProvinceGraph g = build_ok(std::move(nodes), std::move(edges));
    sim::army::MoveCosts costs;
    costs.enter_move_pm = varied_terrain(g.node_count());
    const auto net = network(g, costs);
    const auto n = static_cast<NodeIndex>(g.node_count());
    constexpr int kQueries = 2000;
    std::int64_t checksum = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (int k = 0; k < kQueries; ++k) {
        const auto r = sim::army::find_path(
            net, static_cast<NodeIndex>((k * 37) % n), static_cast<NodeIndex>((k * 151 + 199) % n));
        checksum += r.has_value() ? r.value().cost : 0;
    }
    const auto t1 = std::chrono::steady_clock::now();
    sim::army::PathCache cache;
    for (int k = 0; k < kQueries; ++k) {
        const auto r =
            cache.find(net, static_cast<NodeIndex>(k % 16), static_cast<NodeIndex>((k * 7) % 16 + 300));
        checksum += r.has_value() ? r.value().cost : 0;
    }
    const auto t2 = std::chrono::steady_clock::now();
    const auto us = [](auto d) { return std::chrono::duration<double, std::micro>(d).count(); };
    std::printf("army path bench: %zu nodes, %zu edges; cold %.2f us/path, cached %.3f us/query (%llu hits), "
                "checksum %lld\n",
                g.node_count(),
                g.edges().size(),
                us(t1 - t0) / kQueries,
                us(t2 - t1) / kQueries,
                static_cast<unsigned long long>(cache.hits()),
                static_cast<long long>(checksum));
    CHECK(checksum > 0);
}
