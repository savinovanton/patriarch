#include "pat/core/rng.hpp"
#include "pat/engine/camera.hpp"
#include "pat/engine/math.hpp"
#include "pat/engine/terrain.hpp"
#include "pat/mapdata/map_gen.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <vector>

using namespace pat::engine;
namespace mapdata = pat::mapdata;

namespace {

bool near(float a, float b, float eps = 1e-3f) {
    return std::fabs(a - b) <= eps;
}

mapdata::MapData flat_map(std::uint32_t w, std::uint32_t h, std::uint16_t value) {
    mapdata::MapData m;
    m.width = w;
    m.height = h;
    m.sea_level = 1000;
    m.metres_per_unit_milli = 1000;
    m.heights.assign(static_cast<std::size_t>(w) * h, value);
    m.provinces.assign(m.heights.size(), value > 1000 ? 1 : 0);
    m.materials.assign(m.heights.size(), 3);
    return m;
}

}  // namespace

// ---------------------------------------------------------------- math

TEST_CASE("math: look_at and perspective put the focus point in the middle of the screen") {
    const Mat4 view = look_at({100.0f, 400.0f, 300.0f}, {100.0f, 200.0f, 0.0f}, {0, 0, 1});
    const Mat4 proj = perspective(0.8f, 16.0f / 9.0f, 1.0f, 5000.0f);
    const Vec3 ndc = transform_point(proj * view, {100.0f, 200.0f, 0.0f});
    CHECK(near(ndc.x, 0.0f));
    CHECK(near(ndc.y, 0.0f));
    CHECK(ndc.z > 0.0f);
    CHECK(ndc.z < 1.0f);

    // East is to the right of the screen when looking north (-y), north is up the screen.
    const Vec3 east = transform_point(proj * view, {150.0f, 200.0f, 0.0f});
    CHECK(east.x > 0.0f);
    const Vec3 up = transform_point(proj * view, {100.0f, 200.0f, 50.0f});
    CHECK(up.y > 0.0f);
}

TEST_CASE("math: inverse and pick rays") {
    const Mat4 vp = perspective(0.9f, 1.5f, 1.0f, 3000.0f) * look_at({10, 20, 100}, {50, 60, 0}, {0, 0, 1});
    Mat4 inv;
    REQUIRE(invert(vp, inv));
    const Mat4 id = vp * inv;
    for (std::size_t i = 0; i < 4; ++i) {
        for (std::size_t j = 0; j < 4; ++j) {
            CHECK(near(id.m[i][j], i == j ? 1.0f : 0.0f, 1e-3f));
        }
    }
    Mat4 singular;
    CHECK_FALSE(invert(singular, inv));

    Vec3 origin;
    Vec3 dir;
    REQUIRE(make_pick_ray(vp, 600.0f, 400.0f, 1200.0f, 800.0f, origin, dir));  // centre pixel
    // The ray through the screen centre passes through the look-at target.
    const Vec3 to_target = Vec3{50, 60, 0} - origin;
    const Vec3 along = dir * dot(to_target, dir);
    CHECK(length(to_target - along) < 0.05f);
}

// ---------------------------------------------------------------- camera

TEST_CASE("camera: eye distance, zoom limits and automatic tilt") {
    CameraLimits lim;
    lim.map_width = 8192;
    lim.map_height = 4096;
    lim.min_distance = 50;
    lim.max_distance = 6000;
    OrbitCamera cam(lim);
    CHECK(near(cam.distance(), 6000.0f));
    CHECK(near(length(cam.eye() - cam.focus()), 6000.0f, 0.5f));
    CHECK(cam.zoom_fraction() > 0.99f);
    const float far_pitch = cam.pitch();

    for (int i = 0; i < 100; ++i) {
        cam.zoom_towards(cam.focus(), 1.0f);
    }
    CHECK(near(cam.distance(), 50.0f));
    CHECK(cam.zoom_fraction() < 0.01f);
    CHECK(cam.pitch() < far_pitch);  // oblique when close
    CHECK(near(length(cam.eye() - cam.focus()), 50.0f, 0.1f));

    for (int i = 0; i < 100; ++i) {
        cam.zoom_towards(cam.focus(), -1.0f);
    }
    CHECK(near(cam.distance(), 6000.0f));

    // Manual tilt is limited.
    cam.rotate(0.0f, 10.0f);
    CHECK(cam.pitch() <= lim.max_pitch + 1e-4f);
    cam.rotate(0.0f, -100.0f);
    CHECK(cam.pitch() >= lim.min_pitch - 1e-4f);
}

TEST_CASE("camera: zooming towards a point slides the focus to it") {
    CameraLimits lim;
    lim.map_width = 1000;
    lim.map_height = 500;
    lim.min_distance = 20;
    lim.max_distance = 2000;
    OrbitCamera cam(lim);
    cam.set_focus(500, 250);
    cam.set_distance(1000);
    const Vec3 target{800.0f, 100.0f, 0.0f};
    cam.zoom_towards(target, 3.0f);
    CHECK(cam.focus().x > 500.0f);
    CHECK(cam.focus().x < 800.0f);
    CHECK(cam.focus().y < 250.0f);
    CHECK(cam.focus().y > 100.0f);
    // Fully zoomed in, the focus converges on the point.
    for (int i = 0; i < 60; ++i) {
        cam.zoom_towards(target, 1.0f);
    }
    CHECK(near(cam.focus().x, target.x, 15.0f));
}

TEST_CASE("camera: panning moves the ground with the cursor and glides on release") {
    CameraLimits lim;
    lim.map_width = 4000;
    lim.map_height = 2000;
    OrbitCamera cam(lim);
    cam.set_focus(2000, 1000);
    cam.set_distance(500);
    cam.set_yaw(0.0f);

    cam.begin_pan();
    cam.drag_pan(100.0f, 0.0f, 1080.0f, 0.016f);  // cursor right -> ground right -> focus moves west
    CHECK(cam.focus().x < 2000.0f);
    const float x_after_drag = cam.focus().x;
    cam.drag_pan(0.0f, 100.0f, 1080.0f, 0.016f);  // cursor down -> focus moves north (smaller y)
    CHECK(cam.focus().y < 1000.0f);

    cam.release_pan();
    const float before = cam.focus().y;
    cam.update(0.1f);
    CHECK(cam.focus().y < before);  // keeps gliding
    for (int i = 0; i < 400; ++i) {
        cam.update(0.05f);
    }
    const float settled = cam.focus().y;
    cam.update(0.05f);
    CHECK(near(cam.focus().y, settled, 0.01f));  // inertia has died out
    CHECK(cam.focus().x <= x_after_drag + 1.0f);

    // The focus never leaves the map.
    cam.begin_pan();
    cam.drag_pan(-100000.0f, -100000.0f, 1080.0f, 0.016f);
    CHECK(cam.focus().x <= 4000.0f);
    CHECK(cam.focus().y <= 2000.0f);
    CHECK(cam.focus().x >= 0.0f);
    CHECK(cam.focus().y >= 0.0f);
}

TEST_CASE("camera: rotation changes the viewing direction") {
    CameraLimits lim;
    OrbitCamera cam(lim);
    cam.set_distance(500);
    cam.set_focus(500, 250);
    const Vec3 e0 = cam.eye();
    CHECK(e0.y > cam.focus().y);  // yaw 0 looks north from the south
    cam.rotate(3.14159265f, 0.0f);
    const Vec3 e1 = cam.eye();
    CHECK(e1.y < cam.focus().y);
}

// ---------------------------------------------------------------- terrain geometry and LOD

TEST_CASE("terrain geometry: levels and heights") {
    const mapdata::MapData m = flat_map(8192, 4096, 5000);
    const TerrainGeometry g = TerrainGeometry::from_map(m);
    CHECK(g.root_size == 4096.0f);
    CHECK(g.root_count_x() == 2);
    CHECK(g.max_level == 6);  // 4096 / 2^6 = 64 = one vertex per texel
    CHECK(near(g.node_size(1), 2048.0f));
    CHECK(g.world_z(500.0f) == 0.0f);  // below the waterline
    CHECK(g.world_z(1000.0f) == 0.0f);
    CHECK(g.world_z(65535.0f) > 0.0f);
    CHECK(near(g.world_z(1000.0f + 6553.5f), 0.1f * g.relief, 0.001f * g.relief));  // 10% of full scale
}

TEST_CASE("terrain LOD: nodes tile the whole map without gaps or overlaps") {
    const mapdata::MapData m = flat_map(512, 256, 5000);
    const TerrainGeometry g = TerrainGeometry::from_map(m);
    LodSettings lod;
    std::vector<TerrainNode> nodes;
    for (const Vec3 eye :
         {Vec3{100, 100, 40}, Vec3{256, 128, 400}, Vec3{500, 10, 5}, Vec3{256, 128, 20000}}) {
        select_terrain_nodes(g, eye, nullptr, lod, nodes);
        // Rasterize the nodes into a coverage grid at 1 texel resolution.
        std::vector<int> cover(512 * 256, 0);
        for (const TerrainNode& n : nodes) {
            for (int y = static_cast<int>(n.y); y < static_cast<int>(n.y + n.size) && y < 256; ++y) {
                for (int x = static_cast<int>(n.x); x < static_cast<int>(n.x + n.size) && x < 512; ++x) {
                    ++cover[static_cast<std::size_t>(y) * 512 + static_cast<std::size_t>(x)];
                }
            }
        }
        for (const int c : cover) {
            REQUIRE(c == 1);
        }
    }
}

TEST_CASE("terrain LOD: detail is concentrated near the camera and levels step down smoothly") {
    const TerrainGeometry g = TerrainGeometry::from_map(flat_map(8192, 4096, 5000));
    LodSettings lod;
    std::vector<TerrainNode> nodes;
    const Vec3 eye{2000.0f, 2000.0f, 150.0f};
    select_terrain_nodes(g, eye, nullptr, lod, nodes);

    int finest = 0;
    for (const TerrainNode& n : nodes) {
        finest = std::max(finest, n.level);
    }
    CHECK(finest == g.max_level);

    // Nodes further away are never finer than nodes closer to the camera.
    for (const TerrainNode& a : nodes) {
        for (const TerrainNode& b : nodes) {
            const float da = std::hypot(a.x + a.size * 0.5f - eye.x, a.y + a.size * 0.5f - eye.y);
            const float db = std::hypot(b.x + b.size * 0.5f - eye.x, b.y + b.size * 0.5f - eye.y);
            if (da + a.size < db - b.size) {  // a is clearly closer than b
                CHECK(a.level >= b.level);
            }
        }
    }

    // Far away: only the two root nodes.
    select_terrain_nodes(g, {4096, 2048, 60000}, nullptr, lod, nodes);
    CHECK(nodes.size() == 2);
}

TEST_CASE("terrain LOD: frustum culling and the node budget") {
    CameraLimits lim;
    lim.map_width = 8192;
    lim.map_height = 4096;
    lim.min_distance = 60;
    lim.max_distance = 7000;
    const TerrainGeometry g = TerrainGeometry::from_map(flat_map(8192, 4096, 5000));
    LodSettings lod;
    std::vector<TerrainNode> all;
    std::vector<TerrainNode> culled;
    std::size_t worst = 0;
    for (const float distance : {7000.0f, 3000.0f, 1200.0f, 500.0f, 200.0f, 60.0f}) {
        OrbitCamera cam(lim);
        cam.set_focus(3000, 1800);
        cam.set_distance(distance);
        cam.rotate(0.7f, 0.0f);
        const Mat4 vp = cam.view_projection(16.0f / 9.0f);
        const Frustum f = Frustum::from_view_proj(vp);
        select_terrain_nodes(g, cam.eye(), nullptr, lod, all);
        select_terrain_nodes(g, cam.eye(), &f, lod, culled);
        CHECK(culled.size() <= all.size());
        worst = std::max(worst, culled.size());
        for (const TerrainNode& n : culled) {
            CHECK(f.intersects_aabb({n.x, n.y, 0}, {n.x + n.size, n.y + n.size, g.relief}));
        }
    }
    CHECK(worst <= 500);  // ADR-0003 budget: at most ~500 terrain nodes per frame

    // Looking at one corner at close range culls most of the map.
    OrbitCamera cam(lim);
    cam.set_focus(100, 100);
    cam.set_distance(150);
    const Frustum f = Frustum::from_view_proj(cam.view_projection(16.0f / 9.0f));
    select_terrain_nodes(g, cam.eye(), nullptr, lod, all);
    select_terrain_nodes(g, cam.eye(), &f, lod, culled);
    CHECK(culled.size() < all.size() / 2);
}

TEST_CASE("frustum: boxes in front are visible, behind and far to the side are not") {
    const Mat4 vp = perspective(0.8f, 1.0f, 1.0f, 1000.0f) * look_at({0, 0, 0}, {0, -1, 0}, {0, 0, 1});
    const Frustum f = Frustum::from_view_proj(vp);
    CHECK(f.intersects_aabb({-5, -60, -5}, {5, -50, 5}));            // ahead
    CHECK_FALSE(f.intersects_aabb({-5, 50, -5}, {5, 60, 5}));        // behind
    CHECK_FALSE(f.intersects_aabb({500, -60, -5}, {510, -50, 5}));   // far to the right
    CHECK_FALSE(f.intersects_aabb({-5, -2000, -5}, {5, -1500, 5}));  // beyond the far plane
}

// ---------------------------------------------------------------- picking

TEST_CASE("picker: rays hit a flat map at the right place and misses return nothing") {
    const mapdata::MapData m = flat_map(256, 128, 5000);
    const TerrainGeometry g = TerrainGeometry::from_map(m);
    const HeightfieldPicker picker(m, g);

    const float z = picker.height_at(100.0f, 50.0f);
    CHECK(near(z, g.world_z(5000.0f), 0.01f));

    auto hit = picker.raycast({100.0f, 50.0f, 500.0f}, {0, 0, -1}, 2000.0f);
    REQUIRE(hit.hit);
    CHECK(near(hit.point.x, 100.0f, 0.05f));
    CHECK(near(hit.point.y, 50.0f, 0.05f));
    CHECK(near(hit.point.z, z, 0.05f));

    // Oblique ray: from (0, 64, 100) heading east and down.
    hit = picker.raycast({10.0f, 64.0f, z + 100.0f}, normalize({1.0f, 0.0f, -1.0f}), 1000.0f);
    REQUIRE(hit.hit);
    CHECK(near(hit.point.x, 110.0f, 0.2f));

    // Pointing away, or too short to arrive, misses.
    CHECK_FALSE(picker.raycast({100.0f, 50.0f, 500.0f}, {0, 0, 1}, 2000.0f).hit);
    CHECK_FALSE(picker.raycast({100.0f, 50.0f, 500.0f}, {0, 0, -1}, 10.0f).hit);
    CHECK_FALSE(picker.raycast({1000.0f, 50.0f, 500.0f}, {0, 0, -1}, 2000.0f).hit);  // outside the map

    CHECK(picker.province_at(10.0f, 10.0f) == 1);
    CHECK(picker.province_at(-1.0f, 10.0f) == 0);
    CHECK(picker.province_at(256.0f, 10.0f) == 0);
}

TEST_CASE("picker: skipping empty blocks gives the same answers as brute-force marching") {
    mapdata::GenParams params;
    params.width = 512;
    params.height = 256;
    params.province_count = 40;
    params.seed = 21;
    params.threads = 2;
    const mapdata::MapData m = mapdata::generate_map(params);
    const TerrainGeometry g = TerrainGeometry::from_map(m, 0.08f);  // exaggerated relief makes it harder
    const HeightfieldPicker picker(m, g);

    pat::core::Pcg32 rng(5, 1);
    const auto uniform = [&](float lo, float hi) {
        return lo + (hi - lo) * (static_cast<float>(rng.bounded(100000)) / 100000.0f);
    };

    int hits = 0;
    int mismatches = 0;
    for (int i = 0; i < 400; ++i) {
        const Vec3 origin{uniform(-100.0f, 612.0f), uniform(-100.0f, 356.0f), uniform(20.0f, 300.0f)};
        const Vec3 target{uniform(0.0f, 512.0f), uniform(0.0f, 256.0f), 0.0f};
        const Vec3 dir = normalize(target - origin);
        const float max_dist = 1500.0f;
        const auto fast = picker.raycast(origin, dir, max_dist);

        // Brute force: fine steps over the whole ray.
        bool slow_hit = false;
        Vec3 slow_point;
        float prev_t = 0.0f;
        for (float t = 0.0f; t <= max_dist; t += 0.05f) {
            const Vec3 p = origin + dir * t;
            if (p.x >= 0.0f && p.y >= 0.0f && p.x <= g.width && p.y <= g.height &&
                p.z <= picker.height_at(p.x, p.y)) {
                slow_hit = true;
                slow_point = origin + dir * prev_t;
                break;
            }
            prev_t = t;
        }
        hits += slow_hit ? 1 : 0;
        if (fast.hit != slow_hit || (slow_hit && length(fast.point - slow_point) > 1.0f)) {
            ++mismatches;
        }
    }
    CHECK(hits > 100);  // the test actually exercises terrain hits
    CHECK(mismatches == 0);
}

TEST_CASE("picker: picking through the camera finds the province under a pixel") {
    mapdata::GenParams params;
    params.width = 512;
    params.height = 256;
    params.province_count = 30;
    params.seed = 4;
    params.threads = 2;
    const mapdata::MapData m = mapdata::generate_map(params);
    const TerrainGeometry g = TerrainGeometry::from_map(m);
    const HeightfieldPicker picker(m, g);

    CameraLimits lim;
    lim.map_width = g.width;
    lim.map_height = g.height;
    lim.min_distance = 20;
    lim.max_distance = 1500;
    OrbitCamera cam(lim);

    // Aim the camera at the centroid of a province: the screen centre must pick that province.
    int checked = 0;
    for (const mapdata::Province& p : m.province_list) {
        cam.set_focus(static_cast<float>(p.centroid_x) + 0.5f, static_cast<float>(p.centroid_y) + 0.5f);
        cam.set_focus_height(picker.height_at(cam.focus().x, cam.focus().y));
        cam.set_distance(300.0f);
        Vec3 origin;
        Vec3 dir;
        REQUIRE(make_pick_ray(cam.view_projection(1.5f), 600.0f, 400.0f, 1200.0f, 800.0f, origin, dir));
        const auto hit = picker.raycast(origin, dir, 5000.0f);
        REQUIRE(hit.hit);
        CHECK(near(hit.point.x, cam.focus().x, 1.0f));
        CHECK(near(hit.point.y, cam.focus().y, 1.0f));
        CHECK(picker.province_at(hit.point.x, hit.point.y) ==
              m.provinces[m.index(p.centroid_x, p.centroid_y)]);
        ++checked;
    }
    CHECK(checked == 30);
}
