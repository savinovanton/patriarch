#include "pat/core/telemetry.hpp"
#include "pat/sim/economy/rules.hpp"
#include "pat/sim/economy/systems.hpp"
#include "pat/sim/save.hpp"
#include "pat/sim/simulation.hpp"
#include "pat/stress/dataset.hpp"
#include "pat/stress/economy_bench.hpp"
#include "pat/stress/economy_generator.hpp"
#include "pat/stress/economy_skeleton.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

using namespace pat;

namespace {

constexpr std::size_t kEconomyPart = static_cast<std::size_t>(sim::ExtPart::Economy);

stress::Config economy_config(const char* profile, std::uint64_t seed) {
    auto preset = stress::profile_config(profile);
    REQUIRE(preset);
    auto config = preset.value();
    config.world.seed = seed;
    config.economy.enabled = true;
    return config;
}

}  // namespace

TEST_CASE("stressgen economy: the profiles are valid, reproducible and round-trip through the save") {
    for (const auto name : {"year1", "year60", "year60x3"}) {
        auto config = economy_config(name, 7);
        auto a = stress::generate(config);
        auto b = stress::generate(config);
        REQUIRE(a);
        REQUIRE(b);
        const auto& world = a.value().world;
        CAPTURE(name);
        REQUIRE(world.economy_present());
        CHECK(world.check_invariants().empty());
        CHECK(stress::check_workload(world, a.value().workload).empty());

        // Same seed, same hash: the whole dataset and every sub-hash of the economy.
        CHECK(stress::compute_hash(a.value()) == stress::compute_hash(b.value()));
        CHECK(world.compute_hash() == b.value().world.compute_hash());
        CHECK(world.economy_hash_parts() == b.value().world.economy_hash_parts());
        CHECK(world.economy() == b.value().world.economy());
        CHECK(world.compute_hash().ext[kEconomyPart] != 0);
        for (const auto part : world.economy_hash_parts()) {
            CHECK(part != 0);  // every table of the economy is filled
        }
        config.world.seed = 8;
        auto other = stress::generate(config);
        REQUIRE(other);
        CHECK(stress::compute_hash(other.value()) != stress::compute_hash(a.value()));

        // The tables have the shape of the profile.
        const auto& e = world.economy();
        CHECK(e.provinces.size() == config.world.provinces);
        CHECK(e.groups.rows() == 6U * config.world.provinces);
        CHECK(e.accounts.size() == 1U + config.world.dynasties);
        CHECK(static_cast<unsigned>(e.meta.closed_months) == 12U);
        CHECK(e.incidents.size() == sim::economy::kIncidentLogSize);
        CHECK(e.incidents.seq.front() > 0U);  // the ring has wrapped
        CHECK(e.outcomes.size() == sim::economy::kQuotaOutcomeRingSize);
        CHECK(e.loans.size() > 0);
        CHECK(e.quotas.size() > 0);
        CHECK(e.construction.size() > 0);
        CHECK(e.meta.rules_hash == stress::kStressRulesHash);
        // The statement ends in the month of the snapshot date and starts a year before it.
        CHECK(e.meta.open_month_start == sim::Date::from_ymd(world.today().year(), world.today().month(), 1));
        CHECK(e.accounts.ring_month_start(0) < e.meta.open_month_start);
        CHECK(e.accounts.ring_month_start(11) ==
              sim::Date::from_ymd(world.today().year() - 1, world.today().month(), 1));
        // Not just zeros: the crown and a dynasty have money, there are administrators and buildings.
        CHECK(world.treasury(sim::economy::AccountRef::crown()) != 0);
        CHECK(world.treasury(sim::economy::AccountRef::of(sim::DynastyId(0))) != 0);
        CHECK(std::any_of(e.provinces.administrator.begin(),
                          e.provinces.administrator.end(),
                          [](sim::CharacterId id) { return id.valid(); }));
        CHECK(std::any_of(e.provinces.slot_building.begin(),
                          e.provinces.slot_building.end(),
                          [](std::uint8_t b) { return b != 0; }));
        // The ledger is filled by postings and transfers: both the plain and the nonzero_lines codes appear.
        std::size_t nonzero_lines = 0;
        for (std::size_t m = 0; m < 12; ++m) {
            for (const auto line : e.accounts.ring_lines(0, m)) {
                nonzero_lines += line != 0 ? 1U : 0U;
            }
        }
        CHECK(nonzero_lines > 0);

        auto saved = sim::deserialize_save(sim::serialize_save(world));
        REQUIRE(saved);
        CHECK(saved.value().world.compute_hash() == world.compute_hash());
        CHECK(saved.value().world.economy() == world.economy());
    }
}

TEST_CASE("stressgen economy: off by default, and a world without it keeps its random state") {
    auto preset = stress::profile_config("year1");
    REQUIRE(preset);
    auto config = preset.value();
    config.world.seed = 5;
    auto plain = stress::generate(config);
    REQUIRE(plain);
    CHECK_FALSE(plain.value().world.economy_present());
    CHECK(plain.value().world.compute_hash().ext[kEconomyPart] == 0);
    CHECK(plain.value().world.economy_hash_parts() == std::array<std::uint64_t, 9>{});

    config.economy.enabled = true;
    auto filled = stress::generate(config);
    REQUIRE(filled);
    // The economy changes the hash (its part, the random streams, the provinces the crown takes over);
    // the people are not touched.
    CHECK(filled.value().world.compute_hash().total != plain.value().world.compute_hash().total);
    CHECK(filled.value().world.compute_hash().characters == plain.value().world.compute_hash().characters);
}

TEST_CASE("stressgen economy: the number of closed months and impossible requests") {
    for (const std::uint32_t months : {0U, 1U, 5U, 12U}) {
        stress::Config config;
        config.world = {3, 40, 200, 8, sim::Date::from_ymd(1703, 3, 1), true, 0};
        config.armies = config.schemes = config.contracts = config.events = 0;
        config.economy.enabled = true;
        config.economy.closed_months = months;
        auto generated = stress::generate(config);
        REQUIRE(generated);
        const auto& e = generated.value().world.economy();
        CHECK(static_cast<std::uint32_t>(e.meta.closed_months) == months);
        CHECK(e.meta.open_month_start == sim::Date::from_ymd(1703, 3, 1));
        CHECK(generated.value().world.check_invariants().empty());
    }

    stress::Config config;
    config.world = {3, 10, 50, 4, sim::Date::from_ymd(1703, 1, 1), true, 0};
    config.armies = config.schemes = config.contracts = config.events = 0;
    config.economy.enabled = true;
    config.economy.closed_months = 13;
    CHECK_FALSE(stress::generate(config));
    config.economy.closed_months = 12;
    config.economy.building_sites_permille = 1001;
    CHECK_FALSE(stress::generate(config));

    // A second run on a world that has an economy is refused, not merged.
    config.economy.building_sites_permille = 200;
    auto generated = stress::generate(config);
    REQUIRE(generated);
    CHECK_FALSE(stress::populate_economy(generated.value().world, config.economy));

    // No provinces, no dynasties: only the crown account, still valid.
    config.world = {1, 0, 0, 0, sim::Date::from_ymd(1703, 1, 1), true, 0};
    auto empty = stress::generate(config);
    REQUIRE(empty);
    CHECK(empty.value().world.economy().accounts.size() == 1);
    CHECK(empty.value().world.check_invariants().empty());
}

TEST_CASE("stressgen economy: memory per province is counted from the tables and stays in budget") {
    for (const auto name : {"year60", "year60x3"}) {
        auto generated = stress::generate(economy_config(name, 7));
        REQUIRE(generated);
        const auto& world = generated.value().world;
        const auto memory = stress::economy_memory(world);
        CAPTURE(name);
        CHECK(memory.provinces == world.provinces().size());
        CHECK(memory.groups_used == 6U * memory.provinces * (4 + 4 + 4 + 4 + 4));
        CHECK(memory.ledger_bytes ==
              (1U + memory.dynasties) * 8U *
                      (2 + sim::economy::kLedgerLineCount + 12 + 12 * sim::economy::kLedgerLineCount) +
                  12 * sizeof(sim::Date));
        CHECK(memory.used_total() == memory.used_without_ledger() + memory.ledger_bytes);
        CHECK(memory.allocated_total() >= memory.used_total());
        // GDD 03 section 13: about 1.5 KB per province, the ledger included.
        CHECK(memory.used_total() <= 1536U * memory.provinces);
    }
    CHECK(stress::economy_memory(stress::generate(stress::profile_config("year1").value()).value().world)
              .used_total() == 0);
}

TEST_CASE(
    "economy skeleton: the systems register as system.economy.*, count, and only the population moves") {
    auto generated = stress::generate(economy_config("year60", 7));
    REQUIRE(generated);
    const sim::World& world = generated.value().world;
    const auto provinces = world.provinces().size();

    sim::Simulation with(world);
    const auto counters = stress::install_economy_skeleton(with);
    REQUIRE(with.system_count() == 3);
    REQUIRE(with.enable_telemetry(512));
    with.run_days(365);  // 1762-01-01 -> 1763-01-01
    sim::Simulation without(world);
    without.run_days(365);

    // The stubs change nothing; the real `economy.provinces` of E9-T06 grows the population (a year of
    // peace: every group of a province is processed once a month) and touches only people and slots.
    CHECK(with.world().compute_hash() != without.world().compute_hash());
    CHECK(with.world().check_invariants().empty());
    CHECK(with.world().economy().accounts == without.world().economy().accounts);
    CHECK(with.world().economy().provinces.control == without.world().economy().provinces.control);
    CHECK(with.world().economy().groups.mood == without.world().economy().groups.mood);
    for (std::size_t p = 0; p < provinces; ++p) {
        CHECK(with.world().economy().provinces.last_slot_date[p] != sim::kNoDate);
    }
    // Daily systems every day, the monthly one on the 12 first days of a month, and every province
    // falls into its stagger slot exactly once a month.
    CHECK(counters->construction_runs == 365);
    CHECK(counters->province_runs == 365);
    CHECK(counters->settlement_runs == 12);
    CHECK(counters->slots_visited == 12U * provinces);

    // Registration order is the order of the spec, and the telemetry carries the system names.
    const auto& systems = with.last_report().systems;
    REQUIRE(systems.size() == 3);
    CHECK(systems[0].name == stress::kEconomySettlementName);
    CHECK(systems[1].name == stress::kEconomyConstructionName);
    CHECK(systems[2].name == stress::kEconomyProvincesName);
    const auto summary = with.telemetry_snapshot();
    REQUIRE(summary);
    for (const char* metric :
         {"system.economy.settlement", "system.economy.construction", "system.economy.provinces"}) {
        const auto found = std::find_if(summary->metrics.begin(),
                                        summary->metrics.end(),
                                        [&](const core::MetricSummary& m) { return m.name == metric; });
        REQUIRE(found != summary->metrics.end());
        CHECK(found->calls > 0);
    }
}

TEST_CASE("economy bench: the report of a small world is complete and consistent") {
    stress::Config config;
    config.world = {9, 40, 300, 10, sim::Date::from_ymd(1703, 1, 1), true, 0};
    config.armies = config.schemes = config.contracts = config.events = 0;
    stress::EconomyBenchOptions options;
    options.repeats = 2;
    options.heavy_repeats = 2;
    options.days = 60;
    auto report = stress::run_economy_bench(config, options);
    REQUIRE(report);
    const std::string& json = report.value();
    for (const char* key : {"\"memory\":",
                            "\"bytes_per_province\":",
                            "\"ledger_bytes\":",
                            "\"check_invariants_us\":",
                            "\"snapshot_copy_us\":",
                            "\"compute_hash_us\":",
                            "\"serialize_ms\":",
                            "\"deserialize_ms\":",
                            "\"cache_rebuild_floor_us\":",
                            "\"system.economy.provinces\"",
                            "\"system.economy.settlement\"",
                            "\"system.economy.construction\"",
                            "\"deterministic\":true",
                            "\"tick_us\":",
                            "\"month_starts\":",
                            "\"loaded_hash_equal\":true"}) {
        CAPTURE(key);
        CHECK(json.find(key) != std::string::npos);
    }
    options.days = 0;
    CHECK_FALSE(stress::run_economy_bench(config, options));
}

TEST_CASE("economy.provinces: fifty years of peace on the 400 provinces of the stress profile") {
    namespace eco = sim::economy;
    auto generated = stress::generate(economy_config("year60", 7));
    REQUIRE(generated);
    const sim::World& world = generated.value().world;
    REQUIRE(world.provinces().size() == 400);
    const auto rules = eco::EconomyRules::defaults();

    sim::Simulation a(world);
    eco::install_economy_systems(a, rules);
    sim::Simulation b(world);
    eco::install_economy_systems(b, rules);
    const std::vector<std::uint32_t> first = world.economy().groups.size;
    const auto total = [](const std::vector<std::uint32_t>& sizes) {
        std::uint64_t sum = 0;
        for (const std::uint32_t v : sizes) {
            sum += v;
        }
        return sum;
    };
    const std::uint64_t first_total = total(first);
    for (int year = 0; year < 50; ++year) {
        a.run_days(365 + (a.world().today().year() % 4 == 3 ? 1 : 0));
        b.run_days(365 + (b.world().today().year() % 4 == 3 ? 1 : 0));
    }
    // deterministic, and the state is valid
    CHECK(a.world().compute_hash() == b.world().compute_hash());
    CHECK(a.world().economy() == b.world().economy());
    CHECK(a.world().check_invariants().empty());

    const auto& last = a.world().economy().groups.size;
    // not exponential: the whole empire grows by less than 1.0 % a year on average
    const double factor = static_cast<double>(total(last)) / static_cast<double>(first_total);
    CHECK(factor >= 1.0);
    CHECK(factor < 1.65);  // 1.010 ^ 50
    // no group dies out or explodes (the random growth modifiers of the profile are +-0.5 % a year)
    for (std::size_t row = 0; row < last.size(); ++row) {
        if (first[row] >= 1000) {
            INFO("row " << row);
            CHECK(last[row] > 0);
            CHECK(last[row] < first[row] * 2U);
            CHECK(last[row] > first[row] / 2U);
        }
        CHECK(a.world().economy().groups.growth_carry[row] > -1'000'000);
        CHECK(a.world().economy().groups.growth_carry[row] < 1'000'000);
    }
    // every province is within the cap of persons and was processed in the last month
    for (std::uint32_t p = 0; p < 400; ++p) {
        CHECK(a.world().population_persons(sim::ProvinceId(p)) <= eco::kMaxProvincePersons);
        const sim::Date seen = a.world().economy().provinces.last_slot_date[p];
        CHECK(seen <= a.world().today());
        CHECK(a.world().today() - seen < 31);  // processed within the last month
    }
}
