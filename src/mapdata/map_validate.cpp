#include "pat/mapdata/map_validate.hpp"

#include "pat/mapdata/map_stats.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace pat::mapdata {

namespace {

using core::Error;
using core::Result;

Error invalid(int code, std::string message) {
    return Error{code, std::move(message)};
}

}  // namespace

std::vector<Province> compute_province_stats(const MapData& map, std::size_t province_count) {
    const std::size_t n = province_count;
    const std::uint32_t w = map.width;
    const std::uint32_t h = map.height;

    std::vector<Province> out(n);
    std::vector<std::uint64_t> sum_x(n, 0);
    std::vector<std::uint64_t> sum_y(n, 0);
    std::vector<std::uint64_t> sum_h(n, 0);
    std::vector<std::uint32_t> keys;  // (low id << 16 | high id) once per shared pixel edge
    for (std::size_t i = 0; i < n; ++i) {
        out[i].id = static_cast<std::uint16_t>(i + 1);
        out[i].min_x = 0xFFFF;
        out[i].min_y = 0xFFFF;
    }

    for (std::uint32_t y = 0; y < h; ++y) {
        const std::size_t row = static_cast<std::size_t>(y) * w;
        for (std::uint32_t x = 0; x < w; ++x) {
            const std::size_t i = row + x;
            const std::uint16_t p = map.provinces[i];
            if (p == 0) {
                continue;
            }
            Province& pr = out[static_cast<std::size_t>(p) - 1];
            ++pr.area_px;
            sum_x[p - 1] += x;
            sum_y[p - 1] += y;
            sum_h[p - 1] += map.heights[i];
            pr.min_x = std::min<std::uint16_t>(pr.min_x, static_cast<std::uint16_t>(x));
            pr.min_y = std::min<std::uint16_t>(pr.min_y, static_cast<std::uint16_t>(y));
            pr.max_x = std::max<std::uint16_t>(pr.max_x, static_cast<std::uint16_t>(x));
            pr.max_y = std::max<std::uint16_t>(pr.max_y, static_cast<std::uint16_t>(y));

            // Water neighbours (the map edge is not water).
            if ((x > 0 && map.provinces[i - 1] == 0) || (y > 0 && map.provinces[i - w] == 0) ||
                (x + 1 < w && map.provinces[i + 1] == 0) || (y + 1 < h && map.provinces[i + w] == 0)) {
                pr.coastal = true;
            }
            // Shared edges with other provinces: count each edge once (right and down only).
            if (x + 1 < w) {
                const std::uint16_t q = map.provinces[i + 1];
                if (q != 0 && q != p) {
                    keys.push_back((static_cast<std::uint32_t>(std::min(p, q)) << 16U) | std::max(p, q));
                }
            }
            if (y + 1 < h) {
                const std::uint16_t q = map.provinces[i + w];
                if (q != 0 && q != p) {
                    keys.push_back((static_cast<std::uint32_t>(std::min(p, q)) << 16U) | std::max(p, q));
                }
            }
        }
    }

    for (std::size_t i = 0; i < n; ++i) {
        Province& pr = out[i];
        if (pr.area_px == 0) {
            pr.min_x = pr.min_y = 0;
            continue;
        }
        pr.centroid_x = static_cast<std::uint16_t>(sum_x[i] / pr.area_px);
        pr.centroid_y = static_cast<std::uint16_t>(sum_y[i] / pr.area_px);
        pr.mean_height = static_cast<std::uint16_t>(sum_h[i] / pr.area_px);
    }

    std::sort(keys.begin(), keys.end());
    for (std::size_t k = 0; k < keys.size();) {
        std::size_t e = k;
        while (e < keys.size() && keys[e] == keys[k]) {
            ++e;
        }
        const auto a = static_cast<std::uint16_t>(keys[k] >> 16U);
        const auto b = static_cast<std::uint16_t>(keys[k] & 0xFFFFU);
        const auto len = static_cast<std::uint32_t>(e - k);
        out[a - 1].neighbors.push_back({b, len});
        out[b - 1].neighbors.push_back({a, len});
        k = e;
    }
    for (Province& pr : out) {
        std::sort(pr.neighbors.begin(), pr.neighbors.end(), [](const Border& l, const Border& r) {
            return l.neighbor < r.neighbor;
        });
    }
    return out;
}

void rebuild_province_stats(MapData& map) {
    std::vector<Province> fresh = compute_province_stats(map, map.province_list.size());
    for (std::size_t i = 0; i < fresh.size(); ++i) {
        fresh[i].color_rgb = map.province_list[i].color_rgb;
    }
    map.province_list = std::move(fresh);
}

std::size_t reachable_province_count(const MapData& map) {
    const std::size_t n = map.province_list.size();
    if (n == 0) {
        return 0;
    }
    std::vector<char> seen(n + 1, 0);
    std::vector<std::uint16_t> stack = {1};
    seen[1] = 1;
    std::size_t count = 0;
    while (!stack.empty()) {
        const std::uint16_t id = stack.back();
        stack.pop_back();
        ++count;
        for (const Border& b : map.province_list[static_cast<std::size_t>(id) - 1].neighbors) {
            if (b.neighbor >= 1 && b.neighbor <= n && seen[b.neighbor] == 0) {
                seen[b.neighbor] = 1;
                stack.push_back(b.neighbor);
            }
        }
    }
    return count;
}

Result<void, Error> validate_map(const MapData& map) {
    using R = Result<void, Error>;
    const std::size_t pixels = map.pixel_count();
    if (map.width == 0 || map.height == 0) {
        return R::err(invalid(1, "map has zero size"));
    }
    if (map.heights.size() != pixels || map.provinces.size() != pixels || map.materials.size() != pixels) {
        return R::err(invalid(2, "layer sizes do not match width x height"));
    }
    const std::size_t n = map.province_list.size();
    if (n == 0 || n > 65535) {
        return R::err(invalid(3, "province count must be in 1..65535"));
    }

    for (std::size_t i = 0; i < pixels; ++i) {
        if (map.materials[i] >= kMaterialCount) {
            return R::err(invalid(4, "invalid material value"));
        }
        const bool land = map.heights[i] > map.sea_level;
        const std::uint16_t p = map.provinces[i];
        if (land != (p != 0)) {
            return R::err(
                invalid(5, "province id must be non-zero exactly on land (pixel " + std::to_string(i) + ")"));
        }
        if (p > n) {
            return R::err(invalid(6, "province id out of range"));
        }
    }

    // Each province is a single 4-connected region.
    {
        std::vector<char> visited(pixels, 0);
        std::vector<std::uint8_t> regions(n + 1, 0);
        std::vector<std::size_t> stack;
        const std::size_t w = map.width;
        for (std::size_t start = 0; start < pixels; ++start) {
            const std::uint16_t p = map.provinces[start];
            if (p == 0 || visited[start] != 0) {
                continue;
            }
            if (++regions[p] > 1) {
                return R::err(invalid(7, "province " + std::to_string(p) + " is split into several regions"));
            }
            visited[start] = 1;
            stack.push_back(start);
            while (!stack.empty()) {
                const std::size_t i = stack.back();
                stack.pop_back();
                const std::size_t x = i % w;
                const std::size_t y = i / w;
                const auto visit = [&](std::size_t j) {
                    if (visited[j] == 0 && map.provinces[j] == p) {
                        visited[j] = 1;
                        stack.push_back(j);
                    }
                };
                if (x > 0) {
                    visit(i - 1);
                }
                if (x + 1 < w) {
                    visit(i + 1);
                }
                if (y > 0) {
                    visit(i - w);
                }
                if (y + 1 < map.height) {
                    visit(i + w);
                }
            }
        }
        for (std::size_t p = 1; p <= n; ++p) {
            if (regions[p] == 0) {
                return R::err(invalid(8, "province " + std::to_string(p) + " has no pixels"));
            }
        }
    }

    // The table must describe exactly these pixels.
    const std::vector<Province> expected = compute_province_stats(map, n);
    for (std::size_t i = 0; i < n; ++i) {
        Province expect = expected[i];
        expect.color_rgb = map.province_list[i].color_rgb;
        if (!(expect == map.province_list[i])) {
            return R::err(
                invalid(9, "province table entry " + std::to_string(i + 1) + " does not match the pixels"));
        }
    }

    std::vector<std::uint32_t> colors;
    colors.reserve(n);
    for (const Province& p : map.province_list) {
        colors.push_back(p.color_rgb);
    }
    std::sort(colors.begin(), colors.end());
    if (std::adjacent_find(colors.begin(), colors.end()) != colors.end()) {
        return R::err(invalid(10, "province colours are not unique"));
    }

    if (reachable_province_count(map) != n) {
        return R::err(invalid(11, "the province graph is not connected"));
    }
    return R::ok();
}

std::vector<std::int8_t> compute_normals(const MapData& map, double metres_per_pixel) {
    const std::int64_t w = map.width;
    const std::int64_t h = map.height;
    std::vector<std::int8_t> out(map.pixel_count() * 2, 0);
    const double metres_per_unit = static_cast<double>(map.metres_per_unit_milli) / 1000.0;
    const auto at = [&](std::int64_t x, std::int64_t y) {
        x = std::clamp<std::int64_t>(x, 0, w - 1);
        y = std::clamp<std::int64_t>(y, 0, h - 1);
        return static_cast<double>(map.heights[static_cast<std::size_t>(y * w + x)]);
    };
    for (std::int64_t y = 0; y < h; ++y) {
        for (std::int64_t x = 0; x < w; ++x) {
            // Slope in metres of rise per metre of run. Image rows go south, so +y is south.
            const double dzdx = (at(x + 1, y) - at(x - 1, y)) * metres_per_unit / (2.0 * metres_per_pixel);
            const double dzdy = (at(x, y + 1) - at(x, y - 1)) * metres_per_unit / (2.0 * metres_per_pixel);
            const double inv = 1.0 / std::sqrt(dzdx * dzdx + dzdy * dzdy + 1.0);
            const std::size_t o = static_cast<std::size_t>(y * w + x) * 2;
            out[o] = static_cast<std::int8_t>(std::lround(-dzdx * inv * 127.0));
            out[o + 1] = static_cast<std::int8_t>(std::lround(-dzdy * inv * 127.0));
        }
    }
    return out;
}

}  // namespace pat::mapdata
