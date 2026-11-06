#include "pat/core/bytes.hpp"

#include <algorithm>
#include <barrier>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string>
#include <string_view>
#include <thread>

#include "snapshots.hpp"

#if defined(_WIN32)
#include <windows.h>
#elif defined(__linux__)
#include <sched.h>
#endif

namespace ss = pat::snapshot_spike;
namespace {

constexpr std::array<ss::Method, 3> kMethods{ss::Method::Full, ss::Method::Diff, ss::Method::Pages};
constexpr std::array<std::string_view, 3> kNames{"full", "diff", "pages"};

struct Config {
    bool triple = false;
    int changed = 10;
    int rate = 400;
    int seconds = 2;
    int repetitions = 3;
    int iterations = 1024;
    int hold = 0;
};

std::vector<int> allowed_cpus() {
    std::vector<int> result;
#if defined(_WIN32)
    DWORD_PTR mask = 0, system = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &mask, &system)) {
        for (int i = 0; i < static_cast<int>(sizeof(mask) * 8); ++i) {
            if ((mask & (DWORD_PTR{1} << i)) != 0)
                result.push_back(i);
        }
    }
#elif defined(__linux__)
    cpu_set_t mask;
    if (sched_getaffinity(0, sizeof(mask), &mask) == 0) {
        for (int i = 0; i < CPU_SETSIZE; ++i) {
            if (CPU_ISSET(i, &mask))
                result.push_back(i);
        }
    }
#endif
    return result;
}

class Pin {
public:
    explicit Pin(int cpu) {
#if defined(_WIN32)
        previous_ = SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR{1} << cpu);
        ok = previous_ != 0;
#elif defined(__linux__)
        ok = sched_getaffinity(0, sizeof(previous_), &previous_) == 0;
        cpu_set_t selected;
        CPU_ZERO(&selected);
        CPU_SET(cpu, &selected);
        ok = ok && sched_setaffinity(0, sizeof(selected), &selected) == 0;
#else
        (void)cpu;
#endif
    }
    ~Pin() {
#if defined(_WIN32)
        if (ok)
            (void)SetThreadAffinityMask(GetCurrentThread(), previous_);
#elif defined(__linux__)
        if (ok)
            (void)sched_setaffinity(0, sizeof(previous_), &previous_);
#endif
    }
    bool ok = false;

private:
#if defined(_WIN32)
    DWORD_PTR previous_ = 0;
#elif defined(__linux__)
    cpu_set_t previous_{};
#endif
};

class Pacer {
public:
    Pacer() {
#if defined(_WIN32)
        timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002, TIMER_ALL_ACCESS);
        high_resolution = timer_ != nullptr;
#endif
    }
    ~Pacer() {
#if defined(_WIN32)
        if (timer_)
            CloseHandle(timer_);
#endif
    }
    void until(ss::Clock::time_point target) {
        const auto now = ss::Clock::now();
        if (target <= now)
            return;
#if defined(_WIN32)
        if (timer_) {
            LARGE_INTEGER due;
            due.QuadPart = -std::max<std::int64_t>(
                1, std::chrono::duration_cast<std::chrono::nanoseconds>(target - now).count() / 100);
            if (SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE)) {
                (void)WaitForSingleObject(timer_, INFINITE);
                return;
            }
        }
#endif
        std::this_thread::sleep_until(target);
    }
    bool high_resolution = false;

private:
#if defined(_WIN32)
    HANDLE timer_ = nullptr;
#endif
};

double microseconds(ss::Clock::duration duration) {
    return std::chrono::duration<double, std::micro>(duration).count();
}

void stats(std::vector<double> samples) {
    std::sort(samples.begin(), samples.end());
    const auto mean = samples.empty() ? 0
                                      : std::accumulate(samples.begin(), samples.end(), 0.0) /
                                            static_cast<double>(samples.size());
    auto percentile = [&](double fraction) {
        return samples.empty() ? 0
                               : samples[static_cast<std::size_t>(
                                             std::ceil(fraction * static_cast<double>(samples.size()))) -
                                         1];
    };
    std::cout << "{\"samples\":" << samples.size() << ",\"mean\":" << mean << ",\"p50\":" << percentile(.5)
              << ",\"p95\":" << percentile(.95) << ",\"p99\":" << percentile(.99)
              << ",\"max\":" << (samples.empty() ? 0 : samples.back()) << '}';
}

double mean(const std::vector<double>& values) {
    return values.empty()
               ? 0
               : std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
}
double rsd(const std::vector<double>& values) {
    if (values.size() < 2 || mean(values) == 0)
        return 0;
    const auto average = mean(values);
    double sum = 0;
    for (const auto value : values)
        sum += (value - average) * (value - average);
    return std::sqrt(sum / static_cast<double>(values.size() - 1)) / average * 100;
}

struct Metrics {
    std::vector<double> publish, acquire, read, age, repeat_publish_mean, rates, frame_rates;
    std::uint64_t payload = 0, consumer_bytes = 0, allocations = 0, memory_peak = 0;
    std::uint64_t checkpoints = 0, queue_peak = 0, reused_pages = 0, cache_copies = 0;
    std::uint64_t distinct = 0, repeats = 0, overruns = 0;
    bool high_resolution = true;
    void collect(const ss::Transport& transport) {
        const auto memory = transport.memory();
        allocations += memory->allocations.load();
        memory_peak = std::max(memory_peak, memory->peak.load());
        const auto& traffic = transport.traffic();
        payload += traffic.payload_bytes;
        consumer_bytes += traffic.consumer_bytes;
        checkpoints += traffic.checkpoints;
        queue_peak = std::max(queue_peak, traffic.queue_peak);
        reused_pages += traffic.reused_pages;
        cache_copies += traffic.retained_cache_copies;
    }
};

bool matches(const ss::Frame& frame, const ss::Data& data) {
    if (frame.meta().version != data.meta.version || ss::checksum(frame) != data.meta.checksum)
        return false;
    for (std::size_t row = 0; row < data.rows.size(); ++row) {
        for (std::size_t field = 0; field < ss::kFields; ++field) {
            if (frame.at(row, field) != data.rows[row][field])
                return false;
        }
    }
    return true;
}

bool micro(const ss::Fixture& initial, const Config& config, std::array<Metrics, 3>& metrics) {
    for (int repeat = 0; repeat < config.repetitions; ++repeat) {
        auto fixture = initial;
        std::array<ss::Transport, 3> transports{ss::Transport(kMethods[0], fixture.source),
                                                ss::Transport(kMethods[1], fixture.source),
                                                ss::Transport(kMethods[2], fixture.source)};
        std::array<double, 3> sum{};
        for (int tick = 0; tick < config.iterations + 64; ++tick) {
            ss::advance(fixture, static_cast<std::uint32_t>(config.changed));
            for (std::size_t turn = 0; turn < 3; ++turn) {
                const auto mode = (static_cast<std::size_t>(tick + repeat) + turn) % 3;
                const auto begin = ss::Clock::now();
                transports[mode].publish(fixture.source, fixture.changes, fixture.page_versions);
                const auto published = ss::Clock::now();
                const auto frame = transports[mode].acquire();
                const auto acquired = ss::Clock::now();
                if (ss::checksum(frame) != fixture.source.meta.checksum || !transports[mode].valid())
                    return false;
                if (tick >= 64) {
                    const auto us = microseconds(published - begin);
                    metrics[mode].publish.push_back(us);
                    metrics[mode].acquire.push_back(microseconds(acquired - published));
                    sum[mode] += us;
                }
            }
        }
        for (std::size_t mode = 0; mode < 3; ++mode) {
            if (!matches(transports[mode].acquire(), fixture.source))
                return false;
            metrics[mode].repeat_publish_mean.push_back(sum[mode] / config.iterations);
            metrics[mode].collect(transports[mode]);
        }
    }
    return true;
}

bool paced(
    const ss::Fixture& initial, const Config& config, ss::Method method, int consumer_cpu, Metrics& metrics) {
    auto fixture = initial;
    ss::Transport transport(method, fixture.source);
    std::barrier start(2);
    const auto begin = ss::Clock::now() + std::chrono::milliseconds(30);
    const auto end = begin + std::chrono::seconds(config.seconds);
    bool valid = true;
    bool reader_pin = false;
    bool reader_timer = false;
    const auto prior_frames = metrics.read.size();
    std::thread reader([&] {
        Pin pin(consumer_cpu);
        reader_pin = pin.ok;
        Pacer pacer;
        reader_timer = pacer.high_resolution;
        std::array<ss::Frame, 4> held;
        std::size_t held_cursor = 0;
        std::uint64_t previous = 0;
        auto target = begin;
        const auto period =
            std::chrono::duration_cast<ss::Clock::duration>(std::chrono::duration<double>(1.0 / 60));
        start.arrive_and_wait();
        while (target < end) {
            pacer.until(target);
            if (ss::Clock::now() >= end)
                break;
            const auto a = ss::Clock::now();
            const auto frame = transport.acquire();
            const auto b = ss::Clock::now();
            const auto hash = ss::checksum(frame);
            const auto c = ss::Clock::now();
            metrics.acquire.push_back(microseconds(b - a));
            metrics.read.push_back(microseconds(c - b));
            const auto& meta = frame.meta();
            valid = valid && hash == meta.checksum && meta.version >= previous;
            if (meta.version > 0)
                metrics.age.push_back(microseconds(b - meta.ready));
            if (meta.version > previous)
                ++metrics.distinct;
            else
                ++metrics.repeats;
            previous = meta.version;
            if (config.hold > 0) {
                held[held_cursor++ % static_cast<std::size_t>(config.hold)] = frame;
            }
            target += period;
            if (c > target + period)
                target = c;
        }
    });
    Pacer pacer;
    start.arrive_and_wait();
    auto target = begin;
    const auto period =
        std::chrono::duration_cast<ss::Clock::duration>(std::chrono::duration<double>(1.0 / config.rate));
    std::size_t publications = 0;
    double total_us = 0;
    while (target < end) {
        pacer.until(target);
        if (ss::Clock::now() >= end)
            break;
        ss::advance(fixture, static_cast<std::uint32_t>(config.changed));
        const auto a = ss::Clock::now();
        transport.publish(fixture.source, fixture.changes, fixture.page_versions);
        const auto b = ss::Clock::now();
        const auto us = microseconds(b - a);
        metrics.publish.push_back(us);
        total_us += us;
        ++publications;
        target += period;
        if (b > target + period) {
            ++metrics.overruns;
            target = b;
        }
    }
    reader.join();
    metrics.repeat_publish_mean.push_back(publications ? total_us / static_cast<double>(publications) : 0);
    metrics.rates.push_back(static_cast<double>(publications) / config.seconds);
    metrics.frame_rates.push_back(static_cast<double>(metrics.read.size() - prior_frames) / config.seconds);
    metrics.high_resolution = metrics.high_resolution && pacer.high_resolution && reader_timer;
    metrics.collect(transport);
    return valid && reader_pin && transport.valid() && matches(transport.acquire(), fixture.source);
}

void print_metrics(const Metrics& metrics) {
    std::cout << "{\"publish_us\":";
    stats(metrics.publish);
    std::cout << ",\"acquire_us\":";
    stats(metrics.acquire);
    std::cout << ",\"read_us\":";
    stats(metrics.read);
    std::cout << ",\"age_at_acquire_us\":";
    stats(metrics.age);
    std::cout << ",\"publish_rsd_percent\":" << rsd(metrics.repeat_publish_mean) << ",\"stable\":"
              << (metrics.repeat_publish_mean.size() >= 3 && rsd(metrics.repeat_publish_mean) <= 5 ? "true"
                                                                                                   : "false")
              << ",\"publications_per_second\":" << mean(metrics.rates)
              << ",\"frames_per_second\":" << mean(metrics.frame_rates)
              << ",\"owned_bytes_peak\":" << metrics.memory_peak
              << ",\"object_allocations\":" << metrics.allocations
              << ",\"producer_payload_bytes\":" << metrics.payload
              << ",\"consumer_payload_bytes\":" << metrics.consumer_bytes
              << ",\"checkpoints\":" << metrics.checkpoints << ",\"queue_peak\":" << metrics.queue_peak
              << ",\"reused_pages\":" << metrics.reused_pages
              << ",\"retained_cache_copies\":" << metrics.cache_copies
              << ",\"distinct_versions_read\":" << metrics.distinct
              << ",\"repeated_frames\":" << metrics.repeats << ",\"producer_overruns\":" << metrics.overruns
              << ",\"high_resolution_timers\":" << (metrics.high_resolution ? "true" : "false")
              << ",\"repeat_publish_mean_us\":[";
    for (std::size_t i = 0; i < metrics.repeat_publish_mean.size(); ++i) {
        std::cout << (i ? "," : "") << metrics.repeat_publish_mean[i];
    }
    std::cout << "]}";
}

int fail(std::string_view message) {
    std::fprintf(stderr, "snapshot_spike: %.*s\n", static_cast<int>(message.size()), message.data());
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    Config config;
    for (int i = 1; i < argc; ++i) {
        const std::string_view flag(argv[i]);
        if (flag == "--help") {
            std::cout << "snapshot_spike --profile year60|year60x3 --changed-permille 1..1000\n"
                         "  --rate 1..2000 --seconds 1..10 --repetitions 1..5 --iterations 32..4096\n"
                         "  --hold-frames 0..4\n";
            return 0;
        }
        if (++i == argc)
            return fail("missing option value");
        const std::string_view value(argv[i]);
        if (flag == "--profile") {
            if (value != "year60" && value != "year60x3")
                return fail("unknown profile");
            config.triple = value == "year60x3";
            continue;
        }
        int number = 0;
        const auto [tail, error] = std::from_chars(value.data(), value.data() + value.size(), number);
        if (error != std::errc{} || tail != value.data() + value.size())
            return fail("invalid integer option");
        if (flag == "--changed-permille" && number >= 1 && number <= 1000)
            config.changed = number;
        else if (flag == "--rate" && number >= 1 && number <= 2000)
            config.rate = number;
        else if (flag == "--seconds" && number >= 1 && number <= 10)
            config.seconds = number;
        else if (flag == "--repetitions" && number >= 1 && number <= 5)
            config.repetitions = number;
        else if (flag == "--iterations" && number >= 32 && number <= 4096)
            config.iterations = number;
        else if (flag == "--hold-frames" && number >= 0 && number <= 4)
            config.hold = number;
        else
            return fail("unknown option or value out of range");
    }
    const auto cpus = allowed_cpus();
    if (cpus.size() < 2)
        return fail("two allowed logical CPUs are required");
    const auto consumer_cpu = cpus[std::min<std::size_t>(2, cpus.size() - 1)];
    Pin producer_pin(cpus[0]);
    if (!producer_pin.ok)
        return fail("cannot pin producer thread");
    auto generated = ss::make_fixture(config.triple);
    if (!generated)
        return fail(generated.error().message);
    const auto& initial = generated.value();
    std::array<Metrics, 3> micro_metrics, paced_metrics;
    if (!micro(initial, config, micro_metrics))
        return fail("micro results differ");
    for (int repeat = 0; repeat < config.repetitions; ++repeat) {
        for (std::size_t turn = 0; turn < 3; ++turn) {
            const auto mode = (static_cast<std::size_t>(repeat) + turn) % 3;
            if (!paced(initial, config, kMethods[mode], consumer_cpu, paced_metrics[mode])) {
                return fail("concurrent snapshot validation or thread affinity failed");
            }
        }
    }
    std::vector<double> world_times;
    std::atomic<std::shared_ptr<const pat::sim::World>> world_snapshot(initial.world.snapshot());
    for (int i = 0; i < 192; ++i) {
        const auto begin = ss::Clock::now();
        world_snapshot.store(initial.world.snapshot());
        const auto end = ss::Clock::now();
        if (i >= 16)
            world_times.push_back(microseconds(end - begin));
    }
    if (world_snapshot.load()->compute_hash() != initial.world.compute_hash())
        return fail("World copy differs");
    pat::core::ByteWriter serialized;
    initial.world.save_state(serialized);
    std::cout << std::setprecision(12) << "{\"schema\":1,\"build\":\"" << PAT_SNAPSHOT_CONFIG
              << "\",\"profile\":\"" << (config.triple ? "year60x3" : "year60")
              << "\",\"changed_permille\":" << config.changed << ",\"target_rate\":" << config.rate
              << ",\"seconds\":" << config.seconds << ",\"repetitions\":" << config.repetitions
              << ",\"micro_iterations\":" << config.iterations << ",\"micro_warmup\":64"
              << ",\"hold_frames\":" << config.hold << ",\"producer_cpu\":" << cpus[0]
              << ",\"consumer_cpu\":" << consumer_cpu << ",\"rows\":" << initial.source.rows.size()
              << ",\"ui_payload_bytes\":" << initial.source.rows.size() * sizeof(ss::Row)
              << ",\"serialized_world_bytes\":" << serialized.data().size()
              << ",\"world_copy_reference_us\":";
    stats(std::move(world_times));
    std::cout << ",\"methods\":{";
    for (std::size_t mode = 0; mode < 3; ++mode) {
        std::cout << (mode ? "," : "") << '"' << kNames[mode] << "\":{\"micro\":";
        print_metrics(micro_metrics[mode]);
        std::cout << ",\"paced\":";
        print_metrics(paced_metrics[mode]);
        std::cout << '}';
    }
    std::cout << "},\"correct\":true}\n";
    return 0;
}
