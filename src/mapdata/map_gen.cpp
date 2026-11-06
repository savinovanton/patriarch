#include "pat/mapdata/map_gen.hpp"

#include "pat/core/log.hpp"
#include "pat/core/rng.hpp"
#include "pat/mapdata/map_stats.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numeric>
#include <thread>

namespace pat::mapdata {

namespace {

using Clock = std::chrono::steady_clock;

// ------------------------------------------------------------------ parallel helper

unsigned resolve_threads(unsigned requested) {
    if (requested != 0) {
        return requested;
    }
    return std::max(1U, std::thread::hardware_concurrency());
}

/// Runs fn(row_begin, row_end) over contiguous row ranges on `threads` workers.
template <class Fn>
void parallel_rows(std::uint32_t rows, unsigned threads, Fn&& fn) {
    threads = std::max(1U, std::min<unsigned>(threads, rows));
    if (threads == 1) {
        fn(0U, rows);
        return;
    }
    std::vector<std::thread> pool;
    pool.reserve(threads);
    for (unsigned t = 0; t < threads; ++t) {
        const std::uint32_t begin =
            static_cast<std::uint32_t>(static_cast<std::uint64_t>(rows) * t / threads);
        const std::uint32_t end =
            static_cast<std::uint32_t>(static_cast<std::uint64_t>(rows) * (t + 1) / threads);
        pool.emplace_back([&fn, begin, end] { fn(begin, end); });
    }
    for (std::thread& th : pool) {
        th.join();
    }
}

// ------------------------------------------------------------------ noise

std::uint32_t hash_lattice(int x, int y, std::uint32_t seed) {
    std::uint32_t h = static_cast<std::uint32_t>(x) * 374761393U +
                      static_cast<std::uint32_t>(y) * 668265263U + seed * 2246822519U;
    h = (h ^ (h >> 13U)) * 1274126177U;
    return h ^ (h >> 16U);
}

float fade(float t) {
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

float gradient(std::uint32_t h, float dx, float dy) {
    static constexpr float kInvSqrt2 = 0.70710678f;
    static constexpr float gx[8] = {1, -1, 0, 0, kInvSqrt2, -kInvSqrt2, kInvSqrt2, -kInvSqrt2};
    static constexpr float gy[8] = {0, 0, 1, -1, kInvSqrt2, kInvSqrt2, -kInvSqrt2, -kInvSqrt2};
    return gx[h & 7U] * dx + gy[h & 7U] * dy;
}

/// 2D gradient noise, roughly in [-1, 1].
float perlin(float x, float y, std::uint32_t seed) {
    const float fx = std::floor(x);
    const float fy = std::floor(y);
    const int ix = static_cast<int>(fx);
    const int iy = static_cast<int>(fy);
    const float tx = x - fx;
    const float ty = y - fy;
    const float n00 = gradient(hash_lattice(ix, iy, seed), tx, ty);
    const float n10 = gradient(hash_lattice(ix + 1, iy, seed), tx - 1.0f, ty);
    const float n01 = gradient(hash_lattice(ix, iy + 1, seed), tx, ty - 1.0f);
    const float n11 = gradient(hash_lattice(ix + 1, iy + 1, seed), tx - 1.0f, ty - 1.0f);
    const float u = fade(tx);
    const float v = fade(ty);
    const float a = n00 + (n10 - n00) * u;
    const float b = n01 + (n11 - n01) * u;
    return (a + (b - a) * v) * 1.4142f;
}

/// Fractal sum of octaves, normalized to roughly [-1, 1].
float fbm(float x, float y, std::uint32_t seed, int octaves) {
    float sum = 0.0f;
    float amp = 1.0f;
    float norm = 0.0f;
    for (int o = 0; o < octaves; ++o) {
        sum += perlin(x, y, seed + static_cast<std::uint32_t>(o) * 1013U) * amp;
        norm += amp;
        amp *= 0.5f;
        x = x * 2.0f + 17.3f;
        y = y * 2.0f + 5.9f;
    }
    return sum / norm;
}

/// Ridged variant in [0, 1]: sharp crests, used for mountain ranges.
float ridged(float x, float y, std::uint32_t seed, int octaves) {
    float sum = 0.0f;
    float amp = 1.0f;
    float norm = 0.0f;
    for (int o = 0; o < octaves; ++o) {
        const float n = 1.0f - std::fabs(perlin(x, y, seed + static_cast<std::uint32_t>(o) * 7919U));
        sum += n * n * amp;
        norm += amp;
        amp *= 0.5f;
        x = x * 2.0f + 3.1f;
        y = y * 2.0f + 11.7f;
    }
    return sum / norm;
}

float saturate(float v) {
    return std::clamp(v, 0.0f, 1.0f);
}

float smoothstep(float lo, float hi, float v) {
    const float t = saturate((v - lo) / (hi - lo));
    return t * t * (3.0f - 2.0f * t);
}

/// Distance from point p to the segment a-b.
float segment_distance(float px, float py, float ax, float ay, float bx, float by) {
    const float abx = bx - ax;
    const float aby = by - ay;
    const float t = saturate(((px - ax) * abx + (py - ay) * aby) / (abx * abx + aby * aby));
    const float dx = px - (ax + abx * t);
    const float dy = py - (ay + aby * t);
    return std::sqrt(dx * dx + dy * dy);
}

// ------------------------------------------------------------------ stage 1: heights

constexpr float kSeaLevelF = static_cast<float>(kGeneratedSeaLevel) / 65535.0f;

void generate_heights(MapData& map, const GenParams& params, unsigned threads) {
    const std::uint32_t w = map.width;
    const std::uint32_t h = map.height;
    const auto seed32 = static_cast<std::uint32_t>(params.seed ^ (params.seed >> 32U));
    const std::uint32_t s_coast = seed32 * 3U + 11U;
    const std::uint32_t s_hills = seed32 * 5U + 23U;
    const std::uint32_t s_ridge = seed32 * 7U + 31U;
    const std::uint32_t s_detail = seed32 * 11U + 47U;
    const std::uint32_t s_line = seed32 * 13U + 59U;

    parallel_rows(h, threads, [&](std::uint32_t y0, std::uint32_t y1) {
        for (std::uint32_t y = y0; y < y1; ++y) {
            const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(h);
            for (std::uint32_t x = 0; x < w; ++x) {
                const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(w);
                // Noise space is 2 x 1 units so features are isotropic on a 2:1 map.
                const float nx = u * 2.0f;
                const float ny = v;

                // Continent: an ellipse whose edge is distorted by fractal noise.
                const float ex = (u - 0.52f) / 0.40f;
                const float ey = (v - 0.52f) / 0.37f;
                const float e = std::sqrt(ex * ex + ey * ey);
                // Two noise scales: broad bays and peninsulas plus a ragged fine coastline.
                const float broad = fbm(nx * 1.3f + 3.1f, ny * 1.3f + 7.7f, s_coast, 3);
                const float ragged = fbm(nx * 5.5f + 1.9f, ny * 5.5f + 4.3f, s_coast + 5U, 5);
                const float c = 1.0f - e + 0.50f * broad + 0.16f * ragged;

                float height = kSeaLevelF;
                if (c < 0.0f) {
                    height += 0.20f * std::max(c, -1.0f);  // Sea floor deepens away from the coast.
                } else {
                    const float land = smoothstep(0.0f, 0.06f, c);
                    height += 0.10f * std::min(c, 1.0f);  // Lowland slope inland from the coast.

                    // Rolling hills.
                    height += land * 0.09f * std::max(0.0f, fbm(nx * 4.5f, ny * 4.5f, s_hills, 4) + 0.15f);

                    // Ural-like ridge: a wavering north-south band with ridged crests.
                    const float ridge_u = 0.50f + 0.035f * fbm(ny * 3.0f, 0.5f, s_line, 3);
                    const float ridge_w = (u - ridge_u) / 0.016f;
                    const float ridge_fade =
                        0.45f + 0.55f * smoothstep(-0.2f, 0.4f, fbm(ny * 5.0f, 9.0f, s_line + 1U, 3));
                    height += land * 0.26f * std::exp(-ridge_w * ridge_w) * ridge_fade *
                              (0.55f + 0.45f * ridged(nx * 9.0f, ny * 9.0f, s_ridge, 4));

                    // Southern chain (Caucasus-like) and eastern chain.
                    const float d_south = segment_distance(u, v, 0.30f, 0.80f, 0.60f, 0.90f);
                    height += land * 0.30f * std::exp(-(d_south * d_south) / (0.025f * 0.025f)) *
                              (0.5f + 0.5f * ridged(nx * 8.0f, ny * 8.0f, s_ridge + 1U, 4));
                    const float d_east = segment_distance(u, v, 0.70f, 0.28f, 0.86f, 0.58f);
                    height += land * 0.22f * std::exp(-(d_east * d_east) / (0.03f * 0.03f)) *
                              (0.5f + 0.5f * ridged(nx * 8.0f, ny * 8.0f, s_ridge + 2U, 4));

                    // Fine detail.
                    height += land * 0.018f * fbm(nx * 18.0f, ny * 18.0f, s_detail, 5);
                }
                const float clamped = std::clamp(height, 0.02f, 1.0f);
                map.heights[static_cast<std::size_t>(y) * w + x] =
                    static_cast<std::uint16_t>(clamped * 65535.0f);
            }
        }
    });
    // Land is strictly above the waterline: nudge any rounding ties into the water.
    for (std::uint16_t& hv : map.heights) {
        if (hv == kGeneratedSeaLevel) {
            hv = kGeneratedSeaLevel - 1;
        }
    }
}

// ------------------------------------------------------------------ stage 2: single landmass

/// Flood-fills the land reachable from the centre; every other piece of land becomes shallow water.
/// Returns the number of land pixels that remain.
std::size_t keep_main_landmass(MapData& map) {
    const std::int64_t w = map.width;
    const std::int64_t h = map.height;
    const std::size_t pixels = map.pixel_count();
    std::vector<char> keep(pixels, 0);

    // Start from the land pixel closest to the centre.
    std::size_t start = pixels;
    for (std::int64_t r = 0; r < std::max(w, h) && start == pixels; ++r) {
        for (std::int64_t dy = -r; dy <= r && start == pixels; ++dy) {
            for (std::int64_t dx = -r; dx <= r; ++dx) {
                if (std::max(std::llabs(dx), std::llabs(dy)) != r) {
                    continue;
                }
                const std::int64_t x = w / 2 + dx;
                const std::int64_t y = h / 2 + dy;
                if (x < 0 || y < 0 || x >= w || y >= h) {
                    continue;
                }
                const auto i = static_cast<std::size_t>(y * w + x);
                if (map.is_land(i)) {
                    start = i;
                    break;
                }
            }
        }
    }
    if (start == pixels) {
        return 0;
    }

    std::vector<std::uint32_t> queue;
    queue.push_back(static_cast<std::uint32_t>(start));
    keep[start] = 1;
    for (std::size_t head = 0; head < queue.size(); ++head) {
        const std::size_t i = queue[head];
        const std::int64_t x = static_cast<std::int64_t>(i) % w;
        const std::int64_t y = static_cast<std::int64_t>(i) / w;
        const auto visit = [&](std::int64_t nx, std::int64_t ny) {
            if (nx < 0 || ny < 0 || nx >= w || ny >= h) {
                return;
            }
            const auto j = static_cast<std::size_t>(ny * w + nx);
            if (keep[j] == 0 && map.is_land(j)) {
                keep[j] = 1;
                queue.push_back(static_cast<std::uint32_t>(j));
            }
        };
        visit(x - 1, y);
        visit(x + 1, y);
        visit(x, y - 1);
        visit(x, y + 1);
    }
    for (std::size_t i = 0; i < pixels; ++i) {
        if (keep[i] == 0 && map.is_land(i)) {
            map.heights[i] = static_cast<std::uint16_t>(map.sea_level - 200);
        }
    }
    return queue.size();
}

// ------------------------------------------------------------------ stage 3: provinces

struct Seed {
    float x = 0.0f;
    float y = 0.0f;
};

/// Poisson-disk-like sampling of land points; relaxes the minimum distance until `count` fit.
std::vector<Seed>
pick_seeds(const MapData& map, std::size_t land_pixels, std::uint32_t count, std::uint64_t seed) {
    core::Pcg32 rng(seed, 0x5EEDULL);
    const std::uint32_t w = map.width;
    const std::uint32_t h = map.height;
    const float spacing = std::sqrt(static_cast<float>(land_pixels) / static_cast<float>(count));
    float min_dist = spacing * 0.85f;

    std::vector<Seed> seeds;
    seeds.reserve(count);
    int stale_rounds = 0;
    while (seeds.size() < count) {
        bool placed = false;
        for (int attempt = 0; attempt < 400; ++attempt) {
            const std::uint32_t x = rng.bounded(w);
            const std::uint32_t y = rng.bounded(h);
            if (!map.is_land(map.index(x, y))) {
                continue;
            }
            bool ok = true;
            for (const Seed& s : seeds) {
                const float dx = s.x - static_cast<float>(x);
                const float dy = s.y - static_cast<float>(y);
                if (dx * dx + dy * dy < min_dist * min_dist) {
                    ok = false;
                    break;
                }
            }
            if (ok) {
                seeds.push_back({static_cast<float>(x), static_cast<float>(y)});
                placed = true;
                break;
            }
        }
        if (!placed) {
            min_dist *= 0.92f;  // Too crowded: allow tighter packing.
            if (++stale_rounds > 200) {
                break;
            }
        }
    }
    // Stable, spatially coherent ids: top to bottom in bands, then left to right.
    const float band = std::max(1.0f, spacing);
    std::sort(seeds.begin(), seeds.end(), [band](const Seed& a, const Seed& b) {
        const auto ba = static_cast<int>(a.y / band);
        const auto bb = static_cast<int>(b.y / band);
        return ba != bb ? ba < bb : a.x < b.x;
    });
    return seeds;
}

constexpr std::uint16_t kUnassigned = 0xFFFF;

void assign_provinces(MapData& map,
                      const std::vector<Seed>& seeds,
                      std::size_t land_pixels,
                      std::uint32_t seed32,
                      unsigned threads) {
    const std::uint32_t w = map.width;
    const std::uint32_t h = map.height;
    const float spacing = std::sqrt(static_cast<float>(land_pixels) / static_cast<float>(seeds.size()));
    const float cell = std::max(spacing, 4.0f);
    const int gw = static_cast<int>(std::ceil(static_cast<float>(w) / cell));
    const int gh = static_cast<int>(std::ceil(static_cast<float>(h) / cell));

    std::vector<std::vector<std::uint16_t>> grid(static_cast<std::size_t>(gw) * static_cast<std::size_t>(gh));
    const auto cell_of = [&](float v, int limit) {
        return std::clamp(static_cast<int>(v / cell), 0, limit - 1);
    };
    for (std::size_t i = 0; i < seeds.size(); ++i) {
        grid[static_cast<std::size_t>(cell_of(seeds[i].y, gh)) * static_cast<std::size_t>(gw) +
             static_cast<std::size_t>(cell_of(seeds[i].x, gw))]
            .push_back(static_cast<std::uint16_t>(i + 1));
    }

    const float warp_amp = spacing * 0.20f;
    const std::uint32_t s_warp = seed32 * 17U + 71U;
    const float freq = 1.6f / spacing;  // Roughly one wobble per province.

    parallel_rows(h, threads, [&](std::uint32_t y0, std::uint32_t y1) {
        for (std::uint32_t y = y0; y < y1; ++y) {
            for (std::uint32_t x = 0; x < w; ++x) {
                const std::size_t idx = static_cast<std::size_t>(y) * w + x;
                if (!map.is_land(idx)) {
                    map.provinces[idx] = 0;
                    continue;
                }
                // Warped sample position gives organic, non-straight borders.
                const float fx = static_cast<float>(x);
                const float fy = static_cast<float>(y);
                const float qx = std::clamp(
                    fx + warp_amp * perlin(fx * freq, fy * freq, s_warp), 0.0f, static_cast<float>(w - 1));
                const float qy =
                    std::clamp(fy + warp_amp * perlin(fx * freq + 31.7f, fy * freq + 13.1f, s_warp + 1U),
                               0.0f,
                               static_cast<float>(h - 1));
                const int cx = cell_of(qx, gw);
                const int cy = cell_of(qy, gh);

                float best = 1e30f;
                std::uint16_t best_id = 0;
                const int max_ring = std::max(gw, gh);
                for (int ring = 0; ring <= max_ring; ++ring) {
                    for (int gy = std::max(0, cy - ring); gy <= std::min(gh - 1, cy + ring); ++gy) {
                        for (int gx = std::max(0, cx - ring); gx <= std::min(gw - 1, cx + ring); ++gx) {
                            if (std::max(std::abs(gx - cx), std::abs(gy - cy)) != ring) {
                                continue;
                            }
                            for (const std::uint16_t id :
                                 grid[static_cast<std::size_t>(gy) * static_cast<std::size_t>(gw) +
                                      static_cast<std::size_t>(gx)]) {
                                const float dx = seeds[id - 1U].x - qx;
                                const float dy = seeds[id - 1U].y - qy;
                                const float d2 = dx * dx + dy * dy;
                                if (d2 < best) {
                                    best = d2;
                                    best_id = id;
                                }
                            }
                        }
                    }
                    const float reach = static_cast<float>(ring) * cell;
                    if (best_id != 0 && best <= reach * reach) {
                        break;
                    }
                }
                map.provinces[idx] = best_id;
            }
        }
    });

    // A seed always owns its own pixel, whatever the warp did.
    for (std::size_t i = 0; i < seeds.size(); ++i) {
        map.provinces[map.index(static_cast<std::uint32_t>(seeds[i].x),
                                static_cast<std::uint32_t>(seeds[i].y))] = static_cast<std::uint16_t>(i + 1);
    }
}

/// Keeps only the largest 4-connected piece of every province; the leftover pieces are absorbed by
/// the neighbouring provinces they touch. Guarantees that each province is one connected region.
void make_provinces_connected(MapData& map, std::uint32_t province_count) {
    const std::int64_t w = map.width;
    const std::int64_t h = map.height;
    const std::size_t pixels = map.pixel_count();

    struct Component {
        std::uint32_t start = 0;
        std::uint32_t size = 0;
    };
    std::vector<char> visited(pixels, 0);
    std::vector<std::vector<Component>> components(static_cast<std::size_t>(province_count) + 1);
    std::vector<std::uint32_t> queue;

    const auto neighbours = [&](std::size_t i, auto&& fn) {
        const std::int64_t x = static_cast<std::int64_t>(i) % w;
        const std::int64_t y = static_cast<std::int64_t>(i) / w;
        if (x > 0) {
            fn(i - 1);
        }
        if (x + 1 < w) {
            fn(i + 1);
        }
        if (y > 0) {
            fn(i - static_cast<std::size_t>(w));
        }
        if (y + 1 < h) {
            fn(i + static_cast<std::size_t>(w));
        }
    };

    for (std::size_t s = 0; s < pixels; ++s) {
        const std::uint16_t p = map.provinces[s];
        if (p == 0 || visited[s] != 0) {
            continue;
        }
        queue.clear();
        queue.push_back(static_cast<std::uint32_t>(s));
        visited[s] = 1;
        for (std::size_t head = 0; head < queue.size(); ++head) {
            neighbours(queue[head], [&](std::size_t j) {
                if (visited[j] == 0 && map.provinces[j] == p) {
                    visited[j] = 1;
                    queue.push_back(static_cast<std::uint32_t>(j));
                }
            });
        }
        components[p].push_back({static_cast<std::uint32_t>(s), static_cast<std::uint32_t>(queue.size())});
    }

    // Detach every piece except the largest of each province.
    std::vector<std::uint32_t> frontier;
    for (std::uint32_t p = 1; p <= province_count; ++p) {
        auto& list = components[p];
        if (list.size() <= 1) {
            continue;
        }
        std::size_t keep = 0;
        for (std::size_t k = 1; k < list.size(); ++k) {
            if (list[k].size > list[keep].size ||
                (list[k].size == list[keep].size && list[k].start < list[keep].start)) {
                keep = k;
            }
        }
        for (std::size_t k = 0; k < list.size(); ++k) {
            if (k == keep) {
                continue;
            }
            queue.clear();
            queue.push_back(list[k].start);
            map.provinces[list[k].start] = kUnassigned;
            for (std::size_t head = 0; head < queue.size(); ++head) {
                neighbours(queue[head], [&](std::size_t j) {
                    if (map.provinces[j] == p) {
                        map.provinces[j] = kUnassigned;
                        queue.push_back(static_cast<std::uint32_t>(j));
                    }
                });
            }
        }
    }

    // Multi-source growth: assigned land next to unassigned land expands into it.
    for (std::size_t i = 0; i < pixels; ++i) {
        const std::uint16_t p = map.provinces[i];
        if (p == 0 || p == kUnassigned) {
            continue;
        }
        bool border = false;
        neighbours(i, [&](std::size_t j) { border = border || map.provinces[j] == kUnassigned; });
        if (border) {
            frontier.push_back(static_cast<std::uint32_t>(i));
        }
    }
    for (std::size_t head = 0; head < frontier.size(); ++head) {
        const std::size_t i = frontier[head];
        const std::uint16_t p = map.provinces[i];
        neighbours(i, [&](std::size_t j) {
            if (map.provinces[j] == kUnassigned) {
                map.provinces[j] = p;
                frontier.push_back(static_cast<std::uint32_t>(j));
            }
        });
    }
}

// ------------------------------------------------------------------ stage 4: materials

void classify_materials(MapData& map, std::uint64_t seed, unsigned threads) {
    const std::uint32_t w = map.width;
    const std::uint32_t h = map.height;
    const auto seed32 = static_cast<std::uint32_t>(seed ^ (seed >> 32U));
    const std::uint32_t s_forest = seed32 * 19U + 83U;
    const float land_range = 65535.0f - static_cast<float>(map.sea_level);

    parallel_rows(h, threads, [&](std::uint32_t y0, std::uint32_t y1) {
        for (std::uint32_t y = y0; y < y1; ++y) {
            const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(h);
            const std::uint32_t ym = y > 0 ? y - 1 : y;
            const std::uint32_t yp = y + 1 < h ? y + 1 : y;
            for (std::uint32_t x = 0; x < w; ++x) {
                const std::size_t i = static_cast<std::size_t>(y) * w + x;
                const std::uint16_t hv = map.heights[i];
                Material m = Material::DeepWater;
                if (hv <= map.sea_level) {
                    m = (map.sea_level - hv) > 2400 ? Material::DeepWater : Material::ShallowWater;
                } else {
                    const std::uint32_t xm = x > 0 ? x - 1 : x;
                    const std::uint32_t xp = x + 1 < w ? x + 1 : x;
                    const float gx = static_cast<float>(map.heights[static_cast<std::size_t>(y) * w + xp]) -
                                     static_cast<float>(map.heights[static_cast<std::size_t>(y) * w + xm]);
                    const float gy = static_cast<float>(map.heights[static_cast<std::size_t>(yp) * w + x]) -
                                     static_cast<float>(map.heights[static_cast<std::size_t>(ym) * w + x]);
                    const float slope =
                        std::sqrt(gx * gx + gy * gy) / land_range * static_cast<float>(w) * 0.05f;
                    const float elev = static_cast<float>(hv - map.sea_level) / land_range;
                    const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(w);
                    const float tree = fbm(u * 2.0f * 7.0f, v * 7.0f, s_forest, 4);
                    const float vz =
                        v + 0.06f * fbm(u * 2.0f * 3.0f, v * 3.0f, s_forest + 9U, 3);  // soft zone borders

                    if (elev > 0.42f) {
                        m = Material::Snow;
                    } else if (elev > 0.26f || slope > 1.0f) {
                        m = Material::Rock;
                    } else if (elev > 0.14f || slope > 0.5f) {
                        m = Material::Hills;
                    } else if (elev < 0.012f) {
                        m = Material::Beach;
                    } else if (vz < 0.22f) {
                        m = Material::Tundra;
                    } else if (vz > 0.62f) {
                        m = tree > 0.25f ? Material::Forest : Material::Steppe;
                    } else {
                        m = tree > -0.05f ? Material::Forest : Material::Plains;
                    }
                }
                map.materials[i] = static_cast<std::uint8_t>(m);
            }
        }
    });
}

// ------------------------------------------------------------------ stage 5: colours

std::uint32_t province_color(std::uint32_t id) {
    // Golden-ratio hue steps keep neighbouring ids visually distinct.
    const float hue = std::fmod(static_cast<float>(id) * 0.61803398875f, 1.0f);
    const float sat = 0.45f + 0.30f * std::fmod(static_cast<float>(id) * 0.37f, 1.0f);
    const float val = 0.70f + 0.25f * std::fmod(static_cast<float>(id) * 0.73f, 1.0f);
    const float hh = hue * 6.0f;
    const int sector = static_cast<int>(hh) % 6;
    const float f = hh - std::floor(hh);
    const float p = val * (1.0f - sat);
    const float q = val * (1.0f - sat * f);
    const float t = val * (1.0f - sat * (1.0f - f));
    float r = val;
    float g = t;
    float b = p;
    switch (sector) {
        case 1:
            r = q;
            g = val;
            b = p;
            break;
        case 2:
            r = p;
            g = val;
            b = t;
            break;
        case 3:
            r = p;
            g = q;
            b = val;
            break;
        case 4:
            r = t;
            g = p;
            b = val;
            break;
        case 5:
            r = val;
            g = p;
            b = q;
            break;
        default:
            break;
    }
    const auto to8 = [](float c) {
        return static_cast<std::uint32_t>(std::lround(std::clamp(c, 0.0f, 1.0f) * 255.0f));
    };
    return (to8(r) << 16U) | (to8(g) << 8U) | to8(b);
}

double elapsed_ms(Clock::time_point since) {
    return std::chrono::duration<double, std::milli>(Clock::now() - since).count();
}

}  // namespace

MapData generate_map(const GenParams& params) {
    const unsigned threads = resolve_threads(params.threads);
    const auto seed32 = static_cast<std::uint32_t>(params.seed ^ (params.seed >> 32U));
    const auto t_start = Clock::now();

    MapData map;
    map.width = params.width;
    map.height = params.height;
    map.sea_level = kGeneratedSeaLevel;
    map.metres_per_unit_milli = 120;  // 65535 units span ~7.9 km of elevation.
    map.seed = params.seed;
    map.heights.assign(map.pixel_count(), 0);
    map.provinces.assign(map.pixel_count(), 0);
    map.materials.assign(map.pixel_count(), 0);

    auto t = Clock::now();
    generate_heights(map, params, threads);
    PAT_LOG_INFO("mapgen", "heights: {:.0f} ms ({} threads)", elapsed_ms(t), threads);

    t = Clock::now();
    const std::size_t land = keep_main_landmass(map);
    PAT_LOG_INFO("mapgen",
                 "landmass: {:.0f} ms, {} land pixels ({:.1f}%)",
                 elapsed_ms(t),
                 land,
                 100.0 * static_cast<double>(land) / static_cast<double>(map.pixel_count()));

    const std::uint32_t wanted = std::max(1U, std::min<std::uint32_t>(params.province_count, 4000U));
    t = Clock::now();
    const std::vector<Seed> seeds = pick_seeds(map, land, wanted, params.seed);
    assign_provinces(map, seeds, land, seed32, threads);
    const auto count = static_cast<std::uint32_t>(seeds.size());
    make_provinces_connected(map, count);
    PAT_LOG_INFO("mapgen", "provinces: {:.0f} ms, {} provinces", elapsed_ms(t), count);

    t = Clock::now();
    classify_materials(map, params.seed, threads);
    PAT_LOG_INFO("mapgen", "materials: {:.0f} ms", elapsed_ms(t));

    t = Clock::now();
    map.province_list.resize(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        map.province_list[i].id = static_cast<std::uint16_t>(i + 1);
        map.province_list[i].color_rgb = province_color(i + 1);
    }
    rebuild_province_stats(map);
    PAT_LOG_INFO("mapgen",
                 "province table + adjacency: {:.0f} ms; total {:.0f} ms",
                 elapsed_ms(t),
                 elapsed_ms(t_start));
    return map;
}

}  // namespace pat::mapdata
