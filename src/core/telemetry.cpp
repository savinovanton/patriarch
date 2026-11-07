#include "pat/core/telemetry.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace pat::core {
namespace {

std::uint64_t add(std::uint64_t a, std::uint64_t b, bool& saturated) {
    if (b > std::numeric_limits<std::uint64_t>::max() - a) {
        saturated = true;
        return std::numeric_limits<std::uint64_t>::max();
    }
    return a + b;
}

std::string quote_field(std::string_view value, bool csv = false) {
    std::string out = "\"";
    constexpr char kHex[] = "0123456789abcdef";
    for (const unsigned char c : value) {
        if (csv) {
            if (c == '"')
                out += '"';
            out += static_cast<char>(c);
        } else if (c == '"' || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c < 0x20) {
            out += "\\u00";
            out += kHex[c >> 4U];
            out += kHex[c & 0xfU];
        } else {
            out += static_cast<char>(c);
        }
    }
    return out + '"';
}

std::string json_distribution(const DurationDistribution& d) {
    return "{\"samples\":" + std::to_string(d.samples) + ",\"total_ns\":" + std::to_string(d.total_ns) +
           ",\"mean_ns\":" + std::to_string(d.mean_ns) + ",\"p50_ns\":" + std::to_string(d.p50_ns) +
           ",\"p95_ns\":" + std::to_string(d.p95_ns) + ",\"p99_ns\":" + std::to_string(d.p99_ns) +
           ",\"max_ns\":" + std::to_string(d.max_ns) + ",\"saturated\":" + (d.saturated ? "true}" : "false}");
}

}  // namespace

DurationDistribution duration_distribution(std::span<const std::uint64_t> values) {
    DurationDistribution result;
    if (values.empty())
        return result;
    std::vector<std::uint64_t> sorted(values.begin(), values.end());
    std::sort(sorted.begin(), sorted.end());
    result.samples = sorted.size();
    for (const auto ns : sorted)
        result.total_ns = add(result.total_ns, ns, result.saturated);
    result.mean_ns = result.total_ns / result.samples;
    const auto percentile = [&](std::size_t percent) {
        return sorted[(percent * sorted.size() + 99) / 100 - 1];
    };
    result.p50_ns = percentile(50);
    result.p95_ns = percentile(95);
    result.p99_ns = percentile(99);
    result.max_ns = sorted.back();
    return result;
}

Telemetry::Telemetry(std::size_t window, std::size_t max_metrics)
    : window_(window), max_metrics_(max_metrics), slots_(window * max_metrics), frame_ids_(window) {
    names_.reserve(max_metrics);
}

Result<std::unique_ptr<Telemetry>> Telemetry::create(std::size_t window, std::size_t max_metrics) {
    using R = Result<std::unique_ptr<Telemetry>>;
    if (window == 0 || window > 16384 || max_metrics == 0 || max_metrics > 128 ||
        window * max_metrics > 1'048'576) {
        return R::err({1, "telemetry needs window 1..16384, metrics 1..128, at most 1048576 slots"});
    }
    return R::ok(std::unique_ptr<Telemetry>(new Telemetry(window, max_metrics)));
}

Result<MetricId> Telemetry::register_metric(std::string_view name) {
    using R = Result<MetricId>;
    if (active_ || name.empty() || name.size() > 256)
        return R::err({1, "register a nonempty metric (<=256 bytes) between frames"});
    for (std::size_t i = 0; i < names_.size(); ++i) {
        if (names_[i] == name)
            return R::ok(MetricId(static_cast<std::uint32_t>(i)));
    }
    if (names_.size() == max_metrics_)
        return R::err({1, "telemetry metric capacity reached"});
    names_.emplace_back(name);
    return R::ok(MetricId(static_cast<std::uint32_t>(names_.size() - 1)));
}

bool Telemetry::begin_frame(std::int64_t frame_id) {
    if (active_)
        return false;
    for (std::size_t i = 0; i < names_.size(); ++i)
        slots_[i * window_ + cursor_] = {};
    frame_ids_[cursor_] = frame_id;
    active_ = true;
    return true;
}

void Telemetry::end_frame() {
    if (!active_)
        return;
    active_ = false;
    cursor_ = (cursor_ + 1) % window_;
    frames_ = std::min(frames_ + 1, window_);
}

bool Telemetry::record(MetricId metric, std::uint64_t duration_ns) {
    if (!active_ || !metric.valid() || metric.index() >= names_.size())
        return false;
    auto& slot = slots_[metric.index() * window_ + cursor_];
    slot.duration_ns = add(slot.duration_ns, duration_ns, saturated_);
    slot.calls = add(slot.calls, 1, saturated_);
    return true;
}

void Telemetry::clear() {
    if (active_)
        return;
    std::fill(slots_.begin(), slots_.end(), TelemetrySample{});
    std::fill(frame_ids_.begin(), frame_ids_.end(), 0);
    cursor_ = frames_ = 0;
    saturated_ = false;
}

TelemetrySnapshot Telemetry::snapshot(bool include_history) const {
    TelemetrySnapshot result;
    result.window_capacity = window_;
    result.saturated = saturated_;
    // A snapshot is a boundary operation: never expose the incomplete/overwritten active slot.
    if (active_)
        return result;
    result.frames = frames_;
    const auto first = (cursor_ + window_ - frames_) % window_;
    if (frames_ > 0) {
        result.first_frame = frame_ids_[first];
        result.last_frame = frame_ids_[(cursor_ + window_ - 1) % window_];
    }
    if (include_history) {
        for (std::size_t i = 0; i < frames_; ++i)
            result.frame_ids.push_back(frame_ids_[(first + i) % window_]);
    }
    for (std::size_t metric = 0; metric < names_.size(); ++metric) {
        MetricSummary summary;
        summary.name = names_[metric];
        std::vector<std::uint64_t> all, active;
        all.reserve(frames_);
        active.reserve(frames_);
        for (std::size_t i = 0; i < frames_; ++i) {
            const auto sample = slots_[metric * window_ + (first + i) % window_];
            all.push_back(sample.duration_ns);
            summary.calls = add(summary.calls, sample.calls, result.saturated);
            if (sample.calls > 0)
                active.push_back(sample.duration_ns);
            if (include_history)
                summary.history.push_back(sample);
            summary.last = sample;
        }
        summary.active_ticks = active.size();
        summary.per_tick = duration_distribution(all);
        summary.active = duration_distribution(active);
        result.saturated = result.saturated || summary.per_tick.saturated || summary.active.saturated;
        result.metrics.push_back(std::move(summary));
    }
    return result;
}

TelemetryFrame::TelemetryFrame(Telemetry* telemetry, std::int64_t frame_id) {
    if (telemetry && telemetry->begin_frame(frame_id))
        telemetry_ = telemetry;
}
TelemetryFrame::~TelemetryFrame() {
    if (telemetry_)
        telemetry_->end_frame();
}

ScopedTimer::ScopedTimer(Telemetry* telemetry, MetricId metric) {
    if (telemetry && telemetry->active() && metric.valid()) {
        telemetry_ = telemetry;
        include(metric);
        begin_ = std::chrono::steady_clock::now();
    }
}
void ScopedTimer::include(MetricId metric) {
    if (!telemetry_ || !metric.valid() || count_ == metrics_.size())
        return;
    if (std::find(metrics_.begin(), metrics_.begin() + static_cast<std::ptrdiff_t>(count_), metric) !=
        metrics_.begin() + static_cast<std::ptrdiff_t>(count_))
        return;
    metrics_[count_++] = metric;
}
ScopedTimer::~ScopedTimer() {
    if (!telemetry_)
        return;
    const auto ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin_)
            .count();
    for (std::size_t i = 0; i < count_; ++i)
        (void)telemetry_->record(metrics_[i], static_cast<std::uint64_t>(std::max<std::int64_t>(0, ns)));
}

std::string telemetry_json(const TelemetrySnapshot& s) {
    std::string out =
        "{\"schema\":1,\"unit\":\"ns\",\"window_capacity\":" + std::to_string(s.window_capacity) +
        ",\"frames\":" + std::to_string(s.frames) + ",\"first_frame\":" + std::to_string(s.first_frame) +
        ",\"last_frame\":" + std::to_string(s.last_frame) +
        ",\"saturated\":" + (s.saturated ? "true" : "false") + ",\"frame_ids\":[";
    for (std::size_t i = 0; i < s.frame_ids.size(); ++i)
        out += (i ? "," : "") + std::to_string(s.frame_ids[i]);
    out += "],\"metrics\":[";
    for (std::size_t i = 0; i < s.metrics.size(); ++i) {
        const auto& m = s.metrics[i];
        if (i)
            out += ',';
        out += "{\"name\":" + quote_field(m.name) + ",\"calls\":" + std::to_string(m.calls) +
               ",\"active_ticks\":" + std::to_string(m.active_ticks) +
               ",\"last_ns\":" + std::to_string(m.last.duration_ns) +
               ",\"last_calls\":" + std::to_string(m.last.calls) +
               ",\"per_tick\":" + json_distribution(m.per_tick) +
               ",\"active\":" + json_distribution(m.active) + ",\"history\":[";
        for (std::size_t j = 0; j < m.history.size(); ++j) {
            if (j)
                out += ',';
            out += "{\"duration_ns\":" + std::to_string(m.history[j].duration_ns) +
                   ",\"calls\":" + std::to_string(m.history[j].calls) + '}';
        }
        out += "]}";
    }
    return out + "]}\n";
}

std::string telemetry_csv(const TelemetrySnapshot& s) {
    std::string out =
        "schema,unit,window_capacity,frames,first_frame,last_frame,metric,kind,calls,active_ticks,last_ns,"
        "last_calls,samples,total_ns,mean_ns,p50_ns,p95_ns,p99_ns,max_ns,saturated\n";
    for (const auto& m : s.metrics) {
        for (const bool active : {false, true}) {
            const auto& d = active ? m.active : m.per_tick;
            out += "1,ns," + std::to_string(s.window_capacity) + ',' + std::to_string(s.frames) + ',' +
                   std::to_string(s.first_frame) + ',' + std::to_string(s.last_frame) + ',' +
                   quote_field(m.name, true) + ',' + (active ? "active" : "per_tick") + ',' +
                   std::to_string(m.calls) + ',' + std::to_string(m.active_ticks) + ',' +
                   std::to_string(m.last.duration_ns) + ',' + std::to_string(m.last.calls) + ',' +
                   std::to_string(d.samples) + ',' + std::to_string(d.total_ns) + ',' +
                   std::to_string(d.mean_ns) + ',' + std::to_string(d.p50_ns) + ',' +
                   std::to_string(d.p95_ns) + ',' + std::to_string(d.p99_ns) + ',' +
                   std::to_string(d.max_ns) + ',' + ((s.saturated || d.saturated) ? "true\n" : "false\n");
        }
    }
    return out;
}

}  // namespace pat::core
