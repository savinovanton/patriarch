// Engine spike application (E3). T01: window, input, DPI, fixed-step logic + free render loop, FPS.
// T02: Direct3D 11 renderer behind IRenderer (coloured quads, shader hot reload).
// Later tasks add the 3D map and RmlUi on top of this loop.

#include "pat/core/log.hpp"
#include "pat/engine/dpi.hpp"
#include "pat/engine/frame_clock.hpp"
#include "pat/engine/renderer.hpp"
#include "pat/engine/window.hpp"

#include <SDL3/SDL_timer.h>

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

constexpr int kScancodeV = 25;
constexpr int kScancodeF5 = 62;

struct Options {
    std::int64_t max_frames = -1;  // -1: run until closed
    bool hidden = false;           // hidden window + offscreen software renderer (headless smoke run)
    bool no_vsync = false;
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
        } else {
            std::fprintf(stderr, "usage: patriarch_spike [--frames N] [--hidden] [--no-vsync]\n");
            return false;
        }
    }
    return true;
}

std::int64_t now_ns() {
    return static_cast<std::int64_t>(SDL_GetTicksNS());
}

// A little animated scene made of quads: a translucent panel and a row of bobbing squares.
void build_scene(std::vector<pat::engine::Quad>& quads, pat::engine::PixelSize size, float time_s) {
    using pat::engine::Quad;
    quads.clear();
    const auto w = static_cast<float>(size.w);
    const auto h = static_cast<float>(size.h);
    quads.push_back(Quad{w * 0.1f, h * 0.15f, w * 0.8f, h * 0.7f, {0.12f, 0.16f, 0.22f, 0.85f}});
    constexpr int kCount = 12;
    const float cell = w * 0.8f / kCount;
    for (int i = 0; i < kCount; ++i) {
        const float phase = time_s * 2.0f + static_cast<float>(i) * 0.5f;
        const float bob = std::sin(phase) * h * 0.12f;
        const float hue = static_cast<float>(i) / kCount;
        quads.push_back(Quad{w * 0.1f + cell * static_cast<float>(i) + cell * 0.1f, h * 0.5f - cell * 0.4f + bob,
                             cell * 0.8f, cell * 0.8f,
                             {0.5f + 0.5f * std::sin(hue * 6.2832f), 0.5f + 0.5f * std::sin(hue * 6.2832f + 2.1f),
                              0.5f + 0.5f * std::sin(hue * 6.2832f + 4.2f), 1.0f}});
    }
    quads.push_back(Quad{w * 0.35f, h * 0.2f, w * 0.3f, h * 0.1f, {1.0f, 1.0f, 1.0f, 0.35f}});
}

}  // namespace

int main(int argc, char** argv) {
    using namespace pat::engine;

    Options opt;
    if (!parse_options(argc, argv, opt)) {
        return 2;
    }

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

    FrameClock clock;
    std::vector<Event> events;
    std::vector<Quad> quads;
    bool running = true;
    std::uint64_t logic_steps = 0;
    float ui_scale = effective_ui_scale(window.display_scale());
    std::int64_t last_title_frame_count = 0;

    while (running) {
        events.clear();
        window.poll_events(events);
        for (const Event& e : events) {
            if (std::holds_alternative<event::Quit>(e)) {
                running = false;
            } else if (const auto* key = std::get_if<event::Key>(&e)) {
                if (key->down && key->scancode == scancode::escape) {
                    running = false;
                } else if (key->down && !key->repeat && key->scancode == scancode::f11) {
                    window.set_fullscreen(!window.is_fullscreen());
                } else if (key->down && !key->repeat && key->scancode == kScancodeV) {
                    renderer->set_vsync(!renderer->vsync());
                    PAT_LOG_INFO("spike", "vsync {}", renderer->vsync() ? "on" : "off");
                } else if (key->down && !key->repeat && key->scancode == kScancodeF5) {
                    renderer->reload_shaders_if_changed();
                }
            } else if (const auto* resized = std::get_if<event::Resized>(&e)) {
                renderer->resize({resized->pixel_w, resized->pixel_h});
                const PixelSize logical = logical_size({resized->pixel_w, resized->pixel_h}, ui_scale);
                PAT_LOG_INFO("spike", "resized to {}x{} px ({}x{} logical, scale {:.3f})", resized->pixel_w,
                             resized->pixel_h, logical.w, logical.h, ui_scale);
            } else if (const auto* scale = std::get_if<event::ScaleChanged>(&e)) {
                ui_scale = effective_ui_scale(scale->scale);
                PAT_LOG_INFO("spike", "display scale changed to {:.3f}", ui_scale);
            }
        }

        const FrameStep step = clock.advance(now_ns());
        logic_steps += static_cast<std::uint64_t>(step.fixed_steps);  // UI/camera logic runs here (fixed 60 Hz)

        // Pick up edited shaders a few times per second.
        if (clock.frame_count() % 15 == 0) {
            renderer->reload_shaders_if_changed();
        }

        if (renderer->begin_frame({0.05f, 0.06f, 0.09f, 1.0f})) {
            const float time_s = (static_cast<float>(logic_steps) + step.alpha) / 60.0f;
            build_scene(quads, renderer->size(), time_s);
            renderer->push_marker("scene");
            renderer->draw_quads(quads);
            renderer->pop_marker();
            renderer->end_frame();
        } else {
            SDL_DelayNS(16'000'000);  // Minimized or device lost: do not spin.
        }
        if (opt.hidden) {
            SDL_DelayNS(1'000'000);
        }

        if (clock.fps() > 0.0f && static_cast<std::int64_t>(clock.frame_count()) - last_title_frame_count >= 30) {
            last_title_frame_count = static_cast<std::int64_t>(clock.frame_count());
            const PixelSize px = window.pixel_size();
            char title[256];
            std::snprintf(title, sizeof(title), "Patriarch - spike | %.0f fps (%.2f ms) | %dx%d px | scale %.2f | vsync %s | %s",
                          static_cast<double>(clock.fps()), static_cast<double>(clock.frame_ms()), px.w, px.h,
                          static_cast<double>(ui_scale), renderer->vsync() ? "on" : "off",
                          std::string(renderer->adapter_name()).c_str());
            window.set_title(title);
        }

        if (opt.max_frames >= 0 && static_cast<std::int64_t>(clock.frame_count()) >= opt.max_frames) {
            running = false;
        }
    }

    PAT_LOG_INFO("spike", "exit after {} frames, {} logic steps", clock.frame_count(), logic_steps);
    return 0;
}
