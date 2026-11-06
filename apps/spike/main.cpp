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
#include "pat/engine/map_labels.hpp"
#include "pat/engine/renderer.hpp"
#include "pat/engine/terrain.hpp"
#include "pat/engine/ui.hpp"
#include "pat/engine/window.hpp"
#include "pat/mapdata/map_format.hpp"
#include "pat/game/world_view.hpp"
#include "pat/mapdata/map_gen.hpp"
#include "pat/sim/demo_scenario.hpp"
#include "pat/sim/runner.hpp"
#include "pat/ui/character_window.hpp"
#include "pat/ui/hud_window.hpp"
#include "pat/ui/time_bar.hpp"
#include "pat/ui/tooltip_manager.hpp"

#include <RmlUi/Core.h>
#include <SDL3/SDL_timer.h>

#ifdef _WIN32
#include <windows.h>

#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#endif

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <variant>
#include <vector>

#ifndef PATRIARCH_SPIKE_UI_DIR
#define PATRIARCH_SPIKE_UI_DIR ""
#endif

#ifndef PATRIARCH_SPIKE_SHADER_DIR
#define PATRIARCH_SPIKE_SHADER_DIR ""
#endif

namespace {

constexpr int kScancode1 = 30;
constexpr int kScancodeV = 25;
constexpr int kScancodeL = 15;
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
    float user_ui_scale = 1.0f;    // multiplies the OS display scale (--ui-scale 2 to test 200%)
    bool demo_tooltip = false;     // open nested tooltips on the first skill (for --shot)
    bool no_ui = false;            // do not create the RmlUi demo overlay
    bool no_hud = false;           // do not create the heads-up display (F4 toggles it)
    bool no_labels = false;        // no map labels
    int offscreen_w = 0;           // --offscreen WxH: render to an offscreen target on the real GPU (measurements)
    int offscreen_h = 0;
    bool no_sim = false;           // do not run the simulation thread
    bool sim_speed_set = false;
    int sim_speed = 2;             // initial game speed: 0 = paused, 1-5, 6 = as fast as possible
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
        } else if (arg == "--ui-scale" && i + 1 < argc) {
            out.user_ui_scale = static_cast<float>(std::atof(argv[++i]));
        } else if (arg == "--demo-tooltip") {
            out.demo_tooltip = true;
        } else if (arg == "--offscreen" && i + 1 < argc) {
            if (std::sscanf(argv[++i], "%dx%d", &out.offscreen_w, &out.offscreen_h) != 2 || out.offscreen_w < 64 ||
                out.offscreen_h < 64) {
                std::fprintf(stderr, "--offscreen expects WIDTHxHEIGHT, e.g. 3840x2160\n");
                return false;
            }
        } else if (arg == "--no-sim") {
            out.no_sim = true;
        } else if (arg == "--sim-speed" && i + 1 < argc) {
            out.sim_speed = std::clamp(std::atoi(argv[++i]), 0, 6);
            out.sim_speed_set = true;
        } else if (arg == "--no-hud") {
            out.no_hud = true;
        } else if (arg == "--no-labels") {
            out.no_labels = true;
        } else if (arg == "--no-ui") {
            out.no_ui = true;
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
                         "                       [--no-vsync] [--no-ui] [--no-labels] [--offscreen WxH] [--no-sim] [--sim-speed N] [--ui-scale X] [--bench [FRAMES]]\n");
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

    if (opt.bench && !opt.sim_speed_set) {
        opt.sim_speed = 5;  // benchmark the UI while the simulation runs at speed 5 (US-3.5)
    }

    // ---- window, renderer, map view
    WindowDesc desc;
    desc.title = "Patriarch - spike";
    const bool offscreen = opt.offscreen_w > 0;
    desc.hidden = opt.hidden || offscreen;
    auto created = Window::create(desc);
    if (!created) {
        PAT_LOG_ERROR("spike", "{}", created.error().message);
        return 1;
    }
    Window window = std::move(created).value();

    RendererDesc rdesc;
    rdesc.native_window = (opt.hidden || offscreen) ? nullptr : window.native_handle();
    rdesc.size = offscreen ? PixelSize{opt.offscreen_w, opt.offscreen_h} : window.pixel_size();
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

    const float gpu_mb_before = renderer->gpu_memory_mb();
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
    // The layout of the HUD is drawn for 1080p: the UI scale follows the window height (design_ui_scale).
    float ui_scale = design_ui_scale(renderer->size(), opt.user_ui_scale);
    std::int64_t last_title_frame_count = 0;
    std::int64_t last_frame_ns = now_ns();

    // ---- UI overlay (RmlUi). System fonts are used only by this development spike; the game's own fonts
    // are a separate decision (issue #27) and will live in assets/ with licence records.
    // ---- map labels: SDF text from a system font (development only; the game font is issue #27)
    std::unique_ptr<MapLabelRenderer> labels;
    bool labels_on = true;
    LabelLayoutStats label_stats;
    std::size_t label_min = ~static_cast<std::size_t>(0);
    std::size_t label_max = 0;
    if (!opt.no_labels) {
        for (const char* font : {"C:/Windows/Fonts/timesbd.ttf", "C:/Windows/Fonts/times.ttf", "C:/Windows/Fonts/arialbd.ttf"}) {
            if (!std::filesystem::exists(font)) {
                continue;
            }
            auto atlas = build_sdf_atlas(font);
            if (!atlas) {
                PAT_LOG_WARN("spike", "labels: {}", atlas.error().message);
                break;
            }
            MapLabelSet set = make_test_map_labels(map, geometry, picker, atlas.value());
            PAT_LOG_INFO("spike", "labels: {} provinces + {} regions", set.province_labels, set.region_labels);
            auto created_labels = MapLabelRenderer::create(*renderer, std::move(atlas).value(), std::move(set));
            if (created_labels) {
                labels = std::move(created_labels).value();
            } else {
                PAT_LOG_WARN("spike", "labels: {}", created_labels.error().message);
            }
            break;
        }
    }

    // ---- simulation on its own thread (E1): the UI only ever reads published snapshots
    std::unique_ptr<pat::sim::SimulationRunner> runner;
    if (!opt.no_sim) {
        pat::sim::demo::Config sim_config;
        sim_config.seed = 1;
        sim_config.dynasties = 14;
        sim_config.provinces = static_cast<std::uint32_t>(map.province_list.size());
        sim_config.characters_per_dynasty = 8;
        runner = std::make_unique<pat::sim::SimulationRunner>(pat::sim::demo::make_simulation(sim_config));
        runner->start();
        runner->set_speed(static_cast<pat::sim::Speed>(opt.sim_speed));
    }
    std::shared_ptr<const pat::sim::World> world_snapshot;
    std::uint64_t seen_version = ~static_cast<std::uint64_t>(0);
    std::vector<std::uint32_t> last_owner_palette;
    std::vector<std::uint32_t> last_development_palette;
    pat::sim::CharacterId shown_character;
    float character_refresh = 1.0e9f;  // refresh the window on the first frame

    std::unique_ptr<UiContext> ui;
    bool character_visible = true;
    std::unique_ptr<pat::ui::TooltipManager> tooltips;  // after `character`: destroyed first
    std::unique_ptr<pat::ui::CharacterWindow> character;  // declared after `ui`: destroyed first
    std::unique_ptr<pat::ui::TimeBar> time_bar;           // likewise
    std::unique_ptr<pat::ui::HudWindow> hud;              // likewise
    bool hud_visible = true;
    const auto populate_demo = [&](Rml::ElementDocument* doc) {
        if (doc == nullptr) {
            return;
        }
        if (Rml::Element* list = doc->GetElementById("list")) {
            std::string rml;
            for (int i = 1; i <= 1000; ++i) {
                rml += "<div class=\"row" + std::string(i % 2 != 0 ? " odd" : "") + "\">" + std::to_string(i) +
                       ". Иван Петрович Боярский, стольник</div>";
            }
            list->SetInnerRML(rml);
        }
    };
    if (!opt.no_ui) {
        UiDesc ud;
        ud.size = renderer->size();
        ud.ui_scale = ui_scale;
        for (const char* font :
             {"arial.ttf", "arialbd.ttf", "ariali.ttf", "arialbi.ttf", "times.ttf", "timesbd.ttf"}) {
            const std::filesystem::path path = std::filesystem::path("C:/Windows/Fonts") / font;
            if (std::filesystem::exists(path)) {
                ud.font_files.push_back(path);
            }
        }
        auto ui_result = UiContext::create(*renderer, opt.hidden ? nullptr : &window, ud);
        if (!ui_result) {
            PAT_LOG_ERROR("spike", "UI: {}", ui_result.error().message);
            return 1;
        }
        ui = std::move(ui_result).value();
        Rml::ElementDocument* doc = ui->load_document(std::filesystem::path(PATRIARCH_SPIKE_UI_DIR) / "demo.rml");
        if (doc == nullptr) {
            PAT_LOG_WARN("spike", "UI demo document not found in '{}'", PATRIARCH_SPIKE_UI_DIR);
        }
        populate_demo(doc);
        if (doc != nullptr) {
            doc->Hide();  // the plain demo panel is toggled with F2; the character window is the main UI
        }

        auto window_result = pat::ui::CharacterWindow::create(*ui, std::filesystem::path(PATRIARCH_SPIKE_UI_DIR) / "character.rml");
        if (window_result) {
            character = std::move(window_result).value();
            character->set_snapshot(pat::ui::make_test_character(7));  // replaced by a real character below
            character->set_command_handler([](const std::string& id) { PAT_LOG_INFO("spike", "character command: {}", id); });
        } else {
            PAT_LOG_WARN("spike", "character window: {}", window_result.error().message);
        }
        if (!opt.no_hud) {
            auto hud_result = pat::ui::HudWindow::create(*ui, std::filesystem::path(PATRIARCH_SPIKE_UI_DIR) / "hud.rml",
                                                         PATRIARCH_SPIKE_OLIVE_DIR);
            if (hud_result) {
                hud = std::move(hud_result).value();
                hud->set_snapshot(pat::ui::make_test_hud());  // placeholder numbers until the economy exists
                pat::ui::HudWindow* h = hud.get();
                hud->set_menu_handler([h](const std::string& key) {
                    h->set_active_menu(key);
                    PAT_LOG_INFO("spike", "menu: {}", key);
                });
            } else {
                PAT_LOG_WARN("spike", "HUD: {}", hud_result.error().message);
            }
        }
        if (runner) {
            auto bar = pat::ui::TimeBar::create(*ui, std::filesystem::path(PATRIARCH_SPIKE_UI_DIR) / "timebar.rml");
            if (bar) {
                time_bar = std::move(bar).value();
                pat::sim::SimulationRunner* r = runner.get();
                pat::ui::TimeBar* tb = time_bar.get();
                time_bar->set_speed_handler([r, tb](int speed) {
                    r->set_speed(static_cast<pat::sim::Speed>(speed));
                    tb->set_speed(speed);
                });
            } else {
                PAT_LOG_WARN("spike", "time bar: {}", bar.error().message);
            }
        }
        if (character) {
            auto tooltip_result = pat::ui::TooltipManager::create(*ui, PATRIARCH_SPIKE_UI_DIR);
            if (tooltip_result) {
                tooltips = std::move(tooltip_result).value();
                pat::ui::CharacterWindow* window_ptr = character.get();
                tooltips->set_provider([window_ptr](std::string_view key) { return window_ptr->explanation(key); });
            } else {
                PAT_LOG_WARN("spike", "tooltips: {}", tooltip_result.error().message);
            }
        }
    }

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
    std::vector<double> gpu_ms;
    std::size_t min_nodes = ~static_cast<std::size_t>(0);
    std::size_t max_nodes = 0;
    double max_mode_switch_ms = 0.0;
    std::int64_t bench_frame = 0;

    while (running) {
        const std::int64_t frame_start = now_ns();
        events.clear();
        window.poll_events(events);
        const PixelSize px = offscreen ? renderer->size() : window.pixel_size();
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
            // The UI sees every event first; what it consumes (hover over UI, text focus) the game ignores.
            const bool ui_consumed = ui != nullptr && ui->handle_event(e);
            if (tooltips) {
                tooltips->handle_event(e);
            }
            if (std::holds_alternative<event::Quit>(e)) {
                running = false;
            } else if (const auto* key = std::get_if<event::Key>(&e)) {
                if (ui_consumed) {
                    continue;
                }
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
                    } else if (key->scancode == kScancodeL) {
                        labels_on = !labels_on;
                    } else if (key->scancode == scancode::f4 && hud != nullptr) {
                        hud_visible = !hud_visible;
                        hud->show(hud_visible);
                    } else if (key->scancode == scancode::f3 && character != nullptr) {
                        character_visible = !character_visible;
                        character->show(character_visible);
                    } else if (key->scancode == scancode::f2 && ui != nullptr && ui->context()->GetNumDocuments() > 0) {
                        Rml::ElementDocument* doc = ui->context()->GetDocument(0);
                        if (doc->IsVisible()) {
                            doc->Hide();
                        } else {
                            doc->Show();
                        }
                    } else if (key->scancode >= kScancode1 && key->scancode < kScancode1 + 4) {
                        view->set_mode(static_cast<MapMode>(key->scancode - kScancode1));
                    }
                }
            } else if (const auto* resized = std::get_if<event::Resized>(&e)) {
                if (!offscreen) {
                    renderer->resize({resized->pixel_w, resized->pixel_h});
                }
                ui_scale = design_ui_scale(renderer->size(), opt.user_ui_scale);
                if (ui) {
                    ui->resize(renderer->size(), ui_scale);
                }
                PAT_LOG_INFO("spike", "resized to {}x{} px", resized->pixel_w, resized->pixel_h);
            } else if (const auto* scale = std::get_if<event::ScaleChanged>(&e)) {
                (void)scale;  // the UI scale follows the window height, not the OS scale
                if (ui) {
                    ui->resize(renderer->size(), ui_scale);
                }
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
                if (ui_consumed && button->down) {
                    continue;  // pressed on a UI element
                }
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
                            if (world_snapshot && selected != 0) {
                                // Show the head of the dynasty that owns the province.
                                const auto head = pat::game::head_for_province(*world_snapshot, selected);
                                if (head.valid()) {
                                    shown_character = head;
                                    character_refresh = 1.0e9f;  // refresh the window now
                                }
                            }
                        }
                    }
                } else if (button->button == 3) {
                    right_down = button->down;
                }
            } else if (const auto* wheel = std::get_if<event::MouseWheel>(&e)) {
                if (ui_consumed) {
                    continue;  // scrolling a UI list, not zooming the map
                }
                Vec3 p = camera.focus();
                if (cursor_x < 0.0f || !ground_at(cursor_x, cursor_y, p)) {
                    p = camera.focus();
                }
                camera.zoom_towards(p, wheel->y);
            }
        }

        const FrameStep step = clock.advance(frame_start);

        // One snapshot per frame feeds the map and every window, so they always agree (US-3.5).
        if (runner) {
            world_snapshot = runner->latest_snapshot();
            const std::uint64_t version = runner->snapshot_version();
            if (version != seen_version) {
                seen_version = version;
                const std::size_t provinces = map.province_list.size();
                std::vector<std::uint32_t> owners = pat::game::owner_palette(*world_snapshot, provinces);
                if (owners != last_owner_palette) {
                    view->set_palette(MapMode::Owners, owners);
                    last_owner_palette = std::move(owners);
                }
                std::vector<std::uint32_t> development = pat::game::development_palette(*world_snapshot, provinces);
                if (development != last_development_palette) {
                    view->set_palette(MapMode::Mood, development);
                    last_development_palette = std::move(development);
                }
                if (time_bar) {
                    time_bar->set_date(pat::game::format_date_ru(world_snapshot->today()));
                }
            }
            if (time_bar) {
                const pat::sim::Speed sp = runner->speed();
                time_bar->set_speed(sp == pat::sim::Speed::Unlimited ? 5 : static_cast<int>(sp));
            }
            character_refresh += frame_dt;
            if (character && character_refresh >= 0.25f) {
                character_refresh = 0.0f;
                if (!shown_character.valid() || !world_snapshot->is_alive(shown_character)) {
                    shown_character = pat::game::pick_character(*world_snapshot, 7);
                }
                if (shown_character.valid()) {
                    pat::ui::CharacterSnapshot next = pat::game::character_snapshot(*world_snapshot, shown_character);
                    if (!(next == character->snapshot())) {
                        character->set_snapshot(next);
                    }
                }
            }
        }
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
            if (ui && ui->reload_if_changed() && ui->context()->GetNumDocuments() > 0) {
                populate_demo(ui->context()->GetDocument(0));
            }
        }

        if (opt.demo_tooltip && tooltips && character && clock.frame_count() == 2) {
            // Scripted hover for screenshots: open a three-level tooltip on a skill that has one.
            const auto hover_center = [&](Rml::Element* el) {
                const Event ev = event::MouseMove{el->GetAbsoluteLeft() + el->GetOffsetWidth() * 0.5f,
                                                  el->GetAbsoluteTop() + el->GetOffsetHeight() * 0.5f, 0.0f, 0.0f};
                ui->handle_event(ev);
                tooltips->handle_event(ev);
                ui->update();
            };
            const auto settle = [&](int steps) {
                for (int i = 0; i < steps; ++i) {
                    tooltips->update(0.1f);
                    ui->update();
                }
            };
            Rml::ElementList rows;
            character->document()->GetElementsByClassName(rows, "skill");
            std::vector<Rml::Element*> shown;
            for (Rml::Element* r : rows) {
                if (r->IsVisible(true)) {
                    shown.push_back(r);
                }
            }
            std::size_t pick = 0;
            const auto& explanations = character->snapshot().skill_explanations;
            for (std::size_t i = 0; i < explanations.size() && i < shown.size(); ++i) {
                for (const auto& term : explanations[i].terms) {
                    if (!term.terms.empty() && !term.terms.front().terms.empty()) {
                        pick = i;
                    }
                }
            }
            if (pick < shown.size()) {
                hover_center(shown[pick]);
                settle(6);
                for (std::size_t level = 0; level < 2; ++level) {
                    if (Rml::ElementDocument* d = tooltips->level_document(level)) {
                        Rml::ElementList nested;
                        d->GetElementsByClassName(nested, "nested");
                        if (!nested.empty()) {
                            hover_center(nested.front());
                            settle(6);
                        }
                    }
                }
            }
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
            if (labels && labels_on) {
                LabelLayoutInput li;
                li.view_proj = frame.view_proj;
                li.eye = frame.eye;
                li.viewport_w = width_px;
                li.viewport_h = height_px;
                li.pixels_per_unit_at_one = height_px / (2.0f * std::tan(camera.fov_y() * 0.5f));
                renderer->push_marker("labels");
                label_stats = labels->draw(li);
                renderer->pop_marker();
                label_min = std::min(label_min, label_stats.placed);
                label_max = std::max(label_max, label_stats.placed);
            }
            renderer->pop_marker();
            if (ui) {
                renderer->push_marker("ui");
                if (character && opt.bench) {
                    // Benchmark: show the 1000-row relations list and scroll it up and down.
                    character->set_tab("relations");
                    if (Rml::ElementDocument* d = character->document()) {
                        if (Rml::Element* list = d->GetElementById("rel_list")) {
                            const float u2 = static_cast<float>(bench_frame % 300) / 300.0f;
                            const float tri = u2 < 0.5f ? u2 * 2.0f : 2.0f - u2 * 2.0f;
                            list->SetScrollTop(tri * (list->GetScrollHeight() - list->GetClientHeight()));
                        }
                    }
                }
                if (character) {
                    character->update();
                }
                if (tooltips) {
                    tooltips->update(frame_dt);
                }
                ui->update();
                ui->render();
                renderer->pop_marker();
            }
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
            if (renderer->last_gpu_ms() >= 0.0f) {
                gpu_ms.push_back(static_cast<double>(renderer->last_gpu_ms()));
            }
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
                          "Patriarch - spike | %.0f fps (%.2f ms) | %zu nodes | %zu labels | mode %s | vsync %s | %s | %s",
                          static_cast<double>(clock.fps()), static_cast<double>(clock.frame_ms()),
                          view->last_node_count(), label_stats.placed, mode_name(view->mode()), renderer->vsync() ? "on" : "off",
                          std::string(renderer->adapter_name()).c_str(), info);
            window.set_title(title);
            if (ui && ui->context()->GetNumDocuments() > 0) {
                if (Rml::Element* el = ui->context()->GetDocument(0)->GetElementById("fps")) {
                    char text[128];
                    std::snprintf(text, sizeof(text), "%.0f fps, %zu узлов, режим %s", static_cast<double>(clock.fps()),
                                  view->last_node_count(), mode_name(view->mode()));
                    el->SetInnerRML(text);
                }
            }
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
                    std::string(renderer->adapter_name()).c_str(), static_cast<unsigned>(renderer->size().w),
                    static_cast<unsigned>(renderer->size().h), map.width, map.height, map.province_list.size());
        std::printf("bench: frame ms  p50 %.2f  p95 %.2f  p99 %.2f  max %.2f\n", percentile(steady, 0.50),
                    percentile(steady, 0.95), percentile(steady, 0.99), percentile(steady, 1.0));
        if (!gpu_ms.empty()) {
            const std::vector<double> gpu_steady(gpu_ms.begin() + static_cast<std::ptrdiff_t>(gpu_ms.size() / 4), gpu_ms.end());
            std::printf("bench: GPU ms    p50 %.2f  p95 %.2f  p99 %.2f  max %.2f  (%zu samples)\n",
                        percentile(gpu_steady, 0.50), percentile(gpu_steady, 0.95), percentile(gpu_steady, 0.99),
                        percentile(gpu_steady, 1.0), gpu_steady.size());
        }
        std::printf("bench: video memory %.0f MiB (map view added %.0f MiB)\n",
                    static_cast<double>(renderer->gpu_memory_mb()),
                    static_cast<double>(renderer->gpu_memory_mb() - gpu_mb_before));
#ifdef _WIN32
        {
            PROCESS_MEMORY_COUNTERS counters{};
            if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) != 0) {
                std::printf("bench: process memory %.0f MiB working set, %.0f MiB peak\n",
                            static_cast<double>(counters.WorkingSetSize) / (1024.0 * 1024.0),
                            static_cast<double>(counters.PeakWorkingSetSize) / (1024.0 * 1024.0));
            }
        }
#endif
        std::printf("bench: terrain nodes per frame  min %zu  max %zu\n", min_nodes, max_nodes);
        std::printf("bench: map mode switch (CPU) max %.3f ms\n", max_mode_switch_ms);
        if (runner) {
            std::printf("bench: simulation at speed %d: %lld ticks (days) done, %llu snapshots published\n", opt.sim_speed,
                        static_cast<long long>(runner->ticks_done()), static_cast<unsigned long long>(runner->snapshot_version()));
        }
        if (labels) {
            std::printf("bench: labels placed per frame  min %zu  max %zu (of %zu)\n", label_min, label_max,
                        labels->labels().labels.size());
        }
    }

    PAT_LOG_INFO("spike", "exit after {} frames, {} logic steps", clock.frame_count(), logic_steps);
    return 0;
}
