#include "pat/engine/camera.hpp"

#include <algorithm>
#include <cmath>

namespace pat::engine {

namespace {

constexpr float kZoomStep = 0.86f;  // distance multiplier per wheel notch

float lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

}  // namespace

OrbitCamera::OrbitCamera(const CameraLimits& limits)
    : limits_(limits), focus_{limits.map_width * 0.5f, limits.map_height * 0.5f, 0.0f},
      distance_(limits.max_distance) {}

float OrbitCamera::zoom_fraction() const noexcept {
    const float lo = std::log(limits_.min_distance);
    const float hi = std::log(limits_.max_distance);
    return std::clamp((std::log(distance_) - lo) / (hi - lo), 0.0f, 1.0f);
}

float OrbitCamera::pitch() const noexcept {
    const float automatic = lerp(limits_.near_pitch, limits_.far_pitch, zoom_fraction());
    return std::clamp(automatic + tilt_bias_, limits_.min_pitch, limits_.max_pitch);
}

Vec3 OrbitCamera::eye() const noexcept {
    const float p = pitch();
    const float horizontal = distance_ * std::cos(p);
    // Forward on the ground (the way the camera looks) is (sin yaw, -cos yaw): yaw 0 looks north.
    return {focus_.x - std::sin(yaw_) * horizontal,
            focus_.y + std::cos(yaw_) * horizontal,
            focus_.z + distance_ * std::sin(p)};
}

Mat4 OrbitCamera::view() const noexcept {
    return look_at(eye(), focus_, {0.0f, 0.0f, 1.0f});
}

float OrbitCamera::near_plane() const noexcept {
    return std::max(0.5f, distance_ * 0.02f);
}

float OrbitCamera::far_plane() const noexcept {
    return distance_ + std::max(limits_.map_width, limits_.map_height) * 2.0f;
}

Mat4 OrbitCamera::projection(float aspect) const noexcept {
    return perspective(fov_y_, aspect, near_plane(), far_plane());
}

void OrbitCamera::clamp_focus() noexcept {
    focus_.x = std::clamp(focus_.x, -limits_.margin, limits_.map_width + limits_.margin);
    focus_.y = std::clamp(focus_.y, -limits_.margin, limits_.map_height + limits_.margin);
}

void OrbitCamera::set_focus(float x, float y) {
    focus_.x = x;
    focus_.y = y;
    clamp_focus();
}

void OrbitCamera::set_distance(float d) {
    distance_ = std::clamp(d, limits_.min_distance, limits_.max_distance);
}

void OrbitCamera::drag_pan(float dx, float dy, float viewport_h, float dt) {
    const float world_per_pixel = 2.0f * distance_ * std::tan(fov_y_ * 0.5f) / std::max(viewport_h, 1.0f);
    const float right_x = std::cos(yaw_);
    const float right_y = std::sin(yaw_);
    const float fwd_x = std::sin(yaw_);
    const float fwd_y = -std::cos(yaw_);
    const float foreshortening = 1.0f / std::max(std::sin(pitch()), 0.2f);
    // The ground follows the cursor, so the focus moves the opposite way horizontally and with the
    // cursor vertically (dragging down pulls the map towards the viewer).
    const float mx = -right_x * dx * world_per_pixel + fwd_x * dy * world_per_pixel * foreshortening;
    const float my = -right_y * dx * world_per_pixel + fwd_y * dy * world_per_pixel * foreshortening;
    focus_.x += mx;
    focus_.y += my;
    clamp_focus();
    if (dt > 1e-5f) {
        const float blend = std::min(1.0f, dt * 20.0f);  // smooth the velocity estimate
        velocity_x_ = lerp(velocity_x_, mx / dt, blend);
        velocity_y_ = lerp(velocity_y_, my / dt, blend);
    }
}

void OrbitCamera::move(float right, float forward, float speed, float dt) {
    const float sx = std::cos(yaw_) * right + std::sin(yaw_) * forward;
    const float sy = std::sin(yaw_) * right - std::cos(yaw_) * forward;
    focus_.x += sx * speed * dt;
    focus_.y += sy * speed * dt;
    clamp_focus();
}

void OrbitCamera::rotate(float delta_yaw, float delta_tilt) {
    yaw_ += delta_yaw;
    tilt_bias_ += delta_tilt;
    // Keep the bias reasonable so the automatic tilt stays meaningful.
    tilt_bias_ = std::clamp(tilt_bias_, -1.0f, 1.0f);
}

void OrbitCamera::zoom_towards(Vec3 ground_point, float steps) {
    const float old_distance = distance_;
    distance_ =
        std::clamp(distance_ * std::pow(kZoomStep, steps), limits_.min_distance, limits_.max_distance);
    const float actual = distance_ / old_distance;
    // Keep the point under the cursor roughly fixed on screen by sliding the focus towards it.
    focus_.x += (ground_point.x - focus_.x) * (1.0f - actual);
    focus_.y += (ground_point.y - focus_.y) * (1.0f - actual);
    clamp_focus();
}

void OrbitCamera::update(float dt) {
    if (dragging_) {
        return;
    }
    focus_.x += velocity_x_ * dt;
    focus_.y += velocity_y_ * dt;
    clamp_focus();
    const float damping = std::exp(-5.0f * dt);
    velocity_x_ *= damping;
    velocity_y_ *= damping;
    if (std::fabs(velocity_x_) < 0.01f) {
        velocity_x_ = 0.0f;
    }
    if (std::fabs(velocity_y_) < 0.01f) {
        velocity_y_ = 0.0f;
    }
}

bool make_pick_ray(const Mat4& view_proj,
                   float pixel_x,
                   float pixel_y,
                   float viewport_w,
                   float viewport_h,
                   Vec3& origin,
                   Vec3& direction) {
    Mat4 inverse;
    if (!invert(view_proj, inverse) || viewport_w <= 0.0f || viewport_h <= 0.0f) {
        return false;
    }
    const float ndc_x = 2.0f * pixel_x / viewport_w - 1.0f;
    const float ndc_y = 1.0f - 2.0f * pixel_y / viewport_h;
    const Vec3 near_point = transform_point(inverse, {ndc_x, ndc_y, 0.0f});
    const Vec3 far_point = transform_point(inverse, {ndc_x, ndc_y, 1.0f});
    origin = near_point;
    direction = normalize(far_point - near_point);
    return true;
}

}  // namespace pat::engine
