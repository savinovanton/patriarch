// Engine spike application (E3).
//   T01: window, input, DPI, fixed-step logic + free render loop, FPS.
//   T02: Direct3D 11 renderer behind IRenderer.
//   T04: volumetric map: CDLOD terrain, province layer with borders, orbit camera, picking, map modes.
// Later tasks add labels and RmlUi on top of this loop.
//
// Controls: left drag = pan (click = select province), right drag = rotate/tilt, wheel = zoom to cursor,
// WASD/arrows = move, Q/E = rotate, 1-4 = map mode (terrain / owners / mood / random),
// V = vsync, F5 = reload shaders, F11 = fullscreen, Esc = quit.

#include "pat/core/log.hpp"
#include "pat/core/png_writer.hpp"
#include "pat/engine/camera.hpp"
#include "pat/engine/dpi.hpp"
#include "pat/engine/frame_clock.hpp"
#include "pat/engine/renderer.hpp"
#include "pat/engine/terrain.hpp"
#include "pat/engine/window.hpp"
#include "pat/mapdata/map_format.hpp"
#include "pat/mapdata/map_gen.hpp"

#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <variant>
#include <vector>

#ifndef PATRIARCH_SPIKE_SHADER_DIR
#define PATRIARCH_SPIKE_SHADER_DIR ""
#endif

namespace {

constexpr int kScancode1 = 30;
constexpr int kScancodeV = 25;
constexpr int kScancodeF5 = 62;
constexpr int kScancodeW = 26;
constexpr int kScancodeA = 4;
constexpr int kScancodeS = 22;
constexpr int kScancodeD = 7;
constexpr int kScancodeQ = 20;
constexpr int kScancodeE = 8;
constexpr int kScancodeRight = 79;
constexpr int kScancodeLeft = 80;
constexpr int kScancodeDown = 81;
constexpr int kScancodeUp = 82;

struct Options {
    std::int64_t max_frames = -1;  // -1: run until closed
    bool hidden = false;           // hidden window + offscreen software renderer (headless smoke run)
    bool no_vsync = false;
    bool bench = false;            // scripted camera path, prints frame statistics, then exits
    std::int64_t bench_frames = 900;
    std::string shot_file;         // render a few frames, save a PNG screenshot and exit
    float view_fx = 0.5f, view_fy = 0.5f, view_dist = -1.0f, view_yaw = 0.0f;  // --view start camera
    std::string map_file;          // load a PMD1 map instead of generating one
    std::uint32_t gen_width = 8192;
    std::uint32_t gen_height = 4096;
    std::uint32_t provinces = 400;
};

bool parse_options(int argc, char** argv, Options& out) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--frames" && i + 1 < argc) {
            out.max_frames = std::atoll(argv[++i]);
        } else if (arg == "--hidden") {
            out.hidden = true;
        } else if (arg == "--no-vsync") {
            out.no_vsync = true;
        } else if (arg == "--bench") {
            out.bench = true;
            out.no_vsync = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                out.bench_frames = std::atoll(argv[++i]);
            }
        } else if (arg == "--shot" && i + 1 < argc) {
            out.shot_file = argv[++i];
        } else if (arg == "--view" && i + 1 < argc) {
            if (std::sscanf(argv[++i], "%f,%f,%f,%f", &out.view_fx, &out.view_fy, &out.view_dist, &out.view_yaw) != 4) {
                std::fprintf(stderr, "--view expects fx,fy,distance,yaw (e.g. 0.5,0.5,800,0.3)\n");
                return false;
            }
        } else if (arg == "--map" && i + 1 < argc) {
            out.map_file = argv[++i];
        } else if (arg == "--gen" && i + 1 < argc) {
            unsigned w = 0;
            unsigned h = 0;
            if (std::sscanf(argv[++i], "%ux%u", &w, &h) != 2 || w < 64 || h < 32) {
                std::fprintf(stderr, "--gen expects WIDTHxHEIGHT, e.g. 4096x2048\n");
                return false;
            }
            out.gen_width = w;
            out.gen_height = h;
        } else if (arg == "--provinces" && i + 1 < argc) {
            out.provinces = static_cast<std::uint32_t>(std::atoi(argv[++i]));
        } else {
            std::fprintf(stderr,
                         "usage: patriarch_spike [--map FILE | --gen WxH] [--provinces N] [--frames N] [--hidden]\n"
                         "                       [--no-vsync] [--bench [FRAMES]]\n");
            return false;
        }
    }
    return true;
}

std::int64_t now_ns() {
    return static_cast<std::int64_t>(SDL_GetTicksNS());
}

double percentile(std::vector<double> v, double p) {
    if (v.empty()) {
        return 0.0;
    }
    std::sort(v.begin(), v.end());
    const auto idx = static_cast<std::size_t>(p * static_cast<double>(v.size() - 1) + 0.5);
    return v[idx];
}

const char* mode_name(pat::engine::MapMode m) {
    switch (m) {
        case pat::engine::MapMode::Terrain: return "terrain";
        case pat::engine::MapMode::Owners: return "owners";
        case pat::engine::MapMode::Mood: return "mood";
        case pat::engine::MapMode::Random: return "random";
        default: return "?";
    }
}

}  // namespace

int main(int argc, char** argv) {
    using namespace pat::engine;
    namespace mapdata = pat::mapdata;

    Options opt;
    if (!parse_options(argc, argv, opt)) {
        return 2;
    }

    // ---- map data
    mapdata::MapData map;
    if (!opt.map_file.empty()) {
        auto loaded = mapdata::read_map_file(opt.map_file);
        if (!loaded) {
            PAT_LOG_ERROR("spike", "cannot load map: {}", loaded.error().message);
            return 1;
        }
        map = std::move(loaded).value();
    } else {
        mapdata::GenParams gp;
        gp.width = opt.hidden ? std::min<std::uint32_t>(opt.gen_width, 1024) : opt.gen_width;
        gp.height = opt.hidden ? std::min<std::uint32_t>(opt.gen_height, 512) : opt.gen_height;
        gp.province_count = opt.hidden ? std::min<std::uint32_t>(opt.provinces, 120) : opt.provinces;
        map = mapdata::generate_map(gp);
    }

    // ---- window, renderer, map view
    WindowDesc desc;
    desc.title = "Patriarch - spike";
    desc.hidden = opt.hidden;
    auto created = Window::create(desc);
    if (!created) {
        PAT_LOG_ERROR("spike", "{}", created.error().message);
        return 1;
    }
    Window window = std::move(created).value();

    RendererDesc rdesc;
    rdesc.native_window = opt.hidden ? nullptr : window.native_handle();
    rdesc.size = window.pixel_size();
    rdesc.vsync = !opt.no_vsync;
    rdesc.software = opt.hidden;
    rdesc.shader_dir = PATRIARCH_SPIKE_SHADER_DIR;
#ifndef NDEBUG
    rdesc.debug_layer = true;
#endif
    auto renderer_result = create_renderer(rdesc);
    if (!renderer_result) {
        PAT_LOG_ERROR("spike", "renderer: {}", renderer_result.error().message);
        return 1;
    }
    std::unique_ptr<IRenderer> renderer = std::move(renderer_result).value();

    const auto t_view = std::chrono::steady_clock::now();
    auto view_result = renderer->create_map_view(map, {});
    if (!view_result) {
        PAT_LOG_ERROR("spike", "map view: {}", view_result.error().message);
        return 1;
    }
    std::unique_ptr<IMapView> view = std::move(view_result).value();
    PAT_LOG_INFO("spike", "map view ready in {:.0f} ms ({}x{} texels, {} provinces)",
                 std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_view).count(),
                 map.width, map.height, map.province_list.size());

    const TerrainGeometry geometry = view->geometry();
    const HeightfieldPicker picker(map, geometry);

    CameraLimits limits;
    limits.map_width = geometry.width;
    limits.map_height = geometry.height;
    limits.min_distance = 60.0f;
    limits.max_distance = std::max(geometry.width, geometry.height) * 1.1f;
    OrbitCamera camera(limits);
    camera.set_focus(geometry.width * 0.5f, geometry.height * 0.5f);
    camera.set_distance(geometry.width * 0.62f);  // start with the whole map in view
    view->set_mode(MapMode::Owners);
    if (opt.view_dist > 0.0f) {
        camera.set_focus(geometry.width * opt.view_fx, geometry.height * opt.view_fy);
        camera.set_distance(opt.view_dist);
        camera.set_yaw(opt.view_yaw);
    }

    // ---- loop state
    FrameClock clock;
    std::vector<Event> events;
    bool running = true;
    std::uint64_t logic_steps = 0;
    float ui_scale = effective_ui_scale(window.display_scale());
    std::int64_t last_title_frame_count = 0;
    std::int64_t last_frame_ns = now_ns();

    float cursor_x = -1.0f;
    float cursor_y = -1.0f;
    bool left_down = false;
    bool right_down = false;
    float press_x = 0.0f;
    float press_y = 0.0f;
    bool dragged = false;
    std::uint16_t hovered = 0;
    std::uint16_t selected = 0;
    bool key_w = false, key_a = false, key_s = false, key_d = false, key_q = false, key_e = false;

    // Benchmark bookkeeping.
    std::vector<double> frame_ms;
    std::size_t min_nodes = ~static_cast<std::size_t>(0);
    std::size_t max_nodes = 0;
    double max_mode_switch_ms = 0.0;
    std::int64_t bench_frame = 0;

    while (running) {
        const std::int64_t frame_start = now_ns();
        events.clear();
        window.poll_events(events);
        const PixelSize px = window.pixel_size();
        const float width_px = static_cast<float>(std::max(px.w, 1));
        const float height_px = static_cast<float>(std::max(px.h, 1));
        const float aspect = width_px / height_px;
        const float frame_dt = std::clamp(static_cast<float>(frame_start - last_frame_ns) * 1e-9f, 0.0001f, 0.1f);
        last_frame_ns = frame_start;

        // Ground point under a pixel (or the focus if the ray misses the map).
        const auto ground_at = [&](float sx, float sy, Vec3& point) {
            Vec3 origin;
            Vec3 dir;
            if (!make_pick_ray(camera.view_projection(aspect), sx, sy, width_px, height_px, origin, dir)) {
                return false;
            }
            const auto hit = picker.raycast(origin, dir, camera.far_plane() * 2.0f);
            point = hit.point;
            return hit.hit;
        };

        for (const Event& e : events) {
            if (std::holds_alternative<event::Quit>(e)) {
                running = false;
            } else if (const auto* key = std::get_if<event::Key>(&e)) {
                const bool down = key->down;
                if (key->scancode == kScancodeW || key->scancode == kScancodeUp) key_w = down;
                if (key->scancode == kScancodeS || key->scancode == kScancodeDown) key_s = down;
                if (key->scancode == kScancodeA || key->scancode == kScancodeLeft) key_a = down;
                if (key->scancode == kScancodeD || key->scancode == kScancodeRight) key_d = down;
                if (key->scancode == kScancodeQ) key_q = down;
                if (key->scancode == kScancodeE) key_e = down;
                if (down && !key->repeat) {
                    if (key->scancode == scancode::escape) {
                        running = false;
                    } else if (key->scancode == scancode::f11) {
                        window.set_fullscreen(!window.is_fullscreen());
                    } else if (key->scancode == kScancodeV) {
                        renderer->set_vsync(!renderer->vsync());
                        PAT_LOG_INFO("spike", "vsync {}", renderer->vsync() ? "on" : "off");
                    } else if (key->scancode == kScancodeF5) {
                        renderer->reload_shaders_if_changed();
                        view->reload_shaders_if_changed();
                    } else if (key->scancode >= kScancode1 && key->scancode < kScancode1 + 4) {
                        view->set_mode(static_cast<MapMode>(key->scancode - kScancode1));
                    }
                }
            } else if (const auto* resized = std::get_if<event::Resized>(&e)) {
                renderer->resize({resized->pixel_w, resized->pixel_h});
                PAT_LOG_INFO("spike", "resized to {}x{} px", resized->pixel_w, resized->pixel_h);
            } else if (const auto* scale = std::get_if<event::ScaleChanged>(&e)) {
                ui_scale = effective_ui_scale(scale->scale);
                PAT_LOG_INFO("spike", "display scale changed to {:.3f}", ui_scale);
            } else if (const auto* move = std::get_if<event::MouseMove>(&e)) {
                cursor_x = move->x;
                cursor_y = move->y;
                if (left_down) {
                    camera.drag_pan(move->dx, move->dy, height_px, frame_dt);
                    if (std::hypot(cursor_x - press_x, cursor_y - press_y) > 4.0f) {
                        dragged = true;
                    }
                }
                if (right_down) {
                    camera.rotate(move->dx * 0.005f, move->dy * 0.004f);
                }
            } else if (const auto* button = std::get_if<event::MouseButton>(&e)) {
                if (button->button == 1) {
                    if (button->down) {
                        left_down = true;
                        dragged = false;
                        press_x = button->x;
                        press_y = button->y;
                        camera.begin_pan();
                    } else {
                        left_down = false;
                        camera.release_pan();
                        if (!dragged) {
                            // A click: select the province under the cursor (clicking water deselects).
                            Vec3 p;
                            selected = ground_at(button->x, button->y, p) ? picker.province_at(p.x, p.y) : 0;
                        }
                    }
                } else if (button->button == 3) {
                    right_down = button->down;
                }
            } else if (const auto* wheel = std::get_if<event::MouseWheel>(&e)) {
                Vec3 p = camera.focus();
                if (cursor_x < 0.0f || !ground_at(cursor_x, cursor_y, p)) {
                    p = camera.focus();
                }
                camera.zoom_towards(p, wheel->y);
            }
        }

        const FrameStep step = clock.advance(frame_start);
        logic_steps += static_cast<std::uint64_t>(step.fixed_steps);

        // Camera: keyboard movement and a scripted path for --bench.
        if (opt.bench) {
            const float u = static_cast<float>(bench_frame) / static_cast<float>(opt.bench_frames);
            const float tau = 6.2831853f;
            const float s = 0.5f - 0.5f * std::cos(tau * u);  // zoom out -> in -> out
            const float dist = std::exp(std::log(limits.max_distance) * (1.0f - s) + std::log(limits.min_distance) * s);
            camera.set_distance(dist);
            camera.set_focus(geometry.width * (0.5f + 0.22f * std::sin(tau * u * 2.0f)),
                             geometry.height * (0.5f + 0.18f * std::sin(tau * u * 1.5f)));
            camera.set_yaw(0.9f * std::sin(tau * u));
        } else {
            const float speed = camera.distance() * 0.8f;
            camera.move((key_d ? 1.0f : 0.0f) - (key_a ? 1.0f : 0.0f), (key_w ? 1.0f : 0.0f) - (key_s ? 1.0f : 0.0f),
                        speed, frame_dt);
            camera.rotate(((key_e ? 1.0f : 0.0f) - (key_q ? 1.0f : 0.0f)) * 1.2f * frame_dt, 0.0f);
            camera.update(frame_dt);
        }
        camera.set_focus_height(picker.height_at(camera.focus().x, camera.focus().y));

        // Hover pick.
        hovered = 0;
        if (cursor_x >= 0.0f && cursor_x < width_px && cursor_y >= 0.0f && cursor_y < height_px) {
            Vec3 p;
            if (ground_at(cursor_x, cursor_y, p)) {
                hovered = picker.province_at(p.x, p.y);
            }
        }

        if (opt.bench && opt.bench_frames >= 10 && bench_frame % (opt.bench_frames / 10) == 0) {
            const auto next = static_cast<MapMode>((bench_frame / (opt.bench_frames / 10)) % 4);
            const auto t0 = std::chrono::steady_clock::now();
            view->set_mode(next);
            max_mode_switch_ms = std::max(
                max_mode_switch_ms,
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        }

        if (clock.frame_count() % 15 == 0) {
            renderer->reload_shaders_if_changed();
            view->reload_shaders_if_changed();
        }

        if (renderer->begin_frame({0.04f, 0.05f, 0.08f, 1.0f})) {
            MapFrame frame;
            frame.view_proj = camera.view_projection(aspect);
            frame.eye = camera.eye();
            frame.zoom_fraction = camera.zoom_fraction();
            frame.hovered = hovered;
            frame.selected = selected;
            frame.time_s = (static_cast<float>(logic_steps) + step.alpha) / 60.0f;
            renderer->push_marker("map");
            view->draw(frame);
            renderer->pop_marker();
            if (!opt.shot_file.empty() && clock.frame_count() >= 4) {
                const std::vector<std::uint8_t> rgba = renderer->capture();
                const PixelSize size = renderer->size();
                std::vector<std::uint8_t> rgb;
                rgb.reserve(static_cast<std::size_t>(size.w) * static_cast<std::size_t>(size.h) * 3);
                for (std::size_t i = 0; i + 3 < rgba.size(); i += 4) {
                    rgb.insert(rgb.end(), {rgba[i], rgba[i + 1], rgba[i + 2]});
                }
                const bool ok = pat::core::write_png_rgb(opt.shot_file, static_cast<std::uint32_t>(size.w),
                                                         static_cast<std::uint32_t>(size.h), rgb);
                PAT_LOG_INFO("spike", "screenshot {}: {}", opt.shot_file, ok ? "saved" : "FAILED");
                running = false;
            }
            renderer->end_frame();
            min_nodes = std::min(min_nodes, view->last_node_count());
            max_nodes = std::max(max_nodes, view->last_node_count());
        } else {
            SDL_DelayNS(16'000'000);  // Minimized or device lost: do not spin.
        }
        if (opt.hidden) {
            SDL_DelayNS(1'000'000);
        }

        if (opt.bench) {
            frame_ms.push_back(static_cast<double>(now_ns() - frame_start) * 1e-6);
            if (++bench_frame >= opt.bench_frames) {
                running = false;
            }
        }

        if (clock.fps() > 0.0f && static_cast<std::int64_t>(clock.frame_count()) - last_title_frame_count >= 30) {
            last_title_frame_count = static_cast<std::int64_t>(clock.frame_count());
            char info[96] = "";
            if (const auto* p = map.province(hovered != 0 ? hovered : selected)) {
                std::snprintf(info, sizeof(info), "province %u (%s, %zu neighbours, %u px)", p->id,
                              hovered != 0 ? "hover" : "selected", p->neighbors.size(), p->area_px);
            }
            char title[320];
            std::snprintf(title, sizeof(title),
                          "Patriarch - spike | %.0f fps (%.2f ms) | %zu nodes | mode %s | vsync %s | %s | %s",
                          static_cast<double>(clock.fps()), static_cast<double>(clock.frame_ms()),
                          view->last_node_count(), mode_name(view->mode()), renderer->vsync() ? "on" : "off",
                          std::string(renderer->adapter_name()).c_str(), info);
            window.set_title(title);
        }

        if (opt.max_frames >= 0 && static_cast<std::int64_t>(clock.frame_count()) >= opt.max_frames) {
            running = false;
        }
    }

    if (opt.bench && !frame_ms.empty()) {
        // Skip the first frames (shader warm-up, driver caches).
        std::vector<double> steady(frame_ms.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(30, frame_ms.size() / 4)),
                                   frame_ms.end());
        std::printf("bench: %zu frames on '%s' (%ux%u px), map %ux%u, %zu provinces\n", frame_ms.size(),
                    std::string(renderer->adapter_name()).c_str(), static_cast<unsigned>(window.pixel_size().w),
                    static_cast<unsigned>(window.pixel_size().h), map.width, map.height, map.province_list.size());
        std::printf("bench: frame ms  p50 %.2f  p95 %.2f  p99 %.2f  max %.2f\n", percentile(steady, 0.50),
                    percentile(steady, 0.95), percentile(steady, 0.99), percentile(steady, 1.0));
        std::printf("bench: terrain nodes per frame  min %zu  max %zu\n", min_nodes, max_nodes);
        std::printf("bench: map mode switch (CPU) max %.3f ms\n", max_mode_switch_ms);
    }

    PAT_LOG_INFO("spike", "exit after {} frames, {} logic steps", clock.frame_count(), logic_steps);
    return 0;
}
