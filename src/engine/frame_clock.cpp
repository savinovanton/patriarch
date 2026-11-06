#include "pat/engine/frame_clock.hpp"

#include "pat/core/assert.hpp"

#include <algorithm>

namespace pat::engine {

FrameClock::FrameClock(std::int64_t fixed_step_ns,
                       int max_steps_per_frame,
                       std::int64_t max_frame_ns) noexcept
    : fixed_step_ns_(fixed_step_ns), max_steps_(max_steps_per_frame), max_frame_ns_(max_frame_ns) {
    PAT_ASSERT(fixed_step_ns_ > 0, "fixed step must be positive");
    PAT_ASSERT(max_steps_ > 0, "max steps per frame must be positive");
    PAT_ASSERT(max_frame_ns_ > 0, "max frame time must be positive");
}

FrameStep FrameClock::advance(std::int64_t now_ns) noexcept {
    ++frame_count_;
    if (!started_) {
        started_ = true;
        last_ns_ = now_ns;
        window_start_ns_ = now_ns;
        window_frames_ = 0;
        return {};
    }

    const std::int64_t dt = std::clamp<std::int64_t>(now_ns - last_ns_, 0, max_frame_ns_);
    last_ns_ = now_ns;

    accumulator_ns_ += dt;
    int steps = 0;
    while (accumulator_ns_ >= fixed_step_ns_ && steps < max_steps_) {
        accumulator_ns_ -= fixed_step_ns_;
        ++steps;
    }
    if (accumulator_ns_ >= fixed_step_ns_) {
        accumulator_ns_ = 0;  // Could not catch up within the cap: drop the backlog.
    }

    ++window_frames_;
    const std::int64_t elapsed = now_ns - window_start_ns_;
    if (elapsed >= kFpsWindowNs) {
        const double seconds = static_cast<double>(elapsed) / static_cast<double>(kNsPerSecond);
        fps_ = static_cast<float>(static_cast<double>(window_frames_) / seconds);
        frame_ms_ = static_cast<float>(seconds * 1000.0 / static_cast<double>(window_frames_));
        window_start_ns_ = now_ns;
        window_frames_ = 0;
    }

    FrameStep out;
    out.frame_ns = dt;
    out.fixed_steps = steps;
    out.alpha =
        static_cast<float>(static_cast<double>(accumulator_ns_) / static_cast<double>(fixed_step_ns_));
    return out;
}

}  // namespace pat::engine
