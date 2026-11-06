// simcli - headless runner for the simulation core (E1-T11).
//
// Runs the built-in demo scenario without any graphics: useful for CI, profiling, replay checks and
// reproducing bugs. See --help for the options.

#include "pat/sim/demo_scenario.hpp"
#include "pat/sim/replay.hpp"
#include "pat/sim/runner.hpp"
#include "pat/sim/save.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <format>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace pat::sim;
using Clock = std::chrono::steady_clock;

enum ExitCode : int {
    kOk = 0,
    kUsageOrIoError = 1,
    kReplayDiverged = 2,
    kDeterminismFailed = 3,
    kBadSave = 4,
};

struct Options {
    demo::Config scenario;
    int days = 365;
    std::optional<std::string> speed;  // "1".."5" or "unlimited": run through the threaded runner
    int checkpoint_interval = 30;
    std::optional<std::string> record_path;
    std::optional<std::string> replay_path;
    std::optional<std::string> save_path;
    std::optional<std::string> load_path;
    bool verify_determinism = false;
    bool hash_only = false;
    bool quiet = false;
    bool help = false;
};

constexpr const char* kUsage = R"(simcli - headless runner for the Patriarch simulation core

Usage: simcli [options]

Scenario (built-in demo, not game content):
  --seed N              random seed (default 1)
  --dynasties N         number of dynasties (default 8)
  --provinces N         number of provinces (default 80)
  --characters N        initial characters per dynasty (default 6)

Run:
  --days N              number of days to simulate (default 365)
  --speed S             use the threaded runner: unlimited (exactly N days) or 1..5 (paced,
                        runs until at least N days have passed)

Files:
  --record FILE         write a replay (checkpoint every --checkpoint ticks, default 30)
  --replay FILE         verify a replay instead of running a new session
  --save FILE           save the final state
  --load FILE           start from a saved state (cannot be combined with --record)

Checks and output:
  --verify-determinism  run the scenario several ways and require identical hashes
  --hash-only           print only the final state hash
  --quiet               print nothing but errors
  --help                show this text

Exit codes: 0 ok, 1 usage or I/O error, 2 replay diverged, 3 determinism check failed, 4 bad save
)";

bool parse_u64(std::string_view text, std::uint64_t& out) {
    if (text.empty() || text.size() > 19) {
        return false;
    }
    std::uint64_t value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + static_cast<std::uint64_t>(c - '0');
    }
    out = value;
    return true;
}

/// Returns an error message, or an empty string on success.
std::string parse_args(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];

        // Options that take a value read it from the next argument.
        const auto takes_value = [&](std::string_view name, std::string& out) -> bool {
            if (arg != name) {
                return false;
            }
            if (i + 1 >= argc) {
                out = "<missing>";
                return true;
            }
            out = argv[++i];
            return true;
        };

        std::string text;
        std::uint64_t n = 0;
        const auto number =
            [&](std::string_view name) -> int {  // 0 = not this option, 1 = ok, -1 = bad value
            if (!takes_value(name, text)) {
                return 0;
            }
            return parse_u64(text, n) ? 1 : -1;
        };

        if (arg == "--help" || arg == "-h") {
            options.help = true;
        } else if (arg == "--verify-determinism") {
            options.verify_determinism = true;
        } else if (arg == "--hash-only") {
            options.hash_only = true;
        } else if (arg == "--quiet") {
            options.quiet = true;
        } else if (const int r = number("--seed"); r != 0) {
            if (r < 0)
                return "--seed needs a non-negative integer";
            options.scenario.seed = n;
        } else if (const int r2 = number("--days"); r2 != 0) {
            if (r2 < 0)
                return "--days needs a non-negative integer";
            options.days = static_cast<int>(std::min<std::uint64_t>(n, 1'000'000));
        } else if (const int r3 = number("--dynasties"); r3 != 0) {
            if (r3 < 0)
                return "--dynasties needs a non-negative integer";
            options.scenario.dynasties = static_cast<std::uint32_t>(std::min<std::uint64_t>(n, 10'000));
        } else if (const int r4 = number("--provinces"); r4 != 0) {
            if (r4 < 0)
                return "--provinces needs a non-negative integer";
            options.scenario.provinces = static_cast<std::uint32_t>(std::min<std::uint64_t>(n, 100'000));
        } else if (const int r5 = number("--characters"); r5 != 0) {
            if (r5 < 0)
                return "--characters needs a non-negative integer";
            options.scenario.characters_per_dynasty =
                static_cast<std::uint32_t>(std::min<std::uint64_t>(n, 1'000));
        } else if (const int r6 = number("--checkpoint"); r6 != 0) {
            if (r6 < 0)
                return "--checkpoint needs a positive integer";
            options.checkpoint_interval = static_cast<int>(std::clamp<std::uint64_t>(n, 1, 1'000'000));
        } else if (takes_value("--speed", text)) {
            options.speed = text;
        } else if (takes_value("--record", text)) {
            options.record_path = text;
        } else if (takes_value("--replay", text)) {
            options.replay_path = text;
        } else if (takes_value("--save", text)) {
            options.save_path = text;
        } else if (takes_value("--load", text)) {
            options.load_path = text;
        } else {
            return std::format("unknown option: {}", arg);
        }
    }

    for (const auto* path :
         {&options.record_path, &options.replay_path, &options.save_path, &options.load_path}) {
        if (path->has_value() && *path == std::string("<missing>")) {
            return "an option is missing its argument";
        }
    }
    if (options.speed.has_value()) {
        const std::string& s = *options.speed;
        if (s != "unlimited" && !(s.size() == 1 && s[0] >= '1' && s[0] <= '5')) {
            return "--speed must be 1..5 or unlimited";
        }
    }
    if (options.record_path && options.load_path) {
        return "--record cannot be combined with --load (a replay must start at the scenario start)";
    }
    return {};
}

std::string hash_line(const WorldHash& h) {
    return std::format(
        "total={:016x} calendar={:016x} rng={:016x} dynasties={:016x} provinces={:016x} characters={:016x}",
        h.total,
        h.calendar,
        h.rng,
        h.dynasties,
        h.provinces,
        h.characters);
}

int fail(int code, const std::string& message) {
    std::fprintf(stderr, "simcli: %s\n", message.c_str());
    return code;
}

// ---- measurements ----------------------------------------------------------------------------------

struct TickTiming {
    std::vector<double> milliseconds;

    void add(double ms) { milliseconds.push_back(ms); }
    [[nodiscard]] double mean() const {
        double sum = 0;
        for (const double m : milliseconds) {
            sum += m;
        }
        return milliseconds.empty() ? 0.0 : sum / static_cast<double>(milliseconds.size());
    }
    [[nodiscard]] double percentile(double p) const {
        if (milliseconds.empty()) {
            return 0.0;
        }
        std::vector<double> sorted = milliseconds;
        std::sort(sorted.begin(), sorted.end());
        return sorted[static_cast<std::size_t>(p * static_cast<double>(sorted.size() - 1))];
    }
};

struct SystemTotals {
    std::uint64_t runs = 0;
    std::uint64_t work_units = 0;
};

struct Measurements {
    TickTiming timing;
    std::map<std::string, SystemTotals> systems;
    double wall_seconds = 0;
};

void print_summary(const Options& options, const Simulation& sim, const Measurements& m) {
    if (options.quiet || options.hash_only) {
        return;
    }
    const World& world = sim.world();
    const YearMonthDay gregorian = world.today().gregorian();
    std::printf("date            %s (Julian), Gregorian %04d-%02d-%02d\n",
                to_string(world.today()).c_str(),
                gregorian.year,
                gregorian.month,
                gregorian.day);
    std::printf("ticks           %lld\n", static_cast<long long>(sim.tick_count()));
    std::printf("dynasties       %zu\n", world.dynasties().size());
    std::printf("provinces       %zu\n", world.provinces().size());
    std::printf("characters      %zu alive\n", world.characters().ids.alive_count());
    std::printf("command log     %zu entries\n", sim.command_log().size());
    if (m.wall_seconds > 0 && sim.tick_count() > 0) {
        std::printf("wall time       %.3f s (%.0f days/s)\n",
                    m.wall_seconds,
                    static_cast<double>(sim.tick_count()) / m.wall_seconds);
    }
    if (!m.timing.milliseconds.empty()) {
        std::printf("tick time (ms)  mean %.4f  p50 %.4f  p95 %.4f  p99 %.4f  max %.4f\n",
                    m.timing.mean(),
                    m.timing.percentile(0.5),
                    m.timing.percentile(0.95),
                    m.timing.percentile(0.99),
                    m.timing.percentile(1.0));
    }
    for (const auto& [name, totals] : m.systems) {
        std::printf("system %-16s runs %-6llu work units %llu\n",
                    name.c_str(),
                    static_cast<unsigned long long>(totals.runs),
                    static_cast<unsigned long long>(totals.work_units));
    }
    std::printf("hash            %s\n", hash_line(world.compute_hash()).c_str());
}

// ---- running ---------------------------------------------------------------------------------------

/// Runs `days` ticks on the calling thread, collecting timings and (optionally) a replay.
void run_direct(Simulation& sim, int days, Measurements& m, ReplayRecorder* recorder) {
    const auto begin = Clock::now();
    for (int i = 0; i < days; ++i) {
        const auto t0 = Clock::now();
        const TickReport& report = sim.tick();
        const auto t1 = Clock::now();
        m.timing.add(std::chrono::duration<double, std::milli>(t1 - t0).count());
        for (const SystemRun& run : report.systems) {
            if (run.ran) {
                SystemTotals& totals = m.systems[std::string(run.name)];
                ++totals.runs;
                totals.work_units += run.wu_used;
            }
        }
        if (recorder != nullptr) {
            recorder->after_tick(sim);
        }
    }
    m.wall_seconds += std::chrono::duration<double>(Clock::now() - begin).count();
}

/// Runs through the threaded runner. Unlimited is exact (step(N)); paced speeds run until at least N days.
Simulation run_threaded(Simulation sim, int days, const std::string& speed_text, Measurements& m) {
    SimulationRunner runner(std::move(sim));
    // The target must be fixed BEFORE the thread is released, otherwise a fast thread could already
    // have run some of the days and the target would never be reached.
    const std::int64_t target = runner.ticks_done() + days;
    runner.start();
    const auto begin = Clock::now();
    if (speed_text == "unlimited") {
        runner.step(days);
    } else {
        runner.set_speed(static_cast<Speed>(speed_text[0] - '0'));
    }
    while (!runner.wait_for_ticks(target, std::chrono::milliseconds(1000))) {
        // keep waiting; progress is visible through ticks_done()
    }
    runner.set_speed(Speed::Paused);
    runner.stop();
    m.wall_seconds = std::chrono::duration<double>(Clock::now() - begin).count();
    return std::move(runner.simulation_when_stopped());
}

int write_outputs(const Options& options, const Simulation& sim, ReplayRecorder* recorder) {
    if (recorder != nullptr && options.record_path) {
        const Replay replay = recorder->finish(sim);
        const auto bytes = serialize_replay(replay);
        if (const auto r = write_file_atomic(*options.record_path, bytes); !r.has_value()) {
            return fail(kUsageOrIoError,
                        std::format("cannot write {}: {}", *options.record_path, r.error().message));
        }
        if (!options.quiet && !options.hash_only) {
            std::printf("replay written  %s (%zu bytes, %zu commands, %zu checkpoints)\n",
                        options.record_path->c_str(),
                        bytes.size(),
                        replay.commands.size(),
                        replay.checkpoints.size());
        }
    }
    if (options.save_path) {
        const auto bytes = serialize_save(sim.world(), sim.pending_internal());
        if (const auto r = write_file_atomic(*options.save_path, bytes); !r.has_value()) {
            return fail(kUsageOrIoError,
                        std::format("cannot write {}: {}", *options.save_path, r.error().message));
        }
        if (!options.quiet && !options.hash_only) {
            std::printf("save written    %s (%zu bytes)\n", options.save_path->c_str(), bytes.size());
        }
    }
    return kOk;
}

int play_replay_file(const Options& options) {
    const auto bytes = read_file(*options.replay_path);
    if (!bytes.has_value()) {
        return fail(kUsageOrIoError,
                    std::format("cannot read {}: {}", *options.replay_path, bytes.error().message));
    }
    const auto replay = deserialize_replay(bytes.value());
    if (!replay.has_value()) {
        return fail(kUsageOrIoError, std::format("invalid replay: {}", replay.error().message));
    }
    if (replay.value().scenario_id != demo::kScenarioId) {
        return fail(kUsageOrIoError, "the replay was recorded with a different scenario");
    }

    demo::Config config = options.scenario;
    config.seed = replay.value().seed;
    Simulation sim = demo::make_simulation(config);
    const auto begin = Clock::now();
    const ReplayResult result = play_replay(sim, replay.value());
    const double seconds = std::chrono::duration<double>(Clock::now() - begin).count();

    if (!result.error.empty()) {
        return fail(kUsageOrIoError, result.error);
    }
    if (!result.ok) {
        const ReplayDivergence& d = *result.divergence;
        std::fprintf(stderr,
                     "REPLAY DIVERGED at tick %lld in part '%s'\n  expected %s\n  actual   %s\n",
                     static_cast<long long>(d.tick),
                     d.first_differing_part.c_str(),
                     hash_line(d.expected).c_str(),
                     hash_line(d.actual).c_str());
        return kReplayDiverged;
    }
    if (options.hash_only) {
        std::printf("%016llx\n", static_cast<unsigned long long>(sim.world().compute_hash().total));
    } else if (!options.quiet) {
        std::printf("replay OK: %lld ticks in %.3f s, all %zu checkpoints match\n",
                    static_cast<long long>(result.ticks_run),
                    seconds,
                    replay.value().checkpoints.size());
    }
    return kOk;
}

/// Several independent ways of producing the same simulated history must agree.
int verify_determinism(const Options& options) {
    const int days = options.days;
    bool all_ok = true;
    const auto report = [&](const char* what, bool ok) {
        if (!options.quiet) {
            std::printf("%-48s %s\n", what, ok ? "OK" : "MISMATCH");
        }
        all_ok = all_ok && ok;
    };

    // A: the reference run, recorded as a replay.
    Measurements m;
    Simulation a = demo::make_simulation(options.scenario);
    ReplayRecorder recorder(
        options.scenario.seed, demo::kScenarioId, a.world().today(), options.checkpoint_interval);
    run_direct(a, days, m, &recorder);
    const WorldHash hash_a = a.world().compute_hash();
    const Replay replay = recorder.finish(a);

    // B: the same run again.
    Simulation b = demo::make_simulation(options.scenario);
    Measurements ignored;
    run_direct(b, days, ignored, nullptr);
    report("second direct run gives the same hash", b.world().compute_hash() == hash_a);

    // C: through the simulation thread.
    Simulation c = run_threaded(demo::make_simulation(options.scenario), days, "unlimited", ignored);
    report("threaded run gives the same hash", c.world().compute_hash() == hash_a);

    // D: save in the middle, load into a fresh simulation, continue.
    {
        Simulation first_half = demo::make_simulation(options.scenario);
        first_half.run_days(days / 2);
        const auto bytes = serialize_save(first_half.world(), first_half.pending_internal());
        auto loaded = deserialize_save(bytes);
        bool ok = loaded.has_value();
        if (ok) {
            Simulation resumed = demo::make_simulation(options.scenario);
            resumed.restore_state(std::move(loaded.value().world),
                                  std::move(loaded.value().pending_internal));
            resumed.run_days(days - days / 2);
            ok = resumed.world().compute_hash() == hash_a;
        }
        report("save at half time + load + continue gives the same hash", ok);
    }

    // E: replaying the recording.
    {
        Simulation fresh = demo::make_simulation(options.scenario);
        const ReplayResult result = play_replay(fresh, replay);
        report("replay reproduces every checkpoint", result.ok);
    }

    if (options.hash_only) {
        std::printf("%016llx\n", static_cast<unsigned long long>(hash_a.total));
    } else if (!options.quiet) {
        std::printf("final hash      %s\n", hash_line(hash_a).c_str());
        std::printf("%s\n", all_ok ? "determinism OK" : "determinism FAILED");
    }
    return all_ok ? kOk : kDeterminismFailed;
}

int run_session(const Options& options) {
    Simulation sim = demo::make_simulation(options.scenario);
    if (options.load_path) {
        const auto bytes = read_file(*options.load_path);
        if (!bytes.has_value()) {
            return fail(kUsageOrIoError,
                        std::format("cannot read {}: {}", *options.load_path, bytes.error().message));
        }
        auto loaded = deserialize_save(bytes.value());
        if (!loaded.has_value()) {
            return fail(kBadSave, std::format("invalid save: {}", loaded.error().message));
        }
        sim.restore_state(std::move(loaded.value().world), std::move(loaded.value().pending_internal));
    }

    std::optional<ReplayRecorder> recorder;
    if (options.record_path) {
        recorder.emplace(
            options.scenario.seed, demo::kScenarioId, sim.world().today(), options.checkpoint_interval);
    }

    Measurements m;
    if (options.speed) {
        if (recorder) {
            return fail(kUsageOrIoError, "--record needs the direct run (omit --speed)");
        }
        sim = run_threaded(std::move(sim), options.days, *options.speed, m);
    } else {
        run_direct(sim, options.days, m, recorder ? &*recorder : nullptr);
    }

    print_summary(options, sim, m);
    if (options.hash_only) {
        std::printf("%016llx\n", static_cast<unsigned long long>(sim.world().compute_hash().total));
    }
    return write_outputs(options, sim, recorder ? &*recorder : nullptr);
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (const std::string error = parse_args(argc, argv, options); !error.empty()) {
        std::fprintf(stderr, "simcli: %s\n\n%s", error.c_str(), kUsage);
        return kUsageOrIoError;
    }
    if (options.help) {
        std::fputs(kUsage, stdout);
        return kOk;
    }
    if (options.replay_path) {
        return play_replay_file(options);
    }
    if (options.verify_determinism) {
        return verify_determinism(options);
    }
    return run_session(options);
}
