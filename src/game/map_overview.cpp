#include "pat/game/map_overview.hpp"

#include <algorithm>
#include <cmath>

namespace pat::game {

ui::Season season_of(sim::Date date) {
    const int month = date.ymd().month;
    if (month == 12 || month <= 2) {
        return ui::Season::Winter;
    }
    if (month <= 5) {
        return ui::Season::Spring;
    }
    if (month <= 8) {
        return ui::Season::Summer;
    }
    return ui::Season::Autumn;
}

std::string build_overview_svg(const mapdata::MapData& map,
                               std::size_t cols,
                               std::string_view fill,
                               std::span<const std::uint8_t> include) {
    const std::size_t w = map.width;
    const std::size_t h = map.height;
    cols = std::max<std::size_t>(1, cols);
    const std::size_t rows = (w == 0 || h == 0) ? 1 : std::max<std::size_t>(1, (cols * h + w / 2) / w);

    std::string svg = "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 " + std::to_string(cols) +
                      " " + std::to_string(rows) + "\"><path fill=\"" + std::string(fill) + "\" d=\"";
    if (w != 0 && h != 0 && map.provinces.size() >= w * h) {
        for (std::size_t row = 0; row < rows; ++row) {
            const std::size_t y = std::min(h - 1, (row * 2 + 1) * h / (rows * 2));
            std::size_t run_start = 0;
            bool in_run = false;
            for (std::size_t col = 0; col <= cols; ++col) {
                bool covered = false;
                if (col < cols) {
                    const std::size_t x = std::min(w - 1, (col * 2 + 1) * w / (cols * 2));
                    const std::uint16_t id = map.provinces[y * w + x];
                    covered = id != 0 && (include.empty() || (id < include.size() && include[id] != 0));
                }
                if (covered && !in_run) {
                    in_run = true;
                    run_start = col;
                } else if (!covered && in_run) {
                    in_run = false;
                    svg += "M" + std::to_string(run_start) + " " + std::to_string(row) + "h" +
                           std::to_string(col - run_start) + "v1h-" + std::to_string(col - run_start) + "z";
                }
            }
        }
    }
    svg += "\"/></svg>";
    return svg;
}

std::vector<std::uint8_t> owned_provinces(const sim::World& world,
                                          std::size_t map_province_count,
                                          std::span<const sim::ProvinceId> map_to_world,
                                          sim::DynastyId dynasty) {
    std::vector<std::uint8_t> owned(map_province_count + 1, 0);
    if (!dynasty.valid()) {
        return owned;
    }
    const sim::ProvinceTable& provinces = world.provinces();
    for (std::size_t map_id = 1; map_id < owned.size(); ++map_id) {
        sim::ProvinceId province;
        if (map_to_world.empty()) {
            if (map_id <= provinces.size()) {
                province = sim::ProvinceId(static_cast<std::uint32_t>(map_id - 1));
            }
        } else if (map_id < map_to_world.size() && world.exists(map_to_world[map_id])) {
            province = map_to_world[map_id];
        }
        if (province.valid() && provinces.owner[province.index()] == dynasty) {
            owned[map_id] = 1;
        }
    }
    return owned;
}

ui::MinimapView minimap_view(float focus_x,
                             float focus_y,
                             float distance,
                             float fov_y,
                             float aspect,
                             float map_width,
                             float map_height) {
    ui::MinimapView view;
    if (map_width <= 0.0f || map_height <= 0.0f) {
        return view;
    }
    const float visible_h = 2.0f * distance * std::tan(fov_y * 0.5f);
    const float visible_w = visible_h * aspect;
    view.center_x = focus_x / map_width;
    view.center_y = focus_y / map_height;
    view.width = std::min(1.0f, visible_w / map_width);
    view.height = std::min(1.0f, visible_h / map_height);
    return view;
}

int round_distance_km(double km) {
    if (km < 1.0) {
        return 0;
    }
    double magnitude = 1.0;
    while (magnitude * 10.0 <= km) {
        magnitude *= 10.0;
    }
    for (const double factor : {5.0, 2.0, 1.0}) {
        if (magnitude * factor <= km) {
            return static_cast<int>(magnitude * factor);
        }
    }
    return static_cast<int>(magnitude);
}

int scale_label_km(const ui::MinimapView& view, int map_width_km) {
    return round_distance_km(static_cast<double>(view.width) * map_width_km / 5.0);
}

}  // namespace pat::game
