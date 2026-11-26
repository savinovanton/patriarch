#include "pat/mapdata/map_migrate.hpp"

#include "pat/core/hash_stream.hpp"
#include "pat/mapdata/map_format.hpp"
#include "pat/mapdata/map_validate.hpp"

#include <array>
#include <cstdio>
#include <fstream>
#include <limits>
#include <string>
#include <system_error>

#include "layer_codec.hpp"
#include "raster_label.hpp"

namespace pat::mapdata {

namespace {

using core::Error;
using core::Result;

/// Labels the 4-connected components of "height <= sea_level" in raster order of their first pixel.
/// Returns the number of components (0 if there is no water); `overflow` is set if there are more
/// than a u16 can number.
std::size_t label_water(const MapData& map, std::vector<std::uint16_t>& label, bool& overflow) {
    std::vector<std::uint8_t> water(map.pixel_count());
    for (std::size_t i = 0; i < water.size(); ++i) {
        water[i] = map.heights[i] <= map.sea_level ? 1 : 0;
    }
    return raster::label_components(map.width, water, label, overflow);
}

}  // namespace

std::string generated_province_key(std::uint16_t id) {
    char text[16];
    std::snprintf(text, sizeof text, "p%04u", static_cast<unsigned>(id));
    return text;
}

Result<MigratedMap, Error> migrate_pmd1(const MapData& map) {
    using R = Result<MigratedMap, Error>;
    if (auto ok = validate_map(map); !ok) {
        return R::err(
            codec::make_error(migrate_error::kInvalidSource, "PMD1 map is invalid: " + ok.error().message));
    }

    MigratedMap out;
    PhysicalLayer& l = out.physical;
    l.width = map.width;
    l.height = map.height;
    l.sea_level = map.sea_level;
    l.metres_per_unit_milli = map.metres_per_unit_milli;
    l.seed = map.seed;
    l.material_version = kMaterialSetVersion;
    l.heights = map.heights;
    l.materials = map.materials;

    bool overflow = false;
    const std::size_t seas = label_water(map, l.water_body, overflow);
    if (overflow) {
        return R::err(codec::make_error(migrate_error::kTooManyWaters,
                                        "the map has more than 65535 separate water bodies"));
    }
    const std::vector<std::uint32_t> area = count_water_area(l.water_body, seas);
    for (std::size_t k = 0; k < seas; ++k) {
        WaterBody b;
        b.id = static_cast<std::uint16_t>(k + 1);
        b.kind = WaterKind::sea;
        b.surface_level = map.sea_level;
        b.area_px = area[k];
        b.key = k == 0 ? "sea" : "sea-" + std::to_string(k + 1);
        l.water_bodies.push_back(std::move(b));
    }

    core::HashStream params;
    for (const std::uint64_t h : pmd1_section_hashes(map)) {
        params.u64(h);
    }
    Provenance prov;
    prov.builder = "migrate_pmd1";
    prov.steps.push_back({kMigrationStepId, params.value()});
    l.provenance = std::move(prov);

    ProvinceLayer& p = out.provinces;
    p.width = map.width;
    p.height = map.height;
    p.bind_key = coast_key(l);
    p.province_id = map.provinces;
    p.entries.reserve(map.province_list.size());
    for (const Province& src : map.province_list) {
        ProvinceEntry e;
        e.id = src.id;
        e.key = generated_province_key(src.id);
        e.color_rgb = src.color_rgb;
        e.area_px = src.area_px;
        e.min_x = src.min_x;
        e.min_y = src.min_y;
        e.max_x = src.max_x;
        e.max_y = src.max_y;
        e.centroid_x = src.centroid_x;
        e.centroid_y = src.centroid_y;
        e.coastal = src.coastal;
        e.neighbors = src.neighbors;
        p.entries.push_back(std::move(e));
    }

    // Everything above is derived from a valid map, so these only fail on a bug in this function; the
    // checks keep "never a silently altered map" true.
    if (auto ok = validate_physical(l); !ok) {
        return R::err(ok.error());
    }
    if (auto ok = validate_provinces(l, p); !ok) {
        return R::err(ok.error());
    }
    return R::ok(std::move(out));
}

Result<void, Error> write_map_set(const std::filesystem::path& directory,
                                  const MigratedMap& layers,
                                  const Pmd2WriterOptions& options) {
    using R = Result<void, Error>;
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) {
        return R::err(codec::make_error(migrate_error::kIo, "cannot create " + directory.string()));
    }
    if (auto ok = write_physical_file(directory / kPhysicalFileName, layers.physical, options); !ok) {
        return ok;
    }
    return write_provinces_file(directory / kProvincesFileName, layers.provinces, options);
}

Result<MapData, Error> load_map(const std::filesystem::path& path, const ProvinceReadOptions& options) {
    using R = Result<MapData, Error>;
    // magic (4) | u16 minor | u16 kind: all that is needed to choose the path.
    std::array<std::uint8_t, 8> head{};
    std::size_t got = 0;
    {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            return R::err(codec::make_error(migrate_error::kIo, "cannot open " + path.string()));
        }
        in.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head.size()));
        got = static_cast<std::size_t>(in.gcount());
    }
    const auto is_magic = [&](const char* m) {
        return got >= 4 && head[0] == static_cast<std::uint8_t>(m[0]) &&
               head[1] == static_cast<std::uint8_t>(m[1]) && head[2] == static_cast<std::uint8_t>(m[2]) &&
               head[3] == static_cast<std::uint8_t>(m[3]);
    };
    if (is_magic("PMD1")) {
        return read_map_file(path);
    }
    if (!is_magic("PMD2") || got < head.size()) {
        return R::err(
            codec::make_error(migrate_error::kUnknownFormat, "not a Patriarch map file: " + path.string()));
    }
    const auto kind = static_cast<std::uint16_t>(head[6] | (head[7] << 8U));
    const std::filesystem::path dir = path.parent_path();
    std::filesystem::path physical;
    std::filesystem::path provinces;
    if (kind == static_cast<std::uint16_t>(Pmd2Kind::physical)) {
        physical = path;
        provinces = dir / kProvincesFileName;
    } else if (kind == static_cast<std::uint16_t>(Pmd2Kind::provinces)) {
        physical = dir / kPhysicalFileName;
        provinces = path;
    } else {
        return R::err(codec::make_error(migrate_error::kUnsupportedKind,
                                        "a PMD2 file of kind " + std::to_string(kind) +
                                            " is not a map layer: " + path.string()));
    }
    for (const std::filesystem::path* file : {&physical, &provinces}) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(*file, ec)) {
            return R::err(codec::make_error(migrate_error::kMissingSibling,
                                            "the map set needs " + file->string() + " next to the file"));
        }
    }
    return load_map_set(physical, provinces, options);
}

}  // namespace pat::mapdata
