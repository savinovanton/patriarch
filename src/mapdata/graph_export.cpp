#include "pat/mapdata/graph_export.hpp"

#include "pat/core/detail/wide_math.hpp"

#include <algorithm>
#include <format>

namespace pat::mapdata {

namespace {

/// Whole km of a pixel coordinate, rounded half up.
std::int32_t to_km(std::uint32_t pixel, std::uint32_t metres_per_px) {
    const std::uint64_t metres = static_cast<std::uint64_t>(pixel) * metres_per_px;
    return static_cast<std::int32_t>((metres + 500) / 1000);
}

std::int32_t ceil_distance(std::int64_t dx, std::int64_t dy) {
    const auto sum = static_cast<std::uint64_t>(dx * dx) + static_cast<std::uint64_t>(dy * dy);
    std::uint64_t root = core::detail::isqrt_u128(core::detail::U128{0, sum});
    if (root * root < sum) {
        ++root;
    }
    return static_cast<std::int32_t>(root);
}

}  // namespace

std::string pmd1_province_key(std::uint16_t id) {
    return std::format("p{:04}", id);
}

GraphExport export_province_graph(const MapData& map, std::uint32_t metres_per_px) {
    GraphExport out;
    out.nodes.reserve(map.province_list.size());
    for (const Province& p : map.province_list) {
        out.nodes.push_back({pmd1_province_key(p.id),
                             to_km(p.centroid_x, metres_per_px),
                             to_km(p.centroid_y, metres_per_px)});
    }
    for (std::size_t i = 0; i < map.province_list.size(); ++i) {
        for (const Border& border : map.province_list[i].neighbors) {
            if (border.neighbor == 0 || border.neighbor > map.province_list.size()) {
                continue;
            }
            const auto a = static_cast<std::uint16_t>(i);
            const auto b = static_cast<std::uint16_t>(border.neighbor - 1U);
            if (b <= a) {
                continue;  // each undirected edge once, from its lower end
            }
            const GraphExportNode& na = out.nodes[a];
            const GraphExportNode& nb = out.nodes[b];
            const std::int32_t length =
                std::max(kMinGraphEdgeKm,
                         ceil_distance(std::int64_t{na.x_km} - nb.x_km, std::int64_t{na.y_km} - nb.y_km));
            out.edges.push_back({a, b, length});
        }
    }
    std::sort(out.edges.begin(), out.edges.end(), [](const GraphExportEdge& l, const GraphExportEdge& r) {
        return l.a != r.a ? l.a < r.a : l.b < r.b;
    });
    return out;
}

std::string
format_graph_pd(const GraphExport& graph, std::string_view graph_id, std::uint32_t metres_per_px) {
    std::string text;
    text += "format_version = 1\n\n";
    text += "# Province graph for the simulation (E16-T04, docs/dev/army-model.md section 5).\n";
    text += "# Generated from the map; edit by hand only `road` of an edge (road = tract). Regenerate:\n";
    text += std::format("#   mapgen (--out MAP.pmd ... | --info MAP.pmd) --export-graph graph.pd "
                        "--metres-per-px {} --graph-id {}\n",
                        metres_per_px,
                        graph_id);
    text += "# A test compares nodes, edges and lengths with the map; roads are not compared.\n\n";
    text += std::format("province_graph \"{}\" {{\n", graph_id);
    for (const GraphExportNode& node : graph.nodes) {
        text += std::format("    node \"{}\" {{ x_km = {} y_km = {} }}\n", node.key, node.x_km, node.y_km);
    }
    for (const GraphExportEdge& edge : graph.edges) {
        text += std::format("    edge {{ from = province:{} to = province:{} length_km = {} }}\n",
                            graph.nodes[edge.a].key,
                            graph.nodes[edge.b].key,
                            edge.length_km);
    }
    text += "}\n";
    return text;
}

}  // namespace pat::mapdata
