#pragma once

#include "pat/core/result.hpp"
#include "pat/mapdata/physical_layer.hpp"

#include <cstdint>

// Forward and inverse map projections of GEOR (ADR-0007 section 3.3) on the WGS84 ellipsoid, without
// PROJ: Albers equal-area conic, Lambert conformal conic (one or two standard parallels), Lambert
// azimuthal equal-area and the equidistant cylindrical projection. Formulas follow Snyder, "Map
// Projections - A Working Manual" (USGS PP 1395, 1987); results match PROJ to well below a metre.
//
// Floating point is fine here: this is for tools and the UI (latitude/longitude under the cursor,
// material classification in `mapgen import`), never for the simulation.

namespace pat::mapdata {

struct LonLat {
    double lon_deg = 0.0;  ///< Normalised to [-180, 180).
    double lat_deg = 0.0;
};

struct PlanePoint {
    double x_m = 0.0;  ///< Projection plane, metres, false easting/northing included.
    double y_m = 0.0;
};

class GeoProjection {
public:
    /// Fails (physical_error::kBadGeoref) for projection "none" and degenerate parameters
    /// (for example conic standard parallels that cancel out).
    [[nodiscard]] static core::Result<GeoProjection, core::Error> create(const GeoRef& georef);

    [[nodiscard]] PlanePoint forward(LonLat point) const noexcept;
    [[nodiscard]] LonLat inverse(PlanePoint point) const noexcept;

    /// Centre of pixel (x, y) of the raster described by the georeference, in the plane.
    [[nodiscard]] PlanePoint pixel_center(std::uint32_t x, std::uint32_t y) const noexcept;
    /// Longitude and latitude of the centre of pixel (x, y).
    [[nodiscard]] LonLat pixel_lonlat(std::uint32_t x, std::uint32_t y) const noexcept {
        return inverse(pixel_center(x, y));
    }

private:
    GeoProjection() = default;

    Projection kind_ = Projection::none;
    double lon0_ = 0.0;  // radians
    double lat0_ = 0.0;
    double x0_ = 0.0;  // false easting / northing, metres
    double y0_ = 0.0;
    double origin_x_ = 0.0;  // raster origin and pixel size, metres
    double origin_y_ = 0.0;
    double pixel_x_ = 0.0;
    double pixel_y_ = 0.0;
    // Projection constants (meaning depends on the kind; see the .cpp).
    double n_ = 0.0;
    double c_ = 0.0;
    double rho0_ = 0.0;
    double qp_ = 0.0;
    double rq_ = 0.0;
    double d_ = 0.0;
    double sin_b1_ = 0.0;
    double cos_b1_ = 0.0;
    double cos_ts_ = 1.0;
};

}  // namespace pat::mapdata
