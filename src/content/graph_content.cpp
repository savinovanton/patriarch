#include "pat/content/graph_content.hpp"

#include "pat/content/load_errors.hpp"
#include "pat/data/content_schemas.hpp"
#include "pat/data/graph_schemas.hpp"

#include <algorithm>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <utility>

#include "typed_accessors.hpp"

namespace pat::content {

namespace {

using sim::army::GraphEdge;
using sim::army::GraphNode;
using sim::army::NodeIndex;
using sim::army::ProvinceGraph;

struct NodeEntry {
    std::string key;
    data::SourcePos pos;
    GraphNode node;
};

/// Builds one graph object; problems go to `errors`, nullopt when there was any.
std::optional<ProvinceGraphContent>
build_one(const data::ContentRegistry& registry, const data::ContentObject& object, ErrorSink& errors) {
    const data::TypedBlock& body = object.object.body;
    bool ok = true;

    std::vector<NodeEntry> nodes;
    for (const data::TypedField& field : body.fields) {
        if (field.name != "node") {
            continue;
        }
        const auto* block = field.value.get_if<data::TypedBlock>();
        if (block == nullptr) {
            continue;
        }
        if (registry.find("province", field.label) == nullptr) {
            errors.fail(
                object, field.pos, std::format("node \"{}\" is not a province definition", field.label));
            ok = false;
            continue;
        }
        nodes.push_back({field.label,
                         field.pos,
                         GraphNode{registry.find("province", field.label)->id,
                                   detail::i32_of(*block, "x_km"),
                                   detail::i32_of(*block, "y_km")}});
    }
    if (nodes.empty()) {
        errors.fail(object, object.object.pos, "the graph has no nodes");
        return std::nullopt;
    }
    std::sort(
        nodes.begin(), nodes.end(), [](const NodeEntry& l, const NodeEntry& r) { return l.key < r.key; });
    std::map<std::string, NodeIndex, std::less<>> index;
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        index.emplace(nodes[i].key, static_cast<NodeIndex>(i));
    }

    std::vector<GraphEdge> edges;
    std::vector<data::SourcePos> edge_pos;
    for (const data::TypedField& field : body.fields) {
        if (field.name != "edge") {
            continue;
        }
        const auto* block = field.value.get_if<data::TypedBlock>();
        if (block == nullptr) {
            continue;
        }
        const auto end = [&](std::string_view name) -> NodeIndex {
            const auto* ref = detail::value_of<data::RefValue>(*block, name);
            if (ref == nullptr) {
                return sim::army::kNoNode;
            }
            const auto it = index.find(ref->id);
            if (it == index.end()) {
                errors.fail(object,
                            detail::key_pos(*block, name, field.pos),
                            std::format("edge end \"{}\" is not a node of the graph", ref->id));
                ok = false;
                return sim::army::kNoNode;
            }
            return it->second;
        };
        const NodeIndex a = end("from");
        const NodeIndex b = end("to");
        const auto* road = detail::value_of<data::EnumValue>(*block, "road");
        edges.push_back(GraphEdge{a,
                                  b,
                                  detail::i32_of(*block, "length_km"),
                                  static_cast<sim::army::Road>(road != nullptr ? road->index : 0)});
        edge_pos.push_back(field.pos);
    }
    if (!ok) {
        return std::nullopt;
    }

    std::vector<std::string> keys;
    std::vector<GraphNode> graph_nodes;
    keys.reserve(nodes.size());
    graph_nodes.reserve(nodes.size());
    for (NodeEntry& entry : nodes) {
        graph_nodes.push_back(entry.node);
    }
    auto built = ProvinceGraph::build(std::move(graph_nodes), std::move(edges));
    if (!built) {
        const sim::army::GraphError error = built.error();
        const bool about_node = error.issue == sim::army::GraphIssue::BadCoordinate;
        const data::SourcePos pos = about_node                      ? nodes[error.index].pos
                                    : error.index < edge_pos.size() ? edge_pos[error.index]
                                                                    : object.object.pos;
        errors.fail(object, pos, std::string(sim::army::graph_issue_name(error.issue)));
        return std::nullopt;
    }
    ProvinceGraph graph = std::move(built).value();
    if (!graph.connected()) {
        // Name the first node outside the component of the first node.
        std::string stray;
        for (std::size_t i = 0; i < graph.node_count(); ++i) {
            if (graph.component(static_cast<NodeIndex>(i)) != 0) {
                stray = nodes[i].key;
                break;
            }
        }
        errors.fail(
            object,
            object.object.pos,
            std::format("the graph is not connected: {} components, \"{}\" cannot be reached from \"{}\"",
                        graph.component_count(),
                        stray,
                        nodes.front().key));
        return std::nullopt;
    }
    for (NodeEntry& entry : nodes) {
        keys.push_back(std::move(entry.key));
    }
    return ProvinceGraphContent{std::move(graph), std::move(keys), 0};
}

}  // namespace

ProvinceGraphResult build_province_graph(const data::ContentRegistry& registry, std::string_view graph_id) {
    ErrorSink errors(registry);
    const data::ContentObject* object = registry.find(data::kProvinceGraphKind, graph_id);
    if (object == nullptr) {
        errors.fail_general(std::format("no {} \"{}\"", data::kProvinceGraphKind, graph_id));
        return ProvinceGraphResult::err(errors.take());
    }
    auto built = build_one(registry, *object, errors);
    if (!built) {
        return ProvinceGraphResult::err(errors.take());
    }
    built->content_hash = registry.content_hash();
    return ProvinceGraphResult::ok(std::move(*built));
}

std::vector<data::LoadError> validate_province_graphs(const data::ContentRegistry& registry) {
    ErrorSink errors(registry);
    if (const data::ContentTable* table = registry.table(data::kProvinceGraphKind); table != nullptr) {
        for (const data::ContentObject& object : table->objects()) {
            (void)build_one(registry, object, errors);
        }
    }
    return errors.take();
}

ProvinceGraphResult load_province_graph_content(std::span<const data::ContentSource* const> roots,
                                                std::string_view graph_id) {
    auto loaded = data::load_content(roots, data::content_schemas());
    if (!loaded.ok()) {
        return ProvinceGraphResult::err(std::move(loaded.errors));
    }
    return build_province_graph(loaded.registry, graph_id);
}

ProvinceGraphResult load_province_graph_content(const data::ContentSource& root, std::string_view graph_id) {
    const data::ContentSource* roots[] = {&root};
    return load_province_graph_content(roots, graph_id);
}

mapdata::GraphExport graph_topology(const ProvinceGraphContent& content) {
    mapdata::GraphExport out;
    const auto nodes = content.graph.nodes();
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        out.nodes.push_back({content.keys[i], nodes[i].x_km, nodes[i].y_km});
    }
    for (const GraphEdge& edge : content.graph.edges()) {
        out.edges.push_back({edge.a, edge.b, edge.length_km});
    }
    return out;
}

}  // namespace pat::content
