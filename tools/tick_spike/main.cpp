#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "pool_executor.hpp"
#include "workload.hpp"

#if defined(_WIN32)
#include <windows.h>
#elif defined(__linux__)
#include <sched.h>
#endif

namespace {

class Affinity {
public:
    ~Affinity() {
#if defined(_WIN32)
        if (original_ != 0) {
            (void)SetProcessAffinityMask(GetCurrentProcess(), original_);
        }
#elif defined(__linux__)
        if (changed_) {
            (void)sched_setaffinity(0, sizeof(original_), &original_);
        }
#endif
    }

    bool select(std::size_t threads) {
#if defined(_WIN32)
        DWORD_PTR system = 0;
        if (GetProcessAffinityMask(GetCurrentProcess(), &original_, &system) == 0) {
            return false;
        }
        std::vector<int> order;
        auto append_first = [&](ULONG_PTR mask) {
            for (int cpu = 0; cpu < static_cast<int>(sizeof(mask) * 8); ++cpu) {
                if ((mask & (ULONG_PTR{1} << cpu)) != 0) {
                    order.push_back(cpu);
                    return;
                }
            }
        };
        DWORD bytes = 0;
        (void)GetLogicalProcessorInformation(nullptr, &bytes);
        if (bytes == 0) {
            return false;
        }
        std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> topology(
            (bytes + sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION) - 1) /
            sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION));
        if (GetLogicalProcessorInformation(topology.data(), &bytes) == 0) {
            return false;
        }
        const auto count = bytes / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION);
        for (std::size_t i = 0; i < count; ++i) {
            if (topology[i].Relationship == RelationProcessorCore) {
                append_first(topology[i].ProcessorMask & original_);
            }
        }
        physical_cores = order.size();
        if (physical_cores == 0) {
            return false;
        }
        // Select one hardware thread per physical core first, then SMT siblings if requested.
        for (int cpu = 0; cpu < static_cast<int>(sizeof(original_) * 8); ++cpu) {
            if ((original_ & (DWORD_PTR{1} << cpu)) != 0 &&
                std::find(order.begin(), order.end(), cpu) == order.end()) {
                order.push_back(cpu);
            }
        }
        if (threads > order.size()) {
            return false;
        }
        cpus.assign(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(threads));
        DWORD_PTR selected = 0;
        for (const int cpu : cpus) {
            selected |= DWORD_PTR{1} << cpu;
        }
        return SetProcessAffinityMask(GetCurrentProcess(), selected) != 0;
#elif defined(__linux__)
        if (sched_getaffinity(0, sizeof(original_), &original_) != 0) {
            return false;
        }
        cpu_set_t selected;
        CPU_ZERO(&selected);
        for (int cpu = 0; cpu < CPU_SETSIZE && cpus.size() < threads; ++cpu) {
            if (CPU_ISSET(cpu, &original_)) {
                cpus.push_back(cpu);
                CPU_SET(cpu, &selected);
            }
        }
        if (cpus.size() != threads || sched_setaffinity(0, sizeof(selected), &selected) != 0) {
            return false;
        }
        changed_ = true;
        return true;
#else
        (void)threads;
        return false;
#endif
    }

    std::vector<int> cpus;
    std::size_t physical_cores = 0;  // 0 means topology is not identified on this platform.

private:
#if defined(_WIN32)
    DWORD_PTR original_ = 0;
#elif defined(__linux__)
    cpu_set_t original_{};
    bool changed_ = false;
#endif
};

struct Stats {
    std::size_t count = 0;
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
            percentile(0.50),
            percentile(0.95),
            percentile(0.99),
            values.back()};
}

void print_stats(const Stats& stats) {
    std::cout << "{\"samples\":" << stats.count << ",\"mean_us\":" << stats.mean
              << ",\"p50_us\":" << stats.p50 << ",\"p95_us\":" << stats.p95 << ",\"p99_us\":" << stats.p99
              << ",\"max_us\":" << stats.maximum << '}';
}

std::string hex_hash(std::uint64_t value) {
    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(16) << value;
    return out.str();
}

int fail(const std::string& message) {
    std::fprintf(stderr, "tick_spike: %s\n", message.c_str());
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace pat;
    std::string profile = "year60";
    std::string mode_name = "stagger";
    std::uint64_t seed = 7;
    std::uint64_t quota = 4096;
    std::size_t threads = 1;
    std::size_t grain = 64;
    int years = 10;
    int days = 0;
    int repetitions = 3;
    for (int i = 1; i < argc; ++i) {
        const std::string_view flag(argv[i]);
        if (flag == "--help") {
            std::cout << "tick_spike [--profile year1|year60|year60x3] [--mode burst|stagger]\n"
                         "  [--threads N] [--grain N] [--years N | --days N] [--repetitions N]\n"
                         "  [--seed N] [--ai-wu N]\nJSON to stdout; Release builds only for measurements.\n";
            return 0;
        }
        if (i + 1 == argc) {
            return fail("missing value for " + std::string(flag));
        }
        const std::string_view value(argv[++i]);
        if (flag == "--profile") {
            profile = value;
            continue;
        }
        if (flag == "--mode") {
            mode_name = value;
            continue;
        }
        std::uint64_t n = 0;
        const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), n);
        if (error != std::errc{} || end != value.data() + value.size()) {
            return fail("invalid unsigned integer for " + std::string(flag));
        }
        if (flag == "--seed") {
            seed = n;
        } else if (flag == "--ai-wu" && n <= 1000000) {
            quota = n;
        } else if (flag == "--threads" && n >= 1 && n <= 64) {
            threads = static_cast<std::size_t>(n);
        } else if (flag == "--grain" && n >= 1 && n <= 1000000) {
            grain = static_cast<std::size_t>(n);
        } else if (flag == "--years" && n >= 1 && n <= 50) {
            years = static_cast<int>(n);
        } else if (flag == "--days" && n >= 1 && n <= 20000) {
            days = static_cast<int>(n);
        } else if (flag == "--repetitions" && n >= 1 && n <= 20) {
            repetitions = static_cast<int>(n);
        } else {
            return fail("unknown option or out-of-range value: " + std::string(flag));
        }
    }
    auto config = stress::profile_config(profile);
    if (!config) {
        return fail(config.error().message);
    }
    config.value().world.seed = seed;
    if (mode_name != "burst" && mode_name != "stagger") {
        return fail("--mode must be burst or stagger");
    }
    if (days == 0) {
        days =
            sim::Date::from_ymd(config.value().world.date.year() + years, 1, 1) - config.value().world.date;
    }
    const auto mode = mode_name == "burst" ? spike::MonthlyMode::Burst : spike::MonthlyMode::Stagger;
    Affinity affinity;
    if (!affinity.select(threads)) {
        return fail("cannot apply requested CPU affinity/topology on this host");
    }
    auto initial = spike::make_state(config.value());
    if (!initial) {
        return fail(initial.error().message);
    }
    std::unique_ptr<sim::TaskExecutor> executor;
    if (threads == 1) {
        executor = std::make_unique<sim::SerialExecutor>();
    } else {
        executor = std::make_unique<spike::PoolExecutor>(threads);
    }
    std::vector<spike::Sample> samples(static_cast<std::size_t>(days) *
                                       static_cast<std::size_t>(repetitions));
    std::vector<double> means;
    std::uint64_t hash = 0;
    std::array<std::uint64_t, spike::kStages> work{};
    for (int repeat = 0; repeat < repetitions; ++repeat) {
        auto state = initial.value();
        for (int day = 0; day < 31; ++day) {
            (void)spike::tick(state, *executor, grain, mode, quota);
        }
        state = initial.value();  // Reset outside the timed interval; every repetition has identical input.
        double total = 0;
        for (int day = 0; day < days; ++day) {
            auto& sample = samples[static_cast<std::size_t>(repeat) * static_cast<std::size_t>(days) +
                                   static_cast<std::size_t>(day)];
            sample = spike::tick(state, *executor, grain, mode, quota);
            total += sample.tick_us;
        }
        means.push_back(total / days);
        const auto current_hash = spike::state_hash(state);
        if (repeat != 0 && current_hash != hash) {
            return fail("repetitions produced different state hashes");
        }
        hash = current_hash;
        work = state.work;
    }
    const auto mean = std::accumulate(means.begin(), means.end(), 0.0) / repetitions;
    double variance = 0;
    for (const auto value : means) {
        variance += (value - mean) * (value - mean);
    }
    const double rsd = repetitions > 1 && mean > 0 ? std::sqrt(variance / (repetitions - 1)) / mean * 100 : 0;
    std::vector<double> all;
    std::array<std::vector<double>, 4> cadence;
    std::array<std::vector<double>, spike::kStages> stages;
    std::array<double, spike::kStages> totals{};
    for (const auto& sample : samples) {
        all.push_back(sample.tick_us);
        cadence[static_cast<std::size_t>(sample.cadence)].push_back(sample.tick_us);
        for (std::size_t stage = 0; stage < spike::kStages; ++stage) {
            totals[stage] += sample.stage_us[stage];
            if (sample.stage_us[stage] > 0) {
                stages[stage].push_back(sample.stage_us[stage]);
            }
        }
    }
    std::cout << std::setprecision(12) << "{\"schema\":1,\"profile\":\"" << profile << "\",\"mode\":\""
              << mode_name << "\",\"threads\":" << threads << ",\"grain\":" << grain << ",\"days\":" << days
              << ",\"repetitions\":" << repetitions << ",\"warmup_days\":31,\"seed\":" << seed
              << ",\"ai_quota_wu\":" << quota << ",\"physical_cores\":" << affinity.physical_cores
              << ",\"logical_cpus\":" << std::thread::hardware_concurrency() << ",\"build\":\""
              << PAT_SPIKE_CONFIG << "\",\"affinity_cpus\":[";
    for (std::size_t i = 0; i < affinity.cpus.size(); ++i) {
        std::cout << (i == 0 ? "" : ",") << affinity.cpus[i];
    }
    std::cout << "],\"hash\":\"" << hex_hash(hash) << "\",\"repeat_rsd_percent\":" << rsd
              << ",\"stable\":" << (repetitions >= 3 && rsd <= 5 ? "true" : "false") << ",\"tick\":";
    print_stats(statistics(std::move(all)));
    std::cout << ",\"repeat_mean_us\":[";
    for (std::size_t i = 0; i < means.size(); ++i) {
        std::cout << (i == 0 ? "" : ",") << means[i];
    }
    constexpr std::array<std::string_view, 4> kCadences{"daily", "weekly", "monthly", "yearly"};
    std::cout << "],\"cadence\":{";
    for (std::size_t i = 0; i < cadence.size(); ++i) {
        std::cout << (i == 0 ? "" : ",") << '"' << kCadences[i] << "\":";
        print_stats(statistics(std::move(cadence[i])));
    }
    std::cout << "},\"stages\":{";
    for (std::size_t i = 0; i < spike::kStages; ++i) {
        std::cout << (i == 0 ? "" : ",") << '"' << spike::kStageNames[i] << "\":{\"work_per_run\":" << work[i]
                  << ",\"mean_us_per_tick\":" << totals[i] / static_cast<double>(samples.size())
                  << ",\"active\":";
        print_stats(statistics(std::move(stages[i])));
        std::cout << '}';
    }
    std::cout << "}}\n";
    return 0;
}
