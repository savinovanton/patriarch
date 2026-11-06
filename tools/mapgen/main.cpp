// mapgen - procedural test map generator and map file inspector (E3-T03, ADR-0003).
//
//   mapgen --out map.pmd [--width 8192 --height 4096 --seed 1 --provinces 400 --threads 0]
//          [--preview map.png] [--no-validate]
//   mapgen --info map.pmd [--preview map.png]
//
// Exit codes: 0 ok, 1 generation/IO failure, 2 usage error, 3 validation failure.

#include "pat/core/log.hpp"
#include "pat/mapdata/map_format.hpp"
#include "pat/mapdata/map_gen.hpp"
#include "pat/mapdata/map_validate.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "png_writer.hpp"

namespace {

using namespace pat::mapdata;

struct Options {
    GenParams gen;
    std::string out;
    std::string info;
    std::string preview;
    bool validate = true;
};

bool parse(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        const char* v = nullptr;
        if (a == "--out" && (v = next()) != nullptr) {
            o.out = v;
        } else if (a == "--info" && (v = next()) != nullptr) {
            o.info = v;
        } else if (a == "--preview" && (v = next()) != nullptr) {
            o.preview = v;
        } else if (a == "--width" && (v = next()) != nullptr) {
            o.gen.width = static_cast<std::uint32_t>(std::strtoul(v, nullptr, 10));
        } else if (a == "--height" && (v = next()) != nullptr) {
            o.gen.height = static_cast<std::uint32_t>(std::strtoul(v, nullptr, 10));
        } else if (a == "--seed" && (v = next()) != nullptr) {
            o.gen.seed = std::strtoull(v, nullptr, 10);
        } else if (a == "--provinces" && (v = next()) != nullptr) {
            o.gen.province_count = static_cast<std::uint32_t>(std::strtoul(v, nullptr, 10));
        } else if (a == "--threads" && (v = next()) != nullptr) {
            o.gen.threads = static_cast<unsigned>(std::strtoul(v, nullptr, 10));
        } else if (a == "--no-validate") {
            o.validate = false;
        } else {
            return false;
        }
    }
    const bool generating = !o.out.empty();
    const bool inspecting = !o.info.empty();
    if (generating == inspecting) {  // exactly one mode
        return false;
    }
    return o.gen.width >= 64 && o.gen.height >= 32 && o.gen.width <= 16384 && o.gen.height <= 16384 &&
           o.gen.province_count >= 1;
}

void usage() {
    std::fprintf(stderr,
                 "usage: mapgen --out FILE [--width N --height N --seed N --provinces N --threads N]\n"
                 "              [--preview FILE.png] [--no-validate]\n"
                 "       mapgen --info FILE [--preview FILE.png]\n");
}

void print_stats(const MapData& map) {
    std::size_t land = 0;
    for (const std::uint16_t p : map.provinces) {
        land += p != 0 ? 1U : 0U;
    }
    std::vector<std::uint32_t> areas;
    std::size_t edges = 0;
    std::size_t coastal = 0;
    for (const Province& p : map.province_list) {
        areas.push_back(p.area_px);
        edges += p.neighbors.size();
        coastal += p.coastal ? 1U : 0U;
    }
    std::sort(areas.begin(), areas.end());
    std::printf("map          : %u x %u px, seed %llu, sea level %u\n",
                map.width,
                map.height,
                static_cast<unsigned long long>(map.seed),
                map.sea_level);
    std::printf("land         : %zu px (%.1f%%)\n",
                land,
                100.0 * static_cast<double>(land) / static_cast<double>(map.pixel_count()));
    if (!areas.empty()) {
        std::printf("provinces    : %zu (area px min %u / median %u / max %u), %zu coastal\n",
                    areas.size(),
                    areas.front(),
                    areas[areas.size() / 2],
                    areas.back(),
                    coastal);
        std::printf("graph        : %zu edges, average degree %.2f, %zu reachable from province 1\n",
                    edges / 2,
                    static_cast<double>(edges) / static_cast<double>(areas.size()),
                    reachable_province_count(map));
    }
}

struct Rgb {
    float r, g, b;
};

Rgb material_color(Material m) {
    switch (m) {
        case Material::DeepWater:
            return {0.05f, 0.14f, 0.30f};
        case Material::ShallowWater:
            return {0.12f, 0.30f, 0.46f};
        case Material::Beach:
            return {0.80f, 0.74f, 0.55f};
        case Material::Plains:
            return {0.45f, 0.58f, 0.28f};
        case Material::Steppe:
            return {0.66f, 0.62f, 0.34f};
        case Material::Forest:
            return {0.14f, 0.34f, 0.16f};
        case Material::Hills:
            return {0.48f, 0.42f, 0.28f};
        case Material::Rock:
            return {0.46f, 0.43f, 0.40f};
        case Material::Snow:
            return {0.94f, 0.95f, 0.98f};
        case Material::Tundra:
            return {0.60f, 0.64f, 0.56f};
        default:
            return {1.0f, 0.0f, 1.0f};
    }
}

/// Writes a shaded preview: terrain materials with a hill-shade, province borders drawn on top.
bool write_preview(const MapData& map, const std::string& path) {
    const std::uint32_t pw = std::min<std::uint32_t>(map.width, 2048);
    const std::uint32_t ph = std::max<std::uint32_t>(
        1, static_cast<std::uint32_t>(static_cast<std::uint64_t>(map.height) * pw / map.width));
    const double step = static_cast<double>(map.width) / pw;
    std::vector<std::uint8_t> rgb(static_cast<std::size_t>(pw) * ph * 3);

    const auto sample = [&](std::int64_t x, std::int64_t y) {
        x = std::clamp<std::int64_t>(x, 0, map.width - 1);
        y = std::clamp<std::int64_t>(y, 0, map.height - 1);
        return static_cast<std::size_t>(y) * map.width + static_cast<std::size_t>(x);
    };
    const double exaggeration = 40.0 / std::max(1.0, step);

    for (std::uint32_t py = 0; py < ph; ++py) {
        for (std::uint32_t px = 0; px < pw; ++px) {
            const auto sx = static_cast<std::int64_t>((px + 0.5) * step);
            const auto sy = static_cast<std::int64_t>((py + 0.5) * step);
            const std::size_t i = sample(sx, sy);
            const auto istep = static_cast<std::int64_t>(std::max(1.0, step));
            const double dzdx = (static_cast<double>(map.heights[sample(sx + istep, sy)]) -
                                 static_cast<double>(map.heights[sample(sx - istep, sy)])) /
                                (2.0 * istep);
            const double dzdy = (static_cast<double>(map.heights[sample(sx, sy + istep)]) -
                                 static_cast<double>(map.heights[sample(sx, sy - istep)])) /
                                (2.0 * istep);
            // Light from the north-west.
            const double nx = -dzdx * exaggeration / 65535.0 * 60.0;
            const double ny = -dzdy * exaggeration / 65535.0 * 60.0;
            const double inv = 1.0 / std::sqrt(nx * nx + ny * ny + 1.0);
            const double shade =
                std::clamp(0.75 + 0.9 * ((-nx * 0.55 - ny * 0.55 + 0.63) * inv - 0.63), 0.35, 1.25);

            Rgb c = material_color(static_cast<Material>(map.materials[i]));
            const std::uint16_t prov = map.provinces[i];
            if (prov != 0) {
                const std::uint32_t pc = map.province_list[prov - 1U].color_rgb;
                const Rgb pr{static_cast<float>((pc >> 16U) & 0xFFU) / 255.0f,
                             static_cast<float>((pc >> 8U) & 0xFFU) / 255.0f,
                             static_cast<float>(pc & 0xFFU) / 255.0f};
                c = {c.r * 0.55f + pr.r * 0.45f, c.g * 0.55f + pr.g * 0.45f, c.b * 0.55f + pr.b * 0.45f};
                // Province border: a different province to the right or below.
                const std::size_t right = sample(sx + istep, sy);
                const std::size_t below = sample(sx, sy + istep);
                if (map.provinces[right] != prov || map.provinces[below] != prov) {
                    c = {c.r * 0.35f, c.g * 0.35f, c.b * 0.35f};
                }
            }
            const auto out = [&](float v) {
                return static_cast<std::uint8_t>(std::clamp(v * static_cast<float>(shade), 0.0f, 1.0f) *
                                                 255.0f);
            };
            const std::size_t o = (static_cast<std::size_t>(py) * pw + px) * 3;
            rgb[o] = out(c.r);
            rgb[o + 1] = out(c.g);
            rgb[o + 2] = out(c.b);
        }
    }
    return write_png_rgb(path, pw, ph, rgb);
}

double seconds_since(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parse(argc, argv, opt)) {
        usage();
        return 2;
    }

    MapData map;
    if (!opt.info.empty()) {
        auto t = std::chrono::steady_clock::now();
        auto loaded = read_map_file(opt.info);
        if (!loaded) {
            std::fprintf(stderr, "error: %s\n", loaded.error().message.c_str());
            return 1;
        }
        map = std::move(loaded).value();
        std::printf("loaded %s in %.2f s\n", opt.info.c_str(), seconds_since(t));
    } else {
        auto t = std::chrono::steady_clock::now();
        map = generate_map(opt.gen);
        std::printf("generated in %.2f s\n", seconds_since(t));
        t = std::chrono::steady_clock::now();
        auto written = write_map_file(opt.out, map);
        if (!written) {
            std::fprintf(stderr, "error: %s\n", written.error().message.c_str());
            return 1;
        }
        std::printf("wrote %s (%.1f MB) in %.2f s\n",
                    opt.out.c_str(),
                    static_cast<double>(std::filesystem::file_size(opt.out)) / (1024.0 * 1024.0),
                    seconds_since(t));
    }

    print_stats(map);
    if (opt.validate || !opt.info.empty()) {
        const auto t = std::chrono::steady_clock::now();
        const auto valid = validate_map(map);
        if (!valid) {
            std::fprintf(stderr, "VALIDATION FAILED: %s\n", valid.error().message.c_str());
            return 3;
        }
        std::printf("validation   : OK (%.2f s)\n", seconds_since(t));
    }
    if (!opt.preview.empty()) {
        if (!write_preview(map, opt.preview)) {
            std::fprintf(stderr, "error: cannot write preview %s\n", opt.preview.c_str());
            return 1;
        }
        std::printf("preview      : %s\n", opt.preview.c_str());
    }
    return 0;
}
