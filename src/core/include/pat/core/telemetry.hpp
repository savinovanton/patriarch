#pragma once

#include "pat/core/ids.hpp"
#include "pat/core/profile.hpp"
#include "pat/core/result.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pat::core {

using MetricId = StrongId<struct TelemetryMetricTag>;

struct DurationDistribution {
    std::uint64_t samples = 0;
    std::uint64_t total_ns = 0;
    std::uint64_t mean_ns = 0;
    std::uint64_t p50_ns = 0;
    std::uint64_t p95_ns = 0;
    std::uint64_t p99_ns = 0;
    std::uint64_t max_ns = 0;
    bool saturated = false;
};

// Nearest-rank percentiles. Allocates/sorts a copy; use outside measured scopes.
[[nodiscard]] DurationDistribution duration_distribution(std::span<const std::uint64_t> values);

struct TelemetrySample {
    std::uint64_t duration_ns = 0;  // Inclusive sum of calls in one frame.
    std::uint64_t calls = 0;
};

struct MetricSummary {
    std::string name;
    std::uint64_t calls = 0;
    std::uint64_t active_ticks = 0;
    TelemetrySample last;
    DurationDistribution per_tick;  // Includes zeroes on frames with no calls.
    DurationDistribution active;    // Only frames with calls; NOT per-invocation quantiles.
    std::vector<TelemetrySample> history;
};

// Owned detached value: safe to publish as shared_ptr<const TelemetrySnapshot> to readers.
struct TelemetrySnapshot {
    std::size_t window_capacity = 0;
    std::size_t frames = 0;
    std::int64_t first_frame = 0;
    std::int64_t last_frame = 0;
    bool saturated = false;
    std::vector<MetricSummary> metrics;   // Registration order, never hash-table iteration.
    std::vector<std::int64_t> frame_ids;  // Present only when history was requested.
};

[[nodiscard]] std::string telemetry_json(const TelemetrySnapshot& snapshot);
[[nodiscard]] std::string telemetry_csv(const TelemetrySnapshot& snapshot);

// One writer only. This is diagnostic state, never saved, hashed or used for gameplay/WU.
// All slot storage is allocated at creation. Register names BETWEEN frames; record/begin/end
// allocate nothing. Snapshot/export belong outside the tick; readers use detached snapshots.
class Telemetry {
public:
    [[nodiscard]] static Result<std::unique_ptr<Telemetry>> create(std::size_t window,
                                                                   std::size_t max_metrics = 64);
    Telemetry(const Telemetry&) = delete;
    Telemetry& operator=(const Telemetry&) = delete;

    [[nodiscard]] Result<MetricId> register_metric(std::string_view name);
    [[nodiscard]] bool begin_frame(std::int64_t frame_id);
    void end_frame();
    [[nodiscard]] bool record(MetricId metric, std::uint64_t duration_ns);
    void clear();  // Keeps registration/IDs. Call between frames.
    [[nodiscard]] bool active() const noexcept { return active_; }
    [[nodiscard]] std::size_t window_capacity() const noexcept { return window_; }
    [[nodiscard]] TelemetrySnapshot snapshot(bool include_history = false) const;

private:
    Telemetry(std::size_t window, std::size_t max_metrics);
    std::size_t window_;
    std::size_t max_metrics_;
    std::vector<std::string> names_;
    std::vector<TelemetrySample> slots_;  // Metric-major, fixed stride window_.
    std::vector<std::int64_t> frame_ids_;
    std::size_t cursor_ = 0;
    std::size_t frames_ = 0;
    bool active_ = false;
    bool saturated_ = false;
};

class TelemetryFrame {
public:
    TelemetryFrame(Telemetry* telemetry, std::int64_t frame_id);
    ~TelemetryFrame();
    TelemetryFrame(const TelemetryFrame&) = delete;
    TelemetryFrame& operator=(const TelemetryFrame&) = delete;

private:
    Telemetry* telemetry_ = nullptr;
};

class ScopedTimer {
public:
    // Null/inactive collectors do not read the clock. Collector must outlive this scope.
    ScopedTimer(Telemetry* telemetry, MetricId metric);
    ~ScopedTimer();
    ScopedTimer(const ScopedTimer&) = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;
    // Record the SAME inclusive duration under up to four additional tags (e.g. cadences).
    // Duplicate/invalid/full tags are ignored; this never affects simulation control flow.
    void include(MetricId metric);

private:
    Telemetry* telemetry_ = nullptr;
    std::chrono::steady_clock::time_point begin_{};
    std::array<MetricId, 5> metrics_{};
    std::size_t count_ = 0;
};

}  // namespace pat::core

// Explicit collector/registered ID, no global/TLS state or runtime name lookup in hot code.
// The literal also names a Tracy zone when that optional client is enabled.
#define PAT_DETAIL_TIMER_JOIN_(a, b) a##b
#define PAT_DETAIL_TIMER_JOIN(a, b) PAT_DETAIL_TIMER_JOIN_(a, b)
#define PAT_SCOPED_TIMER(collector, metric, literal)                                                         \
    PAT_PROFILE_SCOPE(literal);                                                                              \
    ::pat::core::ScopedTimer PAT_DETAIL_TIMER_JOIN(pat_telemetry_timer_, __LINE__)(collector, metric)
