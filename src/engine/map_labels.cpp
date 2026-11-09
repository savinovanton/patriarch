#include "pat/engine/map_labels.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string>
#include <string_view>

namespace pat::engine {

namespace {

// Placeholder toponyms for the test map. Not historical data.
constexpr std::array<const char*, 48> kProvinceNames = {
    "Новгородский", "Псковский",    "Тверской",     "Ярославский",  "Костромской",  "Нижегородский",
    "Рязанский",    "Тульский",     "Калужский",    "Смоленский",   "Брянский",     "Курский",
    "Белгородский", "Воронежский",  "Тамбовский",   "Пензенский",   "Симбирский",   "Казанский",
    "Вятский",      "Пермский",     "Вологодский",  "Устюжский",    "Холмогорский", "Олонецкий",
    "Ладожский",    "Выборгский",   "Астраханский", "Азовский",     "Белозерский",  "Тобольский",
    "Томский",      "Енисейский",   "Якутский",     "Иркутский",    "Нерчинский",   "Охотский",
    "Камчатский",   "Мангазейский", "Верхотурский", "Тюменский",    "Уфимский",     "Саратовский",
    "Самарский",    "Царицынский",  "Орловский",    "Владимирский", "Суздальский",  "Московский"};

constexpr std::array<const char*, 24> kRegionNames = {
    "Поморье",       "Заонежье",        "Новгородчина",    "Залесье",         "Поволжье",    "Приуралье",
    "Зауралье",      "Западная Сибирь", "Приобье",         "Енисейский край", "Прибайкалье", "Забайкалье",
    "Приамурье",     "Приморье",        "Колыма",          "Камчатка",        "Подонье",     "Слобожанщина",
    "Причерноморье", "Предкавказье",    "Нижнее Поволжье", "Смоленщина",      "Литва",       "Печора"};

std::string province_name(std::uint32_t id) {
    const std::size_t pool = kProvinceNames.size();
    const std::string base = kProvinceNames[id % pool];
    const std::size_t round = id / pool;  // 0 for the first pass through the pool
    return round == 0 ? base : base + " " + std::to_string(round + 1);
}

std::string region_name(std::size_t index) {
    const std::size_t pool = kRegionNames.size();
    const std::string base = kRegionNames[index % pool];
    return index < pool ? base : base + " " + std::to_string(index / pool + 1);
}

struct Anchor {
    float x = 0.0f;
    float y = 0.0f;
};

/// The province pixel closest to the province's centroid (the centroid itself may lie outside a concave
/// province).
Anchor anchor_pixel(const mapdata::MapData& map, const mapdata::Province& p) {
    const float cx = static_cast<float>(p.centroid_x);
    const float cy = static_cast<float>(p.centroid_y);
    if (map.provinces[map.index(p.centroid_x, p.centroid_y)] == p.id) {
        return {cx + 0.5f, cy + 0.5f};
    }
    float best = std::numeric_limits<float>::max();
    Anchor out{cx + 0.5f, cy + 0.5f};
    for (std::uint32_t y = p.min_y; y <= p.max_y; ++y) {
        for (std::uint32_t x = p.min_x; x <= p.max_x; ++x) {
            if (map.provinces[map.index(x, y)] != p.id) {
                continue;
            }
            const float dx = static_cast<float>(x) - cx;
            const float dy = static_cast<float>(y) - cy;
            const float d = dx * dx + dy * dy;
            if (d < best) {
                best = d;
                out = {static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f};
            }
        }
    }
    return out;
}

MapLabel make_province_label(const mapdata::Province& province,
                             const Anchor& anchor,
                             const HeightfieldPicker& picker,
                             const SdfAtlas& atlas,
                             std::string_view name) {
    MapLabel label;
    label.id = province.id;
    label.kind = LabelKind::Province;
    label.text = utf8_to_utf32(name);
    label.anchor = {anchor.x, anchor.y, picker.height_at(anchor.x, anchor.y) + 2.0f};
    label.world_extent = std::sqrt(static_cast<float>(province.area_px));
    label.width_per_px = measure_text(atlas, label.text, 1.0f);
    return label;
}

}  // namespace

MapLabelSet make_test_map_labels(const mapdata::MapData& map,
                                 const TerrainGeometry& geometry,
                                 const HeightfieldPicker& picker,
                                 const SdfAtlas& atlas) {
    (void)geometry;
    MapLabelSet set;
    const std::size_t n = map.province_list.size();
    if (n == 0) {
        return set;
    }

    std::vector<Anchor> anchors(n);
    for (std::size_t i = 0; i < n; ++i) {
        anchors[i] = anchor_pixel(map, map.province_list[i]);
    }

    // Province labels.
    for (std::size_t i = 0; i < n; ++i) {
        const mapdata::Province& p = map.province_list[i];
        set.labels.push_back(make_province_label(p, anchors[i], picker, atlas, province_name(p.id)));
    }
    set.province_labels = n;

    // Regions: farthest-point seeds over the province anchors, then a few k-means steps weighted by area.
    const std::size_t k =
        std::max<std::size_t>(1, static_cast<std::size_t>(std::lround(std::sqrt(static_cast<double>(n)))));
    std::vector<Anchor> centres;
    centres.push_back(anchors[0]);
    while (centres.size() < k) {
        std::size_t best_i = 0;
        float best_d = -1.0f;
        for (std::size_t i = 0; i < n; ++i) {
            float nearest = std::numeric_limits<float>::max();
            for (const Anchor& c : centres) {
                nearest = std::min(nearest,
                                   (anchors[i].x - c.x) * (anchors[i].x - c.x) +
                                       (anchors[i].y - c.y) * (anchors[i].y - c.y));
            }
            if (nearest > best_d) {
                best_d = nearest;
                best_i = i;
            }
        }
        centres.push_back(anchors[best_i]);
    }
    std::vector<std::size_t> region_of(n, 0);
    for (int iteration = 0; iteration < 4; ++iteration) {
        for (std::size_t i = 0; i < n; ++i) {
            float best = std::numeric_limits<float>::max();
            for (std::size_t c = 0; c < centres.size(); ++c) {
                const float d = (anchors[i].x - centres[c].x) * (anchors[i].x - centres[c].x) +
                                (anchors[i].y - centres[c].y) * (anchors[i].y - centres[c].y);
                if (d < best) {
                    best = d;
                    region_of[i] = c;
                }
            }
        }
        std::vector<double> sx(centres.size(), 0.0);
        std::vector<double> sy(centres.size(), 0.0);
        std::vector<double> sw(centres.size(), 0.0);
        for (std::size_t i = 0; i < n; ++i) {
            const double w = map.province_list[i].area_px;
            sx[region_of[i]] += anchors[i].x * w;
            sy[region_of[i]] += anchors[i].y * w;
            sw[region_of[i]] += w;
        }
        for (std::size_t c = 0; c < centres.size(); ++c) {
            if (sw[c] > 0.0) {
                centres[c] = {static_cast<float>(sx[c] / sw[c]), static_cast<float>(sy[c] / sw[c])};
            }
        }
    }
    // One label per non-empty region, attached to the member province closest to the cluster centre.
    std::size_t region_index = 0;
    for (std::size_t c = 0; c < centres.size(); ++c) {
        double area = 0.0;
        std::size_t anchor_member = n;
        float best = std::numeric_limits<float>::max();
        for (std::size_t i = 0; i < n; ++i) {
            if (region_of[i] != c) {
                continue;
            }
            area += map.province_list[i].area_px;
            const float d = (anchors[i].x - centres[c].x) * (anchors[i].x - centres[c].x) +
                            (anchors[i].y - centres[c].y) * (anchors[i].y - centres[c].y);
            if (d < best) {
                best = d;
                anchor_member = i;
            }
        }
        if (anchor_member == n) {
            continue;
        }
        MapLabel label;
        label.id = static_cast<std::uint32_t>(100000 + c);
        label.kind = LabelKind::Region;
        label.text = utf8_to_utf32(region_name(region_index++));
        label.anchor = {anchors[anchor_member].x,
                        anchors[anchor_member].y,
                        picker.height_at(anchors[anchor_member].x, anchors[anchor_member].y) + 2.0f};
        label.world_extent = std::sqrt(static_cast<float>(area));
        label.width_per_px = measure_text(atlas, label.text, 1.0f);
        set.labels.push_back(std::move(label));
        ++set.region_labels;
    }
    return set;
}

MapLabelSet make_province_labels(const mapdata::MapData& map,
                                 const HeightfieldPicker& picker,
                                 const SdfAtlas& atlas,
                                 std::span<const std::string> names) {
    MapLabelSet set;
    set.labels.reserve(map.province_list.size());
    for (const mapdata::Province& province : map.province_list) {
        if (province.id != 0 && province.id < names.size() && !names[province.id].empty()) {
            set.labels.push_back(make_province_label(
                province, anchor_pixel(map, province), picker, atlas, names[province.id]));
        }
    }
    set.province_labels = set.labels.size();
    return set;
}

core::Result<std::unique_ptr<MapLabelRenderer>>
MapLabelRenderer::create(IRenderer& renderer, SdfAtlas atlas, MapLabelSet labels) {
    using R = core::Result<std::unique_ptr<MapLabelRenderer>>;
    auto text = renderer.create_text_renderer(atlas);
    if (!text) {
        return R::err(text.error());
    }
    std::unique_ptr<MapLabelRenderer> out(new MapLabelRenderer());
    out->atlas_ = std::move(atlas);
    out->labels_ = std::move(labels);
    out->text_ = std::move(text).value();
    return R::ok(std::move(out));
}

LabelLayoutStats MapLabelRenderer::draw(const LabelLayoutInput& input, const LabelStyle& style) {
    const LabelLayoutStats stats = layout_labels(labels_.labels, input, style, placed_);
    quads_.clear();
    for (const PlacedLabel& p : placed_) {
        geometry_.clear();
        // Centre the text vertically on the anchor: the baseline sits a bit below it.
        layout_text(atlas_, p.label->text, p.size_px, p.x, p.y + p.size_px * 0.33f, geometry_);
        const bool region = p.label->kind == LabelKind::Region;
        const Color fill = region ? Color{1.0f, 0.93f, 0.68f, p.alpha} : Color{0.97f, 0.95f, 0.88f, p.alpha};
        const Color outline = Color{0.06f, 0.045f, 0.03f, p.alpha * 0.92f};
        for (const TextQuadGeometry& g : geometry_) {
            quads_.push_back({g, fill, outline});
        }
    }
    if (!quads_.empty()) {
        text_->draw(quads_);
    }
    return stats;
}

}  // namespace pat::engine
