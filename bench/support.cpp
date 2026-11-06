#include "support.hpp"

#include <benchmark/benchmark.h>

#include <charconv>
#include <cstdio>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__linux__)
#include <sched.h>
#endif

namespace pat::bench {
namespace {

class CpuAffinity {
public:
    CpuAffinity() = default;
    CpuAffinity(const CpuAffinity&) = delete;
    CpuAffinity& operator=(const CpuAffinity&) = delete;

    [[nodiscard]] int selected_cpu() const { return cpu_; }

    ~CpuAffinity() {
#if defined(_WIN32)
        if (previous_ != 0) {
            (void)SetThreadAffinityMask(GetCurrentThread(), previous_);
        }
#elif defined(__linux__)
        if (pinned_) {
            (void)sched_setaffinity(0, sizeof(previous_), &previous_);
        }
#endif
    }

    // A negative CPU selects the first CPU in the current allowed affinity mask.
    bool pin(int cpu) {
#if defined(_WIN32)
        DWORD_PTR allowed = 0;
        DWORD_PTR system = 0;
        if (GetProcessAffinityMask(GetCurrentProcess(), &allowed, &system) == 0) {
            return false;
        }
        constexpr int kCpuLimit = std::numeric_limits<DWORD_PTR>::digits;
        if (cpu < 0) {
            cpu = 0;
            while (cpu < kCpuLimit && (allowed & (DWORD_PTR{1} << cpu)) == 0) {
                ++cpu;
            }
        }
        if (cpu >= kCpuLimit || (allowed & (DWORD_PTR{1} << cpu)) == 0) {
            return false;
        }
        previous_ = SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR{1} << cpu);
        if (previous_ == 0) {
            return false;
        }
#elif defined(__linux__)
        if (sched_getaffinity(0, sizeof(previous_), &previous_) != 0) {
            return false;
        }
        if (cpu < 0) {
            cpu = 0;
            while (cpu < CPU_SETSIZE && !CPU_ISSET(cpu, &previous_)) {
                ++cpu;
            }
        }
        if (cpu >= CPU_SETSIZE || !CPU_ISSET(cpu, &previous_)) {
            return false;
        }
        cpu_set_t selected;
        CPU_ZERO(&selected);
        CPU_SET(cpu, &selected);
        if (sched_setaffinity(0, sizeof(selected), &selected) != 0) {
            return false;
        }
        pinned_ = true;
#else
        (void)cpu;
        return false;
#endif
        cpu_ = cpu;
        return true;
    }

private:
    int cpu_ = -1;
#if defined(_WIN32)
    DWORD_PTR previous_ = 0;
#elif defined(__linux__)
    cpu_set_t previous_{};
    bool pinned_ = false;
#endif
};

bool configure_affinity(std::string_view request, CpuAffinity& affinity) {
    if (request == "none") {
        benchmark::AddCustomContext("cpu_affinity", "none");
        return true;
    }
    int cpu = -1;
    if (request != "auto") {
        const auto [end, error] = std::from_chars(request.data(), request.data() + request.size(), cpu);
        if (error != std::errc{} || end != request.data() + request.size() || cpu < 0) {
            std::fprintf(stderr, "--pat_cpu expects auto, none, or a nonnegative CPU index\n");
            return false;
        }
    }
#if !defined(_WIN32) && !defined(__linux__)
    if (request == "auto") {
        benchmark::AddCustomContext("cpu_affinity", "unsupported");
        return true;
    }
#endif
    if (!affinity.pin(cpu)) {
        std::fprintf(stderr,
                     "Cannot pin benchmark thread to CPU '%.*s' on this platform\n",
                     static_cast<int>(request.size()),
                     request.data());
        return false;
    }
    benchmark::AddCustomContext("cpu_affinity", std::to_string(affinity.selected_cpu()));
    return true;
}

}  // namespace

int run_benchmarks(int argc, char** argv) {
    // Defaults precede user flags so the last occurrence (the user's value) takes precedence.
    // Keep all strings alive until Shutdown; Initialize compacts the argv pointer array.
    std::vector<std::string> arguments{argv[0],
                                       "--benchmark_min_time=0.1s",
                                       "--benchmark_min_warmup_time=0.05",
                                       "--benchmark_repetitions=5",
                                       "--benchmark_display_aggregates_only=true",
                                       "--benchmark_format=json"};
    std::string cpu = "auto";
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg.starts_with("--pat_cpu=")) {
            cpu = arg.substr(std::string_view("--pat_cpu=").size());
        } else {
            arguments.emplace_back(arg);
        }
    }
    std::vector<char*> pointers;
    pointers.reserve(arguments.size() + 1);
    for (auto& argument : arguments) {
        pointers.push_back(argument.data());
    }
    pointers.push_back(nullptr);
    int count = static_cast<int>(arguments.size());
    benchmark::Initialize(&count, pointers.data());
    if (benchmark::ReportUnrecognizedArguments(count, pointers.data())) {
        benchmark::Shutdown();
        return 1;
    }
    CpuAffinity affinity;
    if (!configure_affinity(cpu, affinity)) {
        benchmark::Shutdown();
        return 1;
    }
    const auto matched = benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return matched == 0 ? 1 : 0;
}

}  // namespace pat::bench
