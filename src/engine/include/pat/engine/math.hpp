#pragma once

#include <array>
#include <cmath>

// Small float math for rendering code (not used by the simulation).
//
// Conventions: left-handed world with x = east, y = south (map image rows), z = up. Matrices are
// row-major `m[row][col]` and transform column vectors (v' = M * v), so they can be uploaded to HLSL
// as `row_major float4x4` and applied with `mul(M, v)`. Clip space is D3D style (depth 0..1).

namespace pat::engine {

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct Vec4 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 0.0f;
};

[[nodiscard]] constexpr Vec3 operator+(Vec3 a, Vec3 b) noexcept {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
[[nodiscard]] constexpr Vec3 operator-(Vec3 a, Vec3 b) noexcept {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
[[nodiscard]] constexpr Vec3 operator*(Vec3 a, float s) noexcept {
    return {a.x * s, a.y * s, a.z * s};
}
[[nodiscard]] constexpr float dot(Vec3 a, Vec3 b) noexcept {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
[[nodiscard]] constexpr Vec3 cross(Vec3 a, Vec3 b) noexcept {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
[[nodiscard]] inline float length(Vec3 a) noexcept {
    return std::sqrt(dot(a, a));
}
[[nodiscard]] inline Vec3 normalize(Vec3 a) noexcept {
    const float len = length(a);
    return len > 0.0f ? a * (1.0f / len) : Vec3{};
}

struct Mat4 {
    std::array<std::array<float, 4>, 4> m{};

    [[nodiscard]] static constexpr Mat4 identity() noexcept {
        Mat4 r;
        for (int i = 0; i < 4; ++i) {
            r.m[static_cast<std::size_t>(i)][static_cast<std::size_t>(i)] = 1.0f;
        }
        return r;
    }
    [[nodiscard]] const float* data() const noexcept { return m[0].data(); }
};

[[nodiscard]] inline Mat4 operator*(const Mat4& a, const Mat4& b) noexcept {
    Mat4 r;
    for (std::size_t i = 0; i < 4; ++i) {
        for (std::size_t j = 0; j < 4; ++j) {
            float s = 0.0f;
            for (std::size_t k = 0; k < 4; ++k) {
                s += a.m[i][k] * b.m[k][j];
            }
            r.m[i][j] = s;
        }
    }
    return r;
}

[[nodiscard]] inline Vec4 operator*(const Mat4& a, Vec4 v) noexcept {
    return {a.m[0][0] * v.x + a.m[0][1] * v.y + a.m[0][2] * v.z + a.m[0][3] * v.w,
            a.m[1][0] * v.x + a.m[1][1] * v.y + a.m[1][2] * v.z + a.m[1][3] * v.w,
            a.m[2][0] * v.x + a.m[2][1] * v.y + a.m[2][2] * v.z + a.m[2][3] * v.w,
            a.m[3][0] * v.x + a.m[3][1] * v.y + a.m[3][2] * v.z + a.m[3][3] * v.w};
}

/// Transforms a point and performs the perspective divide.
[[nodiscard]] inline Vec3 transform_point(const Mat4& a, Vec3 p) noexcept {
    const Vec4 r = a * Vec4{p.x, p.y, p.z, 1.0f};
    const float inv = r.w != 0.0f ? 1.0f / r.w : 1.0f;
    return {r.x * inv, r.y * inv, r.z * inv};
}

/// View matrix looking from `eye` at `target` (left-handed).
[[nodiscard]] inline Mat4 look_at(Vec3 eye, Vec3 target, Vec3 up) noexcept {
    const Vec3 z = normalize(target - eye);
    const Vec3 x = normalize(cross(up, z));
    const Vec3 y = cross(z, x);
    Mat4 r = Mat4::identity();
    r.m[0] = {x.x, x.y, x.z, -dot(x, eye)};
    r.m[1] = {y.x, y.y, y.z, -dot(y, eye)};
    r.m[2] = {z.x, z.y, z.z, -dot(z, eye)};
    return r;
}

/// Perspective projection (left-handed, depth range 0..1).
[[nodiscard]] inline Mat4 perspective(float fov_y_radians, float aspect, float near_z, float far_z) noexcept {
    const float f = 1.0f / std::tan(fov_y_radians * 0.5f);
    Mat4 r;
    r.m[0] = {f / aspect, 0.0f, 0.0f, 0.0f};
    r.m[1] = {0.0f, f, 0.0f, 0.0f};
    r.m[2] = {0.0f, 0.0f, far_z / (far_z - near_z), -near_z * far_z / (far_z - near_z)};
    r.m[3] = {0.0f, 0.0f, 1.0f, 0.0f};
    return r;
}

/// General 4x4 inverse (cofactor expansion). Returns false for singular matrices.
[[nodiscard]] inline bool invert(const Mat4& in, Mat4& out) noexcept {
    const float* a = in.data();
    float inv[16];
    inv[0] = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15] + a[9] * a[7] * a[14] +
             a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
    inv[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15] - a[8] * a[7] * a[14] -
             a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
    inv[8] = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15] + a[8] * a[7] * a[13] +
             a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
    inv[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14] - a[8] * a[6] * a[13] -
              a[12] * a[5] * a[10] + a[12] * a[6] * a[9];
    inv[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15] - a[9] * a[3] * a[14] -
             a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
    inv[5] = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15] + a[8] * a[3] * a[14] +
             a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
    inv[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15] - a[8] * a[3] * a[13] -
             a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
    inv[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] - a[8] * a[1] * a[14] + a[8] * a[2] * a[13] +
              a[12] * a[1] * a[10] - a[12] * a[2] * a[9];
    inv[2] = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15] + a[5] * a[3] * a[14] +
             a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
    inv[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15] - a[4] * a[3] * a[14] -
             a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
    inv[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15] + a[4] * a[3] * a[13] +
              a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
    inv[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] + a[4] * a[1] * a[14] - a[4] * a[2] * a[13] -
              a[12] * a[1] * a[6] + a[12] * a[2] * a[5];
    inv[3] = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] + a[5] * a[2] * a[11] - a[5] * a[3] * a[10] -
             a[9] * a[2] * a[7] + a[9] * a[3] * a[6];
    inv[7] = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] - a[4] * a[2] * a[11] + a[4] * a[3] * a[10] +
             a[8] * a[2] * a[7] - a[8] * a[3] * a[6];
    inv[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] + a[4] * a[1] * a[11] - a[4] * a[3] * a[9] -
              a[8] * a[1] * a[7] + a[8] * a[3] * a[5];
    inv[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] - a[4] * a[1] * a[10] + a[4] * a[2] * a[9] +
              a[8] * a[1] * a[6] - a[8] * a[2] * a[5];
    const float det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
    if (std::fabs(det) < 1e-20f) {
        return false;
    }
    const float inv_det = 1.0f / det;
    for (std::size_t i = 0; i < 16; ++i) {
        out.m[i / 4][i % 4] = inv[i] * inv_det;
    }
    return true;
}

}  // namespace pat::engine
