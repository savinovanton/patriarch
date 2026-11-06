#include "pat/engine/window.hpp"

#include <doctest/doctest.h>

#include <cstdlib>
#include <variant>
#include <vector>

namespace {

// Use SDL's offscreen "dummy" video driver so the test runs on machines without a display (CI).
void use_dummy_video_driver() {
#ifdef _WIN32
    _putenv_s("SDL_VIDEODRIVER", "dummy");
#else
    setenv("SDL_VIDEODRIVER", "dummy", 1);
#endif
}

}  // namespace

TEST_CASE("Window: create, query, resize-free event pump and close request") {
    use_dummy_video_driver();

    pat::engine::WindowDesc desc;
    desc.title = "Patriarch test window";
    desc.logical_width = 640;
    desc.logical_height = 360;
    desc.hidden = true;

    auto created = pat::engine::Window::create(desc);
    REQUIRE(created.has_value());
    pat::engine::Window window = std::move(created).value();

    CHECK(window.video_driver() == "dummy");
    const auto size = window.pixel_size();
    CHECK(size.w > 0);
    CHECK(size.h > 0);
    CHECK(window.display_scale() > 0.0f);

    window.set_title("Renamed");  // must not crash
    window.set_text_input(true);
    window.set_text_input(false);

    std::vector<pat::engine::Event> events;
    window.poll_events(events);
    // No spurious quit before a close request.
    for (const auto& e : events) {
        CHECK_FALSE(std::holds_alternative<pat::engine::event::Quit>(e));
    }

    events.clear();
    window.request_close();
    window.poll_events(events);
    bool quit = false;
    for (const auto& e : events) {
        quit = quit || std::holds_alternative<pat::engine::event::Quit>(e);
    }
    CHECK(quit);
}

TEST_CASE("Window: is movable and can be created again after destruction") {
    use_dummy_video_driver();

    pat::engine::WindowDesc desc;
    desc.hidden = true;
    for (int i = 0; i < 2; ++i) {
        auto first = pat::engine::Window::create(desc);
        REQUIRE(first.has_value());
        pat::engine::Window a = std::move(first).value();
        pat::engine::Window b = std::move(a);
        CHECK(b.pixel_size().w > 0);
    }
}
