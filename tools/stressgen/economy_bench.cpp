#include "pat/stress/economy_bench.hpp"

#include "pat/core/bytes.hpp"
#include "pat/core/telemetry.hpp"
#include "pat/sim/save.hpp"
#include "pat/sim/simulation.hpp"
#include "pat/stress/economy_generator.hpp"
#include "pat/stress/economy_skeleton.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace pat::stress {

namespace {

using Clock = std::chrono::steady_clock;

/// Distribution of repeated timings, in the unit the samples were taken in.
struct Stat {
    std::size_t n = 0;
    double min = 0;
    double median = 0;
    double mean = 0;
    double p95 = 0;
    double max = 0;
};

Stat summarize(std::vector<double> samples) {
    Stat out;
    if (samples.empty()) {
        return out;
    }
    std::sort(samples.begin(), samples.end());
    out.n = samples.size();
    out.min = samples.front();
    out.max = samples.back();
    out.median = samples[samples.size() / 2];
    double total = 0;
    for (const double v : samples) {
        total += v;
    }
    out.mean = total / static_cast<double>(samples.size());
    const auto rank = static_cast<std::size_t>(std::ceil(0.95 * static_cast<double>(samples.size())));
    out.p95 = samples[std::min(samples.size(), std::max<std::size_t>(rank, 1)) - 1];
    return out;
}

/// Runs `body` `repeats` times (after one untimed warm-up run) and returns the durations in
/// microseconds.
template <class F>
std::vector<double> time_us(std::uint32_t repeats, F&& body) {
    body();
    std::vector<double> samples;
    samples.reserve(repeats);
    for (std::uint32_t i = 0; i < repeats; ++i) {
        const auto start = Clock::now();
        body();
        const auto stop = Clock::now();
        samples.push_back(std::chrono::duration<double, std::micro>(stop - start).count());
    }
    return samples;
}

std::string json_stat(const Stat& s, double divisor = 1.0) {
    return std::format(
        "{{\"n\":{},\"min\":{:.3f},\"median\":{:.3f},\"mean\":{:.3f},\"p95\":{:.3f},\"max\":{:.3f}}}",
        s.n,
        s.min / divisor,
        s.median / divisor,
        s.mean / divisor,
        s.p95 / divisor,
        s.max / divisor);
}

std::string hex64(std::uint64_t value) {
    return std::format("\"{:016x}\"", value);
}

/// A read-only pass over every column the modifier cache of E9-T17 will read for a province: a floor
/// for the cost of rebuilding it (the real rebuild also computes). The sum keeps the optimizer honest.
std::uint64_t sweep_economy(const sim::World& world) {
    namespace eco = sim::economy;
    const eco::EconomyState& e = world.economy();
    std::uint64_t acc = 0;
    for (std::size_t p = 0; p < e.provinces.size(); ++p) {
        acc += e.provinces.kind[p] + e.provinces.owner_kind[p] + e.provinces.tax_policy[p] +
               e.provinces.recruit_policy[p] + e.provinces.slots_total[p];
        acc += static_cast<std::uint64_t>(e.provinces.control[p].raw()) +
               static_cast<std::uint64_t>(e.provinces.corruption[p].raw()) +
               static_cast<std::uint64_t>(e.provinces.unrest[p].raw());
        for (std::size_t g = 0; g < eco::kGroupCount; ++g) {
            const std::size_t row = eco::group_row(p, static_cast<eco::Group>(g));
            acc += e.groups.size[row] + static_cast<std::uint64_t>(e.groups.mood[row].raw()) +
                   static_cast<std::uint64_t>(e.groups.wealth[row].raw());
        }
        for (std::size_t good = 0; good < eco::kMaxGoods; ++good) {
            acc += static_cast<std::uint64_t>(e.provinces.capacity[p * eco::kMaxGoods + good].raw());
        }
        for (std::size_t slot = 0; slot < eco::kMaxSlots; ++slot) {
            acc += e.provinces.slot_building[p * eco::kMaxSlots + slot];
        }
    }
    return acc;
}

std::string json_memory(const EconomyMemory& m) {
    const auto per_province = [&](std::size_t bytes) {
        return m.provinces == 0 ? 0.0 : static_cast<double>(bytes) / static_cast<double>(m.provinces);
    };
    std::string out = std::format("{{\"provinces\":{},\"dynasties\":{}", m.provinces, m.dynasties);
    out += std::format(
        ",\"tables_used_bytes\":{{\"groups\":{},\"provinces\":{},\"dynasties\":{},\"construction\":{},"
        "\"loans\":{},\"quotas\":{},\"outcomes\":{},\"incidents\":{},\"ledger\":{}}}",
        m.groups_used,
        m.province_used,
        m.dynasty_used,
        m.construction_used,
        m.loans_used,
        m.quotas_used,
        m.outcomes_used,
        m.incidents_used,
        m.ledger_bytes);
    out += std::format(",\"economy_bytes\":{},\"economy_bytes_without_ledger\":{},\"ledger_bytes\":{}",
                       m.used_total(),
                       m.used_without_ledger(),
                       m.ledger_bytes);
    out += std::format(",\"bytes_per_province\":{:.1f},\"bytes_per_province_without_ledger\":{:.1f},"
                       "\"ledger_bytes_per_province\":{:.1f}",
                       per_province(m.used_total()),
                       per_province(m.used_without_ledger()),
                       per_province(m.ledger_bytes));
    out += std::format(",\"allocated_bytes\":{},\"allocated_bytes_without_ledger\":{},"
                       "\"allocated_bytes_per_province\":{:.1f}}}",
                       m.allocated_total(),
                       m.allocated_without_ledger(),
                       per_province(m.allocated_total()));
    return out;
}

/// The timings of the checks and of the copy of one world.
struct WorldTimings {
    Stat invariants_us;
    Stat hash_us;
    Stat snapshot_copy_us;
    Stat snapshot_free_us;
};

WorldTimings time_world(const sim::World& world, std::uint32_t repeats, std::uint64_t& sink) {
    WorldTimings out;
    out.invariants_us = summarize(time_us(repeats, [&] { sink += world.check_invariants().size(); }));
    out.hash_us = summarize(time_us(repeats, [&] { sink ^= world.compute_hash().total; }));
    // The copy (a snapshot for the UI) and its release are timed one after the other.
    std::vector<double> copy;
    std::vector<double> release;
    for (std::uint32_t i = 0; i < repeats + 1; ++i) {
        const auto start = Clock::now();
        auto snapshot = world.snapshot();
        const auto mid = Clock::now();
        sink += snapshot->provinces().size();
        snapshot.reset();
        const auto stop = Clock::now();
        if (i > 0) {  // the first round is the warm-up
            copy.push_back(std::chrono::duration<double, std::micro>(mid - start).count());
            release.push_back(std::chrono::duration<double, std::micro>(stop - mid).count());
        }
    }
    out.snapshot_copy_us = summarize(std::move(copy));
    out.snapshot_free_us = summarize(std::move(release));
    return out;
}

std::string json_timings(const WorldTimings& t) {
    return std::format("{{\"check_invariants_us\":{},\"compute_hash_us\":{},\"snapshot_copy_us\":{},"
                       "\"snapshot_free_us\":{}}}",
                       json_stat(t.invariants_us),
                       json_stat(t.hash_us),
                       json_stat(t.snapshot_copy_us),
                       json_stat(t.snapshot_free_us));
}

std::size_t raw_state_bytes(const sim::World& world) {
    core::ByteWriter writer;
    world.save_state(writer);
    return writer.size();
}

std::string json_metric(const core::MetricSummary& m) {
    const auto dist = [](const core::DurationDistribution& d) {
        return std::format(
            "{{\"samples\":{},\"mean_ns\":{},\"p50_ns\":{},\"p95_ns\":{},\"p99_ns\":{},\"max_ns\":{}}}",
            d.samples,
            d.mean_ns,
            d.p50_ns,
            d.p95_ns,
            d.p99_ns,
            d.max_ns);
    };
    return std::format("{{\"name\":\"{}\",\"calls\":{},\"active_ticks\":{},\"per_tick\":{},\"active\":{}}}",
                       m.name,
                       m.calls,
                       m.active_ticks,
                       dist(m.per_tick),
                       dist(m.active));
}

}  // namespace

core::Result<std::string> run_economy_bench(const Config& config, const EconomyBenchOptions& options) {
    using R = core::Result<std::string>;
    if (options.repeats == 0 || options.heavy_repeats == 0 || options.days == 0 || options.days > 16000) {
        return R::err({5, "bench repeats and days are out of range"});
    }

    // The world without economy first, then the same world with it filled in: the difference of the
    // two is what the economy costs.
    Config plain_config = config;
    plain_config.economy.enabled = false;
    const auto generation_start = Clock::now();
    auto plain = generate(plain_config);
    const auto generation_stop = Clock::now();
    if (!plain) {
        return R::err(plain.error());
    }
    const sim::World& baseline = plain.value().world;
    sim::World world = baseline;  // a copy: it becomes the world with the economy
    EconomyConfig economy_config = config.economy;
    economy_config.enabled = true;
    const auto economy_start = Clock::now();
    if (const auto filled = populate_economy(world, economy_config); !filled) {
        return R::err(filled.error());
    }
    const auto economy_stop = Clock::now();
    const auto ms = [](Clock::time_point a, Clock::time_point b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    };

    std::uint64_t sink = 0;
    const EconomyMemory memory = economy_memory(world);
    const WorldTimings base_times = time_world(baseline, options.repeats, sink);
    const WorldTimings eco_times = time_world(world, options.repeats, sink);
    const Stat parts_us = summarize(time_us(options.repeats, [&] { sink ^= world.economy_hash_parts()[0]; }));

    // ---- saving --------------------------------------------------------------------------------
    const std::size_t raw_baseline = raw_state_bytes(baseline);
    const std::size_t raw_economy = raw_state_bytes(world);
    const Stat state_us = summarize(time_us(options.heavy_repeats, [&] { sink += raw_state_bytes(world); }));
    std::vector<std::uint8_t> save;
    const Stat serialize_us =
        summarize(time_us(options.heavy_repeats, [&] { save = sim::serialize_save(world); }));
    const std::size_t file_baseline = sim::serialize_save(baseline).size();

    double read_median_us = 0;
    Stat write_us;
    Stat read_us;
    if (!options.scratch.empty()) {
        std::vector<double> writes;
        std::vector<double> reads;
        for (std::uint32_t i = 0; i < options.heavy_repeats + 1; ++i) {
            const auto w0 = Clock::now();
            if (const auto written = sim::write_file_atomic(options.scratch, save); !written) {
                return R::err(written.error());
            }
            const auto w1 = Clock::now();
            auto read = sim::read_file(options.scratch);
            const auto w2 = Clock::now();
            if (!read) {
                return R::err(read.error());
            }
            sink += read.value().size();
            if (i > 0) {
                writes.push_back(std::chrono::duration<double, std::micro>(w1 - w0).count());
                reads.push_back(std::chrono::duration<double, std::micro>(w2 - w1).count());
            }
        }
        write_us = summarize(std::move(writes));
        read_us = summarize(std::move(reads));
        read_median_us = read_us.median;
        if (options.remove_scratch) {
            std::error_code ignored;
            std::filesystem::remove(options.scratch, ignored);
        }
    }

    // ---- loading: the load of the game (zstd, parse, check_invariants) ---------------------------
    bool loaded_hash_equal = false;
    sim::SaveData loaded{sim::World(sim::Date::from_ymd(1703, 1, 1), 1), {}};
    const Stat deserialize_us = summarize(time_us(options.heavy_repeats, [&] {
        auto result = sim::deserialize_save(save);
        if (result) {
            loaded = std::move(result).value();
        }
    }));
    loaded_hash_equal = loaded.world.compute_hash() == world.compute_hash();
    if (!loaded_hash_equal) {
        return R::err({5, "the loaded economy world has another hash than the saved one"});
    }
    const Stat sweep_us = summarize(time_us(options.repeats, [&] { sink += sweep_economy(loaded.world); }));
    const EconomyMemory loaded_memory = economy_memory(loaded.world);

    // ---- the skeleton of the economy systems on the loaded world ---------------------------------
    const sim::WorldHash hash_before = loaded.world.compute_hash();
    sim::Simulation with_stubs(std::move(loaded.world));
    const auto counters = install_economy_skeleton(with_stubs);
    if (const auto enabled = with_stubs.enable_telemetry(1024); !enabled) {
        return R::err(enabled.error());
    }
    with_stubs.run_days(static_cast<int>(options.days));
    const auto telemetry = with_stubs.telemetry_snapshot();
    sim::Simulation without_stubs(world);
    without_stubs.run_days(static_cast<int>(options.days));
    const bool skeleton_is_inert = with_stubs.world().compute_hash() == without_stubs.world().compute_hash();
    if (!skeleton_is_inert) {
        return R::err({5, "the skeleton of the economy systems changed the world"});
    }

    // ---- the report ----------------------------------------------------------------------------
    std::string metrics;
    if (telemetry) {
        for (const core::MetricSummary& m : telemetry->metrics) {
            if (m.name.starts_with("system.economy.") || m.name == "sim.tick") {
                metrics += (metrics.empty() ? "" : ",") + json_metric(m);
            }
        }
    }
    const double load_ms = (read_median_us + deserialize_us.median) / 1000.0;
    std::string out =
        std::format("{{\"report\":\"E9-T05 economy state\",\"profile_world\":{{\"seed\":{},\"date\":\"{}\","
                    "\"provinces\":{},\"dynasties\":{},\"characters\":{},\"closed_months\":{}}}",
                    config.world.seed,
                    sim::to_string(world.today()),
                    world.provinces().size(),
                    world.dynasties().size(),
                    world.characters().ids.alive_count(),
                    economy_config.closed_months);
    out += std::format(",\"options\":{{\"repeats\":{},\"heavy_repeats\":{},\"days\":{},\"disk\":{}}}",
                       options.repeats,
                       options.heavy_repeats,
                       options.days,
                       options.scratch.empty() ? "false" : "true");
    out += std::format(",\"generation_ms\":{{\"world\":{:.3f},\"economy\":{:.3f}}}",
                       ms(generation_start, generation_stop),
                       ms(economy_start, economy_stop));
    out += ",\"memory\":" + json_memory(memory);
    out += ",\"memory_after_load\":" + json_memory(loaded_memory);
    out += ",\"without_economy\":" + json_timings(base_times);
    out += ",\"with_economy\":" + json_timings(eco_times);
    out += ",\"economy_hash_parts_us\":" + json_stat(parts_us);
    out += std::format(
        ",\"save\":{{\"raw_bytes_without_economy\":{},\"raw_bytes\":{},\"raw_bytes_of_economy\":{},"
        "\"file_bytes_without_economy\":{},\"file_bytes\":{},\"file_bytes_of_economy\":{},"
        "\"save_state_ms\":{},\"serialize_ms\":{},\"write_ms\":{},\"read_ms\":{},"
        "\"deserialize_ms\":{},\"load_ms\":{:.3f}}}",
        raw_baseline,
        raw_economy,
        raw_economy - raw_baseline,
        file_baseline,
        save.size(),
        static_cast<std::int64_t>(save.size()) - static_cast<std::int64_t>(file_baseline),
        json_stat(state_us, 1000.0),
        json_stat(serialize_us, 1000.0),
        options.scratch.empty() ? std::string("null") : json_stat(write_us, 1000.0),
        options.scratch.empty() ? std::string("null") : json_stat(read_us, 1000.0),
        json_stat(deserialize_us, 1000.0),
        load_ms);
    out += ",\"cache_rebuild_floor_us\":" + json_stat(sweep_us);
    out += std::format(
        ",\"skeleton\":{{\"days\":{},\"settlement_runs\":{},\"construction_runs\":{},\"province_runs\":{},"
        "\"slots_visited\":{},\"hash_unchanged\":{},\"metrics\":[{}]}}",
        options.days,
        counters->settlement_runs,
        counters->construction_runs,
        counters->province_runs,
        counters->slots_visited,
        skeleton_is_inert ? "true" : "false",
        metrics);
    out += std::format(",\"world_hash\":{},\"economy_hash\":{},\"loaded_hash_equal\":{},\"sink\":{}}}",
                       hex64(hash_before.total),
                       hex64(hash_before.ext[static_cast<std::size_t>(sim::ExtPart::Economy)]),
                       loaded_hash_equal ? "true" : "false",
                       hex64(sink));
    return R::ok(std::move(out));
}

}  // namespace pat::stress
