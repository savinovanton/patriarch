#include "pat/core/hash_stream.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>
#include <string>
#include <string_view>

#include "triggers.hpp"

#if defined(_WIN32)
#include <windows.h>
#elif defined(__linux__)
#include <sched.h>
#endif

namespace {

// Single-threaded comparison: restrict this subprocess, never system-wide CPU settings.
class Affinity {
public:
    bool pin() {
#if defined(_WIN32)
        DWORD_PTR system = 0;
        if (GetProcessAffinityMask(GetCurrentProcess(), &original_, &system) == 0 || original_ == 0) {
            return false;
        }
        DWORD_PTR selected = original_ & (~original_ + 1);
        while ((selected >> cpu) != 1) {
            ++cpu;
        }
        return SetProcessAffinityMask(GetCurrentProcess(), selected) != 0;
#elif defined(__linux__)
        if (sched_getaffinity(0, sizeof(original_), &original_) != 0) {
            return false;
        }
        cpu_set_t selected;
        CPU_ZERO(&selected);
        for (; cpu < CPU_SETSIZE; ++cpu) {
            if (CPU_ISSET(cpu, &original_)) {
                CPU_SET(cpu, &selected);
                return sched_setaffinity(0, sizeof(selected), &selected) == 0;
            }
        }
        return false;
#else
        return false;
#endif
    }
    ~Affinity() {
#if defined(_WIN32)
        if (original_ != 0) {
            (void)SetProcessAffinityMask(GetCurrentProcess(), original_);
        }
#elif defined(__linux__)
        if (cpu < CPU_SETSIZE) {
            (void)sched_setaffinity(0, sizeof(original_), &original_);
        }
#endif
    }
    int cpu = 0;

private:
#if defined(_WIN32)
    DWORD_PTR original_ = 0;
#elif defined(__linux__)
    cpu_set_t original_{};
#endif
};

struct Stats {
    std::size_t samples = 0;
    double mean = 0;
    double p50 = 0;
    double p95 = 0;
    double p99 = 0;
    double maximum = 0;
};

Stats statistics(std::vector<double> values) {
    if (values.empty()) {
        return {};
    }
    std::sort(values.begin(), values.end());
    auto percentile = [&](double p) {
        return values[static_cast<std::size_t>(std::ceil(p * static_cast<double>(values.size()))) - 1];
    };
    return {values.size(),
            std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size()),
            percentile(0.5),
            percentile(0.95),
            percentile(0.99),
            values.back()};
}

void print_stats(const Stats& s) {
    std::cout << "{\"samples\":" << s.samples << ",\"mean\":" << s.mean << ",\"p50\":" << s.p50
              << ",\"p95\":" << s.p95 << ",\"p99\":" << s.p99 << ",\"max\":" << s.maximum << '}';
}

std::string hex_hash(std::uint64_t value) {
    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(16) << value;
    return out.str();
}

int fail(const std::string& message) {
    std::fprintf(stderr, "trigger_spike: %s\n", message.c_str());
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    namespace ts = pat::trigger_spike;
    ts::Config config;
    int days = 365;
    int repetitions = 3;
    for (int i = 1; i < argc; ++i) {
        const std::string_view flag(argv[i]);
        if (flag == "--help") {
            std::cout << "trigger_spike [--characters N] [--changed-permille N] [--sample-permille N]\n"
                         "  [--days N] [--repetitions N] [--seed N]\n"
                         "500 definitions / 400 provinces; JSON compares all three algorithms.\n";
            return 0;
        }
        if (++i == argc) {
            return fail("missing value for " + std::string(flag));
        }
        const std::string_view value(argv[i]);
        std::uint64_t n = 0;
        const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), n);
        if (error != std::errc{} || end != value.data() + value.size()) {
            return fail("invalid unsigned value for " + std::string(flag));
        }
        if (flag == "--seed") {
            config.seed = n;
        } else if (flag == "--characters" && n >= 1 && n <= 15000) {
            config.characters = static_cast<std::uint32_t>(n);
        } else if (flag == "--changed-permille" && n <= 1000) {
            config.changed_permille = static_cast<std::uint32_t>(n);
        } else if (flag == "--sample-permille" && n <= 1000) {
            config.sample_permille = static_cast<std::uint32_t>(n);
        } else if (flag == "--days" && n >= 1 && n <= 366) {
            days = static_cast<int>(n);
        } else if (flag == "--repetitions" && n >= 1 && n <= 10) {
            repetitions = static_cast<int>(n);
        } else {
            return fail("unknown option or out-of-range value: " + std::string(flag));
        }
    }
    Affinity affinity;
    if (!affinity.pin()) {
        return fail("cannot pin subprocess to an allowed CPU");
    }
    auto initial = ts::make_fixture(config);
    if (!initial) {
        return fail(initial.error().message);
    }
    std::array<ts::Evaluator, 3> evaluators{
        ts::Evaluator(initial.value()), ts::Evaluator(initial.value()), ts::Evaluator(initial.value())};
    std::array<std::vector<double>, 3> times;
    std::array<std::vector<double>, 3> means;
    std::array<std::uint64_t, 3> hashes{};
    std::array<ts::Counts, 3> counts{};
    std::array<std::uint64_t, 3> action_hits{};
    std::array<std::uint64_t, 3> pulse_hits{};
    ts::Audit audit(initial.value().audit_size);
    for (auto& time : times) {
        time.resize(static_cast<std::size_t>(days) * static_cast<std::size_t>(repetitions));
    }
    using Clock = std::chrono::steady_clock;
    for (int repeat = 0; repeat < repetitions; ++repeat) {
        auto fixture = initial.value();
        for (int day = 0; day < 31; ++day) {
            ts::prepare_tick(fixture);
            for (std::size_t mode = 0; mode < 3; ++mode) {
                evaluators[mode].run(fixture, static_cast<ts::Method>(mode));
            }
            fixture.world.advance_day();
        }
        fixture = initial.value();
        std::array<pat::core::HashStream, 3> digest;
        std::array<double, 3> sum{};
        counts = {};
        action_hits = {};
        pulse_hits = {};
        for (int day = 0; day < days; ++day) {
            ts::prepare_tick(fixture);  // Common producer/input changes, excluded from consumer timing.
            for (std::size_t turn = 0; turn < 3; ++turn) {
                const auto mode = (static_cast<std::size_t>(day + repeat) + turn) % 3;
                const auto begin = Clock::now();
                evaluators[mode].run(fixture, static_cast<ts::Method>(mode));
                const auto us = std::chrono::duration<double, std::micro>(Clock::now() - begin).count();
                times[mode][static_cast<std::size_t>(repeat) * static_cast<std::size_t>(days) +
                            static_cast<std::size_t>(day)] = us;
                sum[mode] += us;
                const auto& result = evaluators[mode];
                counts[mode].visited += result.counts.visited;
                counts[mode].predicates += result.counts.predicates;
                counts[mode].duplicates += result.counts.duplicates;
                action_hits[mode] += result.actions.size();
                pulse_hits[mode] += result.pulses.size();
                digest[mode].u64(ts::hit_hash(result.actions));
                digest[mode].u64(ts::hit_hash(result.pulses));
            }
            const auto& reference = evaluators[0];
            if (reference.actions != evaluators[1].actions || reference.pulses != evaluators[1].pulses ||
                reference.actions != evaluators[2].actions ||
                !std::includes(reference.pulses.begin(),
                               reference.pulses.end(),
                               evaluators[2].pulses.begin(),
                               evaluators[2].pulses.end())) {
                return fail("algorithms disagree on exact on-action/pulse results");
            }
            if (repeat == 0) {
                audit.observe(fixture, reference.pulses, day, false);
                audit.observe(fixture, evaluators[2].pulses, day, true);
            }
            fixture.world.advance_day();
        }
        for (std::size_t mode = 0; mode < 3; ++mode) {
            const auto hash = digest[mode].value();
            if (repeat != 0 && hash != hashes[mode]) {
                return fail("repetitions produced different hit streams");
            }
            hashes[mode] = hash;
            means[mode].push_back(sum[mode] / days);
        }
    }
    std::vector<double> delays;
    std::size_t qualified = 0;
    for (std::size_t i = 0; i < audit.full_first.size(); ++i) {
        if (audit.full_first[i] >= 0) {
            ++qualified;
            if (audit.sampled_first[i] >= 0) {
                delays.push_back(audit.sampled_first[i] - audit.full_first[i]);
            }
        }
    }
    std::cout << std::setprecision(12) << "{\"schema\":1,\"characters\":" << config.characters
              << ",\"provinces\":" << config.provinces << ",\"events\":" << config.events
              << ",\"changed_permille\":" << config.changed_permille
              << ",\"sample_permille\":" << config.sample_permille << ",\"seed\":" << config.seed
              << ",\"days\":" << days << ",\"repetitions\":" << repetitions
              << ",\"warmup_days\":31,\"cpu\":" << affinity.cpu << ",\"build\":\"" << PAT_TRIGGER_CONFIG
              << "\",\"methods\":{";
    constexpr std::array<std::string_view, 3> kNames{"naive", "indexed", "sampled"};
    for (std::size_t mode = 0; mode < 3; ++mode) {
        const auto stats = statistics(std::move(times[mode]));
        const auto mean = std::accumulate(means[mode].begin(), means[mode].end(), 0.0) / repetitions;
        double variance = 0;
        for (const auto value : means[mode]) {
            variance += (value - mean) * (value - mean);
        }
        const auto rsd =
            repetitions > 1 && mean > 0 ? std::sqrt(variance / (repetitions - 1)) / mean * 100 : 0;
        std::cout << (mode == 0 ? "" : ",") << '"' << kNames[mode] << "\":{\"tick_us\":";
        print_stats(stats);
        std::cout << ",\"us_per_1000_characters\":" << mean * 1000 / config.characters
                  << ",\"repeat_rsd_percent\":" << rsd
                  << ",\"stable\":" << (repetitions >= 3 && rsd <= 5 ? "true" : "false") << ",\"hash\":\""
                  << hex_hash(hashes[mode]) << "\",\"visited_per_run\":" << counts[mode].visited
                  << ",\"predicates_per_run\":" << counts[mode].predicates
                  << ",\"duplicate_candidates\":" << counts[mode].duplicates
                  << ",\"action_hits\":" << action_hits[mode] << ",\"pulse_hits\":" << pulse_hits[mode]
                  << ",\"repeat_mean_us\":[";
        for (std::size_t i = 0; i < means[mode].size(); ++i) {
            std::cout << (i == 0 ? "" : ",") << means[mode][i];
        }
        std::cout << "]}";
    }
    std::cout << "},\"pulse_audit\":{\"qualified_distinct_pairs\":" << qualified
              << ",\"sampled_distinct_pairs\":" << delays.size()
              << ",\"undetected_pairs\":" << qualified - delays.size() << ",\"recall_percent\":"
              << (qualified == 0 ? 100.0
                                 : static_cast<double>(delays.size()) * 100 / static_cast<double>(qualified))
              << ",\"first_detection_delay_days\":";
    print_stats(statistics(std::move(delays)));
    std::cout << "}}\n";
    return 0;
}
