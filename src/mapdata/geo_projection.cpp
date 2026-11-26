#include "pat/mapdata/geo_projection.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "layer_codec.hpp"

namespace pat::mapdata {

namespace {

constexpr double kA = 6378137.0;            // WGS84 semi-major axis, metres
constexpr double kF = 1.0 / 298.257223563;  // WGS84 flattening
constexpr double kE2 = kF * (2.0 - kF);     // first eccentricity squared
const double kE = std::sqrt(kE2);           // first eccentricity
constexpr double kPi = std::numbers::pi;
constexpr double kDeg = kPi / 180.0;

double udeg_to_rad(std::int32_t udeg) {
    return static_cast<double>(udeg) * 1e-6 * kDeg;
}

/// Longitude difference wrapped to [-pi, pi).
double wrap_pi(double a) {
    a = std::fmod(a + kPi, 2.0 * kPi);
    if (a < 0.0) {
        a += 2.0 * kPi;
    }
    return a - kPi;
}

double m_of(double phi) {  // Snyder 14-15
    const double s = std::sin(phi);
    return std::cos(phi) / std::sqrt(1.0 - kE2 * s * s);
}

double t_of(double phi) {  // Snyder 15-9
    const double s = std::sin(phi);
    return std::tan(kPi / 4.0 - phi / 2.0) / std::pow((1.0 - kE * s) / (1.0 + kE * s), kE / 2.0);
}

double q_of(double phi) {  // Snyder 3-12
    const double s = std::sin(phi);
    return (1.0 - kE2) *
           (s / (1.0 - kE2 * s * s) - (1.0 / (2.0 * kE)) * std::log((1.0 - kE * s) / (1.0 + kE * s)));
}

/// Latitude from q (inverse of q_of), Snyder 3-16 by iteration.
double phi_from_q(double q) {
    const double qp = q_of(kPi / 2.0);
    if (std::abs(q) >= qp - 1e-12) {
        return q > 0.0 ? kPi / 2.0 : -kPi / 2.0;
    }
    double phi = std::asin(q / 2.0);
    for (int k = 0; k < 20; ++k) {
        const double s = std::sin(phi);
        const double c = std::cos(phi);
        const double one = 1.0 - kE2 * s * s;
        const double next = phi + one * one / (2.0 * c) *
                                      (q / (1.0 - kE2) - s / one +
                                       (1.0 / (2.0 * kE)) * std::log((1.0 - kE * s) / (1.0 + kE * s)));
        if (std::abs(next - phi) < 1e-14) {
            return next;
        }
        phi = next;
    }
    return phi;
}

/// Latitude from t (inverse of t_of), Snyder 7-9 by iteration.
double phi_from_t(double t) {
    double phi = kPi / 2.0 - 2.0 * std::atan(t);
    for (int k = 0; k < 20; ++k) {
        const double s = std::sin(phi);
        const double next =
            kPi / 2.0 - 2.0 * std::atan(t * std::pow((1.0 - kE * s) / (1.0 + kE * s), kE / 2.0));
        if (std::abs(next - phi) < 1e-14) {
            return next;
        }
        phi = next;
    }
    return phi;
}

double normalise_lon_deg(double lon_rad) {
    return wrap_pi(lon_rad) / kDeg;
}

/// Meridian arc length from the equator divided by a (the series of PROJ's pj_mlfn, to e^8).
double meridian_arc(double phi) {
    const double es = kE2;
    const double en0 = 1.0 - es * (0.25 + es * (0.046875 + es * (0.01953125 + es * 0.01068115234375)));
    const double en1 = es * (0.75 - es * (0.046875 + es * (0.01953125 + es * 0.01068115234375)));
    const double t2 = es * es;
    const double en2 = t2 * (0.46875 - es * (0.01302083333333333333 + es * 0.00712076822916666666));
    const double en3 = t2 * es * (0.36458333333333333333 - es * 0.00569661458333333333);
    const double en4 = t2 * es * es * 0.3076171875;
    const double s = std::sin(phi);
    const double cs = std::cos(phi) * s;
    const double s2 = s * s;
    return en0 * phi - cs * (en1 + s2 * (en2 + s2 * (en3 + s2 * en4)));
}

/// Latitude whose meridian_arc is `arc` (Newton iteration).
double phi_from_arc(double arc) {
    double phi = arc;
    for (int k = 0; k < 20; ++k) {
        const double s = std::sin(phi);
        const double t = 1.0 - kE2 * s * s;
        const double step = (meridian_arc(phi) - arc) * t * std::sqrt(t) / (1.0 - kE2);
        phi -= step;
        if (std::abs(step) < 1e-14) {
            break;
        }
    }
    return phi;
}

}  // namespace

core::Result<GeoProjection, core::Error> GeoProjection::create(const GeoRef& g) {
    using R = core::Result<GeoProjection, core::Error>;
    const auto bad = [](const char* m) {
        return R::err(codec::make_error(physical_error::kBadGeoref, std::string("projection: ") + m));
    };
    if (g.projection == Projection::none || g.datum != Datum::wgs84) {
        return bad("no projection (or no datum) to compute with");
    }
    GeoProjection p;
    p.kind_ = g.projection;
    p.lon0_ = udeg_to_rad(g.lon_central_udeg);
    p.lat0_ = udeg_to_rad(g.lat_origin_udeg);
    p.x0_ = static_cast<double>(g.false_easting_mm) / 1000.0;
    p.y0_ = static_cast<double>(g.false_northing_mm) / 1000.0;
    p.origin_x_ = static_cast<double>(g.origin_x_mm) / 1000.0;
    p.origin_y_ = static_cast<double>(g.origin_y_mm) / 1000.0;
    p.pixel_x_ = static_cast<double>(g.pixel_size_x_mm) / 1000.0;
    p.pixel_y_ = static_cast<double>(g.pixel_size_y_mm) / 1000.0;
    const double phi1 = udeg_to_rad(g.lat_std1_udeg);
    const double phi2 = udeg_to_rad(g.lat_std2_udeg);

    switch (g.projection) {
        case Projection::albers: {
            const double m1 = m_of(phi1);
            const double m2 = m_of(phi2);
            const double q1 = q_of(phi1);
            const double q2 = q_of(phi2);
            p.n_ = g.lat_std1_udeg == g.lat_std2_udeg ? std::sin(phi1) : (m1 * m1 - m2 * m2) / (q2 - q1);
            if (std::abs(p.n_) < 1e-10) {
                return bad("degenerate Albers standard parallels");
            }
            p.c_ = m1 * m1 + p.n_ * q1;
            p.rho0_ = kA * std::sqrt(std::max(0.0, p.c_ - p.n_ * q_of(p.lat0_))) / p.n_;
            break;
        }
        case Projection::lambert_conformal: {
            const double m1 = m_of(phi1);
            const double t1 = t_of(phi1);
            if (g.lat_std1_udeg == g.lat_std2_udeg) {
                p.n_ = std::sin(phi1);
            } else {
                p.n_ = (std::log(m1) - std::log(m_of(phi2))) / (std::log(t1) - std::log(t_of(phi2)));
            }
            if (std::abs(p.n_) < 1e-10 || !std::isfinite(p.n_)) {
                return bad("degenerate Lambert standard parallels");
            }
            p.c_ = m1 / (p.n_ * std::pow(t1, p.n_));  // F
            p.rho0_ = kA * p.c_ * std::pow(t_of(p.lat0_), p.n_);
            break;
        }
        case Projection::lambert_azimuthal: {
            p.qp_ = q_of(kPi / 2.0);
            p.rq_ = kA * std::sqrt(p.qp_ / 2.0);
            const double beta1 = std::asin(q_of(p.lat0_) / p.qp_);
            p.sin_b1_ = std::sin(beta1);
            p.cos_b1_ = std::cos(beta1);
            if (p.cos_b1_ < 1e-12) {
                return bad("polar aspect of the azimuthal projection is not supported");
            }
            p.d_ = kA * m_of(p.lat0_) / (p.rq_ * p.cos_b1_);
            break;
        }
        case Projection::equirectangular:
            // Ellipsoidal form as in PROJ: x scaled by the parallel radius at lat_ts, y = meridian arc.
            p.cos_ts_ = std::cos(phi1) / std::sqrt(1.0 - kE2 * std::sin(phi1) * std::sin(phi1));
            p.c_ = meridian_arc(p.lat0_);
            if (std::cos(phi1) < 1e-12) {
                return bad("degenerate equirectangular standard parallel");
            }
            break;
        default:
            return bad("unknown projection");
    }
    return R::ok(p);
}

PlanePoint GeoProjection::forward(LonLat point) const noexcept {
    const double phi = point.lat_deg * kDeg;
    const double dlon = wrap_pi(point.lon_deg * kDeg - lon0_);
    double x = 0.0;
    double y = 0.0;
    switch (kind_) {
        case Projection::albers:
        case Projection::lambert_conformal: {
            const double rho = kind_ == Projection::albers
                                   ? kA * std::sqrt(std::max(0.0, c_ - n_ * q_of(phi))) / n_
                                   : kA * c_ * std::pow(t_of(phi), n_);
            const double theta = n_ * dlon;
            x = rho * std::sin(theta);
            y = rho0_ - rho * std::cos(theta);
            break;
        }
        case Projection::lambert_azimuthal: {
            const double beta = std::asin(q_of(phi) / qp_);
            const double sb = std::sin(beta);
            const double cb = std::cos(beta);
            const double b = rq_ * std::sqrt(2.0 / (1.0 + sin_b1_ * sb + cos_b1_ * cb * std::cos(dlon)));
            x = b * d_ * cb * std::sin(dlon);
            y = (b / d_) * (cos_b1_ * sb - sin_b1_ * cb * std::cos(dlon));
            break;
        }
        case Projection::equirectangular:
            x = kA * dlon * cos_ts_;
            y = kA * (meridian_arc(phi) - c_);
            break;
        default:
            break;
    }
    return {x + x0_, y + y0_};
}

LonLat GeoProjection::inverse(PlanePoint point) const noexcept {
    const double x = point.x_m - x0_;
    const double y = point.y_m - y0_;
    double lon = lon0_;
    double phi = lat0_;
    switch (kind_) {
        case Projection::albers:
        case Projection::lambert_conformal: {
            const double sign = n_ < 0.0 ? -1.0 : 1.0;
            const double dx = sign * x;
            const double dy = sign * (rho0_ - y);
            const double rho = sign * std::sqrt(dx * dx + dy * dy);
            const double theta = std::atan2(dx, dy);
            lon = lon0_ + theta / n_;
            if (kind_ == Projection::albers) {
                phi = phi_from_q((c_ - rho * rho * n_ * n_ / (kA * kA)) / n_);
            } else if (rho == 0.0) {
                phi = sign * kPi / 2.0;
            } else {
                phi = phi_from_t(std::pow(rho / (kA * c_), 1.0 / n_));
            }
            break;
        }
        case Projection::lambert_azimuthal: {
            const double rho = std::sqrt((x / d_) * (x / d_) + (d_ * y) * (d_ * y));
            if (rho < 1e-9) {
                break;
            }
            const double ce = 2.0 * std::asin(std::min(1.0, rho / (2.0 * rq_)));
            const double sc = std::sin(ce);
            const double cc = std::cos(ce);
            phi = phi_from_q(qp_ * (cc * sin_b1_ + d_ * y * sc * cos_b1_ / rho));
            lon = lon0_ + std::atan2(x * sc, d_ * rho * cos_b1_ * cc - d_ * d_ * y * sin_b1_ * sc);
            break;
        }
        case Projection::equirectangular:
            lon = lon0_ + x / (kA * cos_ts_);
            phi = phi_from_arc(y / kA + c_);
            break;
        default:
            break;
    }
    return {normalise_lon_deg(lon), phi / kDeg};
}

PlanePoint GeoProjection::pixel_center(std::uint32_t x, std::uint32_t y) const noexcept {
    return {origin_x_ + (static_cast<double>(x) + 0.5) * pixel_x_,
            origin_y_ - (static_cast<double>(y) + 0.5) * pixel_y_};
}

}  // namespace pat::mapdata
