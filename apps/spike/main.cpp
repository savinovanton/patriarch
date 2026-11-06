// Engine spike application (E3). T01: window, input, DPI, fixed-step logic + free render loop, FPS.
// Later tasks add the D3D11 renderer, the 3D map and RmlUi on top of this loop.

#include "pat/core/log.hpp"
#include "pat/engine/dpi.hpp"
#include "pat/engine/frame_clock.hpp"
#include "pat/engine/window.hpp"

#include <SDL3/SDL_timer.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <variant>
#include <vector>

namespace {

struct Options {
    std::int64_t max_frames = -1;  // -1: run until closed
    bool hidden = false;
};

bool parse_options(int argc, char** argv, Options& out) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--frames" && i + 1 < argc) {
            out.max_frames = std::atoll(argv[++i]);
        } else if (arg == "--hidden") {
            out.hidden = true;
        } else {
            std::fprintf(stderr, "usage: patriarch_spike [--frames N] [--hidden]\n");
            return false;
        }
    }
    return true;
}

std::int64_t now_ns() {
    return static_cast<std::int64_t>(SDL_GetTicksNS());
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

    FrameClock clock;
    std::vector<Event> events;
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
                }
            } else if (const auto* resized = std::get_if<event::Resized>(&e)) {
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

        // Render goes here (E3-T02). Until then, yield the CPU instead of spinning.
        SDL_DelayNS(1'000'000);

        if (clock.fps() > 0.0f && static_cast<std::int64_t>(clock.frame_count()) - last_title_frame_count >= 30) {
            last_title_frame_count = static_cast<std::int64_t>(clock.frame_count());
            const PixelSize px = window.pixel_size();
            char title[160];
            std::snprintf(title, sizeof(title), "Patriarch - spike | %.0f fps (%.2f ms) | %dx%d px | scale %.2f",
                          static_cast<double>(clock.fps()), static_cast<double>(clock.frame_ms()), px.w, px.h,
                          static_cast<double>(ui_scale));
            window.set_title(title);
        }

        if (opt.max_frames >= 0 && static_cast<std::int64_t>(clock.frame_count()) >= opt.max_frames) {
            running = false;
        }
    }

    PAT_LOG_INFO("spike", "exit after {} frames, {} logic steps", clock.frame_count(), logic_steps);
    return 0;
}
