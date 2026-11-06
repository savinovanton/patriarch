#pragma once

#include "pat/engine/math.hpp"

namespace pat::engine {

struct CameraLimits {
    float map_width = 1000.0f;  ///< The focus point is kept inside the map (plus `margin`).
    float map_height = 500.0f;
    float margin = 0.0f;
    float min_distance = 40.0f;  ///< Closest zoom: eye-to-focus distance.
    float max_distance = 4000.0f;
    float far_pitch = 1.30f;   ///< Pitch (radians above the horizon) when fully zoomed out (almost top-down).
    float near_pitch = 0.62f;  ///< Pitch when fully zoomed in.
    float min_pitch = 0.20f;   ///< Hard limits for the player's manual tilt.
    float max_pitch = 1.50f;
};

/// Orbit camera around a focus point on the ground. Zooming moves towards the cursor and automatically
/// tilts the view (almost top-down when far, oblique when close); the player can add a tilt bias and
/// rotate freely. Panning has inertia. Everything is in world units (see terrain.hpp).
class OrbitCamera {
public:
    explicit OrbitCamera(const CameraLimits& limits);

    [[nodiscard]] Vec3 focus() const noexcept { return focus_; }
    [[nodiscard]] float distance() const noexcept { return distance_; }
    [[nodiscard]] float yaw() const noexcept { return yaw_; }
    [[nodiscard]] float pitch() const noexcept;
    [[nodiscard]] float fov_y() const noexcept { return fov_y_; }
    /// 0 = fully zoomed in, 1 = fully zoomed out (logarithmic).
    [[nodiscard]] float zoom_fraction() const noexcept;

    [[nodiscard]] Vec3 eye() const noexcept;
    [[nodiscard]] Mat4 view() const noexcept;
    [[nodiscard]] Mat4 projection(float aspect) const noexcept;
    [[nodiscard]] Mat4 view_projection(float aspect) const noexcept { return projection(aspect) * view(); }
    [[nodiscard]] float near_plane() const noexcept;
    [[nodiscard]] float far_plane() const noexcept;

    void set_focus(float x, float y);
    /// Height of the ground below the focus; keeps the orbit centre on the terrain.
    void set_focus_height(float z) noexcept { focus_.z = z; }
    void set_distance(float d);
    void set_yaw(float yaw) noexcept { yaw_ = yaw; }

    /// Drags the ground with the cursor. `dt` (seconds) feeds the inertia estimate.
    void drag_pan(float dx_pixels, float dy_pixels, float viewport_height_pixels, float dt);
    /// Call when the pan drag ends: the camera keeps gliding and slows down.
    void release_pan() noexcept { dragging_ = false; }
    void begin_pan() noexcept {
        dragging_ = true;
        velocity_x_ = velocity_y_ = 0.0f;
    }
    /// Keyboard/edge scrolling in screen directions (right, forward) at `speed` world units per second.
    void move(float right, float forward, float speed, float dt);

    void rotate(float delta_yaw, float delta_tilt);
    /// Zooms by `steps` wheel notches (positive = closer) keeping `ground_point` under the cursor.
    void zoom_towards(Vec3 ground_point, float steps);

    /// Advances inertia.
    void update(float dt);

private:
    void clamp_focus() noexcept;

    CameraLimits limits_;
    Vec3 focus_;
    float distance_;
    float yaw_ = 0.0f;
    float tilt_bias_ = 0.0f;
    float fov_y_ = 0.8f;  // ~46 degrees
    float velocity_x_ = 0.0f;
    float velocity_y_ = 0.0f;
    bool dragging_ = false;
};

/// World-space ray through a pixel of a viewport. Returns false if the matrix is singular.
[[nodiscard]] bool make_pick_ray(const Mat4& view_proj,
                                 float pixel_x,
                                 float pixel_y,
                                 float viewport_w,
                                 float viewport_h,
                                 Vec3& origin,
                                 Vec3& direction);

}  // namespace pat::engine
