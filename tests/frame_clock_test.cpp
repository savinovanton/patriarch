#include "pat/engine/dpi.hpp"
#include "pat/engine/frame_clock.hpp"

#include <doctest/doctest.h>

using pat::engine::FrameClock;

namespace {
constexpr std::int64_t kMs = 1'000'000;
constexpr std::int64_t kStep = FrameClock::kNsPerSecond / 60;
}  // namespace

TEST_CASE("FrameClock: the first frame only starts the clock") {
    FrameClock clock;
    const auto s = clock.advance(5'000 * kMs);
    CHECK(s.fixed_steps == 0);
    CHECK(s.frame_ns == 0);
    CHECK(clock.frame_count() == 1);
}

TEST_CASE("FrameClock: fixed steps follow real time at any frame rate") {
    // Use a 16 ms logic step so frame deltas divide it exactly (no rounding noise in the test).
    // 250 fps rendering (4 ms frames): one logic step every 4 frames; 960 ms -> 60 steps.
    FrameClock clock(16 * kMs);
    std::int64_t now = 0;
    int steps = 0;
    (void)clock.advance(now);
    for (int i = 0; i < 240; ++i) {
        now += 4 * kMs;
        steps += clock.advance(now).fixed_steps;
    }
    CHECK(steps == 60);

    // 31 fps rendering (32 ms frames): two logic steps per frame; 960 ms -> 60 steps.
    FrameClock slow(16 * kMs);
    now = 0;
    steps = 0;
    (void)slow.advance(now);
    for (int i = 0; i < 30; ++i) {
        now += 32 * kMs;
        const auto s = slow.advance(now);
        CHECK(s.fixed_steps == 2);
        steps += s.fixed_steps;
    }
    CHECK(steps == 60);
}

TEST_CASE("FrameClock: alpha is the leftover fraction of a step") {
    FrameClock clock;
    (void)clock.advance(0);
    const auto s = clock.advance(kStep + kStep / 4);
    CHECK(s.fixed_steps == 1);
    CHECK(s.alpha == doctest::Approx(0.25f).epsilon(0.001));
}

TEST_CASE("FrameClock: long stalls are clamped and the backlog is dropped") {
    FrameClock clock(kStep, /*max_steps_per_frame=*/3, /*max_frame_ns=*/FrameClock::kNsPerSecond / 4);
    (void)clock.advance(0);
    // A 10 second stall (breakpoint): at most 3 steps, then no catch-up debt afterwards.
    const auto stall = clock.advance(10 * FrameClock::kNsPerSecond);
    CHECK(stall.frame_ns == FrameClock::kNsPerSecond / 4);
    CHECK(stall.fixed_steps == 3);
    const auto next = clock.advance(10 * FrameClock::kNsPerSecond + kStep);
    CHECK(next.fixed_steps == 1);
}

TEST_CASE("FrameClock: time going backwards is treated as zero") {
    FrameClock clock;
    (void)clock.advance(100 * kMs);
    const auto s = clock.advance(50 * kMs);
    CHECK(s.frame_ns == 0);
    CHECK(s.fixed_steps == 0);
}

TEST_CASE("FrameClock: fps is measured over half-second windows") {
    FrameClock clock;
    std::int64_t now = 0;
    (void)clock.advance(now);
    CHECK(clock.fps() == 0.0f);
    for (int i = 0; i < 120; ++i) {  // 2 seconds at 60 fps
        now += kStep;
        (void)clock.advance(now);
    }
    CHECK(clock.fps() == doctest::Approx(60.0f).epsilon(0.02));
    CHECK(clock.frame_ms() == doctest::Approx(16.67f).epsilon(0.02));
}

TEST_CASE("dpi: effective scale is clamped and snapped") {
    using namespace pat::engine;
    CHECK(effective_ui_scale(1.0f) == 1.0f);
    CHECK(effective_ui_scale(1.25f) == 1.25f);
    CHECK(effective_ui_scale(1.5f) == 1.5f);
    CHECK(effective_ui_scale(2.0f) == 2.0f);
    CHECK(effective_ui_scale(1.0f, 1.2f) == 1.25f);  // 1.2 snaps to the nearest 1/8 step
    CHECK(effective_ui_scale(0.1f) == kMinUiScale);
    CHECK(effective_ui_scale(10.0f) == kMaxUiScale);
}

TEST_CASE("dpi: logical and physical sizes round-trip for common scales") {
    using namespace pat::engine;
    for (const float scale : {1.0f, 1.25f, 1.5f, 1.75f, 2.0f}) {
        const PixelSize logical{1280, 720};
        const PixelSize px = physical_size(logical, scale);
        CHECK(logical_size(px, scale) == logical);
    }
    CHECK(physical_size({1280, 720}, 1.5f) == PixelSize{1920, 1080});
    CHECK(logical_size({1920, 1080}, 2.0f) == PixelSize{960, 540});
    CHECK(logical_size({0, 0}, 1.0f) == PixelSize{1, 1});
}
