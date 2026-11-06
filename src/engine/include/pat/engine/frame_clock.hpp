#pragma once

#include <cstdint>

namespace pat::engine {

/// Result of advancing the clock by one rendered frame.
struct FrameStep {
    std::int64_t frame_ns = 0;  ///< Real time since the previous frame (clamped), nanoseconds.
    int fixed_steps = 0;        ///< Number of fixed logic steps to run before rendering this frame.
    float alpha = 0.0f;         ///< Fraction of a fixed step left over, for render interpolation [0, 1).
};

/// Decouples fixed-step UI/logic updates from the free-running render loop (E3-T01) and measures FPS.
///
/// Time is passed in explicitly (monotonic nanoseconds) so the clock is deterministic and testable.
/// It never touches simulation time: the simulation runs on its own thread (ADR-0001).
class FrameClock {
public:
    static constexpr std::int64_t kNsPerSecond = 1'000'000'000;

    /// @param fixed_step_ns        Length of one logic step (default 1/60 s).
    /// @param max_steps_per_frame  Cap on catch-up steps; excess time is dropped (avoids a death spiral).
    /// @param max_frame_ns         Longest frame delta taken into account (e.g. after a breakpoint).
    explicit FrameClock(std::int64_t fixed_step_ns = kNsPerSecond / 60,
                        int max_steps_per_frame = 5,
                        std::int64_t max_frame_ns = kNsPerSecond / 4) noexcept;

    /// Registers a new frame at monotonic time `now_ns`. The first call only starts the clock.
    [[nodiscard]] FrameStep advance(std::int64_t now_ns) noexcept;

    /// Frames per second averaged over the last measurement window (0 until the first window ends).
    [[nodiscard]] float fps() const noexcept { return fps_; }
    /// Average frame time over the last measurement window, milliseconds.
    [[nodiscard]] float frame_ms() const noexcept { return frame_ms_; }
    /// Total number of frames registered (including the first).
    [[nodiscard]] std::uint64_t frame_count() const noexcept { return frame_count_; }
    [[nodiscard]] std::int64_t fixed_step_ns() const noexcept { return fixed_step_ns_; }

private:
    static constexpr std::int64_t kFpsWindowNs = kNsPerSecond / 2;

    std::int64_t fixed_step_ns_;
    int max_steps_;
    std::int64_t max_frame_ns_;

    bool started_ = false;
    std::int64_t last_ns_ = 0;
    std::int64_t accumulator_ns_ = 0;

    std::int64_t window_start_ns_ = 0;
    std::uint32_t window_frames_ = 0;
    float fps_ = 0.0f;
    float frame_ms_ = 0.0f;
    std::uint64_t frame_count_ = 0;
};

}  // namespace pat::engine
