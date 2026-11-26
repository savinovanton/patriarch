#include "pat/mapdata/geo_projection.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <string>
#include <string_view>

using namespace pat::mapdata;

namespace {

/// GEOR of the stage-1 candidate projections (docs/dev/geodata-pipeline.md section 5).
GeoRef georef(std::string_view kind) {
    GeoRef g;
    g.datum = Datum::wgs84;
    g.lon_central_udeg = 100'000'000;
    g.pixel_size_x_mm = 1'300'000;
    g.pixel_size_y_mm = 1'300'000;
    g.origin_x_mm = -5'324'800'000;
    g.origin_y_mm = 3'342'400'000;
    g.bbox_min_lon_udeg = 15'000'000;
    g.bbox_max_lon_udeg = -165'000'000;
    g.bbox_min_lat_udeg = 28'000'000;
    g.bbox_max_lat_udeg = 82'000'000;
    if (kind == "albers" || kind == "lcc") {
        g.projection = kind == "albers" ? Projection::albers : Projection::lambert_conformal;
        g.lat_origin_udeg = 58'000'000;
        g.lat_std1_udeg = 47'000'000;
        g.lat_std2_udeg = 69'000'000;
    } else if (kind == "laea") {
        g.projection = Projection::lambert_azimuthal;
        g.lat_origin_udeg = 62'000'000;
    } else if (kind == "eqc") {
        g.projection = Projection::equirectangular;
        g.lat_std1_udeg = 50'000'000;
    } else {  // "lcc1": one standard parallel, false easting/northing
        g.projection = Projection::lambert_conformal;
        g.lat_origin_udeg = 58'000'000;
        g.lat_std1_udeg = 58'000'000;
        g.lat_std2_udeg = 58'000'000;
        g.false_easting_mm = 500'000'000;
        g.false_northing_mm = -200'000'000;
    }
    return g;
}

struct Reference {
    const char* projection;
    const char* place;
    double lon, lat, x, y;
};

// PROJ 9.9.0 (pyproj 3.8.0, conda-forge), EPSG:4326 -> the PROJ string of each candidate.
constexpr Reference kReference[] = {
    {"albers", "moscow", 37.6173, 55.7558, -3346896.120, 1375820.342},
    {"albers", "dezhnev", -169.65, 66.08, 2979192.562, 3210691.496},
    {"albers", "rudolf", 58.0, 81.8, -822274.292, 2820921.382},
    {"albers", "vladivostok", 131.9, 43.1, 2532976.881, -1070783.822},
    {"albers", "kaliningrad", 20.5, 54.7, -3998318.739, 2232163.457},
    {"lcc", "moscow", 37.6173, 55.7558, -3318194.351, 1417841.842},
    {"lcc", "dezhnev", -169.65, 66.08, 2934099.343, 3224960.017},
    {"lcc", "rudolf", 58.0, 81.8, -699389.011, 2928119.855},
    {"lcc", "vladivostok", 131.9, 43.1, 2535528.914, -1031721.431},
    {"lcc", "kaliningrad", 20.5, 54.7, -3943051.860, 2291383.420},
    {"laea", "moscow", 37.6173, 55.7558, -3312578.731, 1046575.268},
    {"laea", "dezhnev", -169.65, 66.08, 2730302.059, 2900290.141},
    {"laea", "rudolf", 58.0, 81.8, -622944.541, 2418572.661},
    {"laea", "vladivostok", 131.9, 43.1, 2532044.186, -1484490.963},
    {"laea", "kaliningrad", 20.5, 54.7, -3861885.630, 1969359.399},
    {"eqc", "moscow", 37.6173, 55.7558, -4472574.689, 6181373.881},
    {"eqc", "dezhnev", -169.65, 66.08, 6477711.339, 7331759.822},
    {"eqc", "rudolf", 58.0, 81.8, -3011221.652, 9086138.047},
    {"eqc", "vladivostok", 131.9, 43.1, 2287094.540, 4773829.255},
    {"eqc", "kaliningrad", 20.5, 54.7, -5699812.412, 6063834.084},
    {"lcc1", "moscow", 37.6173, 55.7558, -2885983.556, 1234659.883},
    {"lcc1", "dezhnev", -169.65, 66.08, 3507561.905, 3079801.767},
    {"lcc1", "rudolf", 58.0, 81.8, -218594.816, 2792075.250},
    {"lcc1", "vladivostok", 131.9, 43.1, 3078728.520, -1254413.736},
    {"lcc1", "kaliningrad", 20.5, 54.7, -3528341.293, 2119929.603},
};

}  // namespace

TEST_CASE("geo projection: forward and inverse match PROJ") {
    for (const Reference& r : kReference) {
        const std::string what = std::string(r.projection) + " " + r.place;
        CAPTURE(what);
        const auto proj = GeoProjection::create(georef(r.projection));
        REQUIRE(proj.has_value());
        const PlanePoint p = proj.value().forward({r.lon, r.lat});
        CHECK(std::abs(p.x_m - r.x) < 0.01);  // centimetres over thousands of kilometres
        CHECK(std::abs(p.y_m - r.y) < 0.01);
        const LonLat back = proj.value().inverse({r.x, r.y});
        CHECK(std::abs(back.lon_deg - r.lon) < 1e-7);  // the reference plane is rounded to 1 mm
        CHECK(std::abs(back.lat_deg - r.lat) < 1e-7);
    }
}

TEST_CASE("geo projection: pixel centres and the antimeridian") {
    const auto proj = GeoProjection::create(georef("lcc"));
    REQUIRE(proj.has_value());
    const PlanePoint c = proj.value().pixel_center(0, 0);
    CHECK(c.x_m == doctest::Approx(-5'324'800.0 + 650.0));
    CHECK(c.y_m == doctest::Approx(3'342'400.0 - 650.0));
    // Chukotka east of 180 E comes back as a western longitude in [-180, 180).
    const PlanePoint d = proj.value().forward({-169.65, 66.08});
    const LonLat back = proj.value().inverse(d);
    CHECK(back.lon_deg == doctest::Approx(-169.65).epsilon(1e-9));
    const LonLat east = proj.value().inverse(proj.value().forward({179.9, 66.0}));
    CHECK(east.lon_deg == doctest::Approx(179.9).epsilon(1e-9));
    // The canvas centre column is the central meridian.
    const LonLat mid = proj.value().pixel_lonlat(4096, 2048);
    CHECK(std::abs(mid.lon_deg - 100.0) < 0.02);  // the pixel centre is 650 m east of it
}

TEST_CASE("geo projection: degenerate parameters are rejected") {
    CHECK_FALSE(GeoProjection::create(GeoRef{}).has_value());
    GeoRef g = georef("lcc");
    g.lat_std1_udeg = 40'000'000;
    g.lat_std2_udeg = -40'000'000;  // the cone opens to neither pole
    CHECK_FALSE(GeoProjection::create(g).has_value());
    g = georef("albers");
    g.lat_std1_udeg = 30'000'000;
    g.lat_std2_udeg = -30'000'000;
    CHECK_FALSE(GeoProjection::create(g).has_value());
    g = georef("laea");
    g.lat_origin_udeg = 90'000'000;
    CHECK_FALSE(GeoProjection::create(g).has_value());
}
