#include "pat/sim/simulation.hpp"
#include "pat/sim/task_executor.hpp"

#include <doctest/doctest.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace pat::sim;
using pat::core::Fixed32;
using pat::core::StringId;

namespace {

// ---- test systems -------------------------------------------------------------------------------

/// Calls a lambda whenever it runs; lets each test define behaviour inline.
class LambdaSystem final : public System {
public:
    using Body = std::function<void(World&, TickContext&)>;
    LambdaSystem(std::string name, Cadence cadence, Body body)
        : name_(std::move(name)), cadence_(cadence), body_(std::move(body)) {}

    [[nodiscard]] std::string_view name() const override { return name_; }
    [[nodiscard]] Cadence cadence() const override { return cadence_; }
    void run(World& world, TickContext& context) override { body_(world, context); }

private:
    std::string name_;
    Cadence cadence_;
    Body body_;
};

std::unique_ptr<System> make_system(std::string name, Cadence cadence, LambdaSystem::Body body) {
    return std::make_unique<LambdaSystem>(std::move(name), cadence, std::move(body));
}

World make_world(std::uint64_t seed = 1, int provinces = 0) {
    World world(Date::from_ymd(1703, 1, 1), seed);
    const DynastyId a = world.create_dynasty(StringId(0));
    (void)world.create_dynasty(StringId(1));
    for (int i = 0; i < provinces; ++i) {
        (void)world.create_province(StringId(100), a, Fixed32::from_int(10));
    }
    return world;
}

Command honor(std::int32_t delta) {
    return Command{DynastyId(), cmd::AdjustHonor{DynastyId(0), delta}};
}

}  // namespace

TEST_CASE("simulation: each tick advances the date by one day") {
    Simulation sim(make_world());
    CHECK(sim.world().today() == Date::from_ymd(1703, 1, 1));
    CHECK(sim.tick().date == Date::from_ymd(1703, 1, 2));
    sim.run_days(364);
    CHECK(sim.world().today() == Date::from_ymd(1704, 1, 1));  // 1703 has 365 days
    CHECK(sim.tick_count() == 365);
}

TEST_CASE("simulation: systems run at their cadence") {
    int daily = 0;
    int weekly = 0;
    int monthly = 0;
    int quarterly = 0;
    int yearly = 0;

    Simulation sim(make_world());
    sim.add_system(make_system("daily", Cadence::Daily, [&](World&, TickContext&) { ++daily; }));
    sim.add_system(make_system("weekly", Cadence::Weekly, [&](World&, TickContext&) { ++weekly; }));
    sim.add_system(make_system("monthly", Cadence::Monthly, [&](World&, TickContext&) { ++monthly; }));
    sim.add_system(make_system("quarterly", Cadence::Quarterly, [&](World&, TickContext&) { ++quarterly; }));
    sim.add_system(make_system("yearly", Cadence::Yearly, [&](World&, TickContext&) { ++yearly; }));

    sim.run_days(365 + 366 + 365 + 365);  // 1 January 1703 -> 1 January 1707
    CHECK(sim.world().today() == Date::from_ymd(1707, 1, 1));
    CHECK(daily == 1461);
    CHECK(weekly == 209);
    CHECK(monthly == 48);
    CHECK(quarterly == 16);
    CHECK(yearly == 4);
}

TEST_CASE("simulation: systems run in registration order and the report lists them") {
    std::vector<std::string> order;
    Simulation sim(make_world());
    sim.add_system(
        make_system("first", Cadence::Daily, [&](World&, TickContext&) { order.push_back("first"); }));
    sim.add_system(
        make_system("second", Cadence::Daily, [&](World&, TickContext&) { order.push_back("second"); }));
    sim.add_system(
        make_system("monthly", Cadence::Monthly, [&](World&, TickContext&) { order.push_back("monthly"); }));
    sim.add_system(
        make_system("third", Cadence::Daily, [&](World&, TickContext&) { order.push_back("third"); }));

    const TickReport& report = sim.tick();  // 2 January: no month boundary
    CHECK(order == std::vector<std::string>{"first", "second", "third"});
    REQUIRE(report.systems.size() == 4);
    CHECK(report.systems[0].name == "first");
    CHECK(report.systems[2].name == "monthly");
    CHECK_FALSE(report.systems[2].ran);
    CHECK(report.systems[3].ran);
}

TEST_CASE("simulation: work units are accounted per system and per tick") {
    Simulation sim(make_world(), SimulationConfig{1000, 64, 4096});
    sim.add_system(
        make_system("light", Cadence::Daily, [](World&, TickContext& ctx) { ctx.budget.spend(100); }));
    sim.add_system(
        make_system("weekly", Cadence::Weekly, [](World&, TickContext& ctx) { ctx.budget.spend(300); }));

    const TickReport& tuesday = sim.tick();  // Saturday 2 January 1703: not a Monday
    CHECK(tuesday.wu_quota == 1000);
    CHECK(tuesday.wu_used == 100);
    CHECK(tuesday.systems[0].wu_used == 100);
    CHECK(tuesday.systems[1].wu_used == 0);
    CHECK_FALSE(tuesday.over_budget);

    bool saw_weekly = false;
    for (int i = 0; i < 7; ++i) {
        const TickReport& r = sim.tick();
        if (r.systems[1].ran) {
            saw_weekly = true;
            CHECK(r.wu_used == 400);
            CHECK(r.systems[1].wu_used == 300);
        }
    }
    CHECK(saw_weekly);
}

TEST_CASE("simulation: the budget is a per-tick allowance that systems can query") {
    bool refused = false;
    bool granted = false;
    Simulation sim(make_world(), SimulationConfig{500, 64, 4096});
    sim.add_system(make_system("planner", Cadence::Daily, [&](World&, TickContext& ctx) {
        granted = ctx.budget.try_spend(400);
        refused = !ctx.budget.try_spend(400);  // only 100 left
        CHECK(ctx.budget.remaining() == 100);
    }));
    const TickReport& r = sim.tick();
    CHECK(granted);
    CHECK(refused);
    CHECK(r.wu_used == 400);
    CHECK_FALSE(r.over_budget);

    // The allowance is renewed every tick.
    refused = false;
    (void)sim.tick();
    CHECK(refused);
}

TEST_CASE("simulation: overspending is reported") {
    Simulation sim(make_world(), SimulationConfig{100, 64, 4096});
    sim.add_system(
        make_system("hog", Cadence::Daily, [](World&, TickContext& ctx) { ctx.budget.spend(250); }));
    const TickReport& r = sim.tick();
    CHECK(r.over_budget);
    CHECK(r.wu_used == 250);
    CHECK(r.wu_quota == 100);
}

TEST_CASE("simulation: external commands are applied at the start of the next tick, before systems") {
    World world = make_world();
    const DynastyId a(0);
    Simulation sim(std::move(world));

    Fixed32 honor_seen_by_system;
    sim.add_system(make_system("observer", Cadence::Daily, [&](World& w, TickContext&) {
        honor_seen_by_system = Fixed32::from_int(w.dynasties().honor[a.index()]);
    }));

    CHECK(sim.submit(honor(40)));
    CHECK(sim.world().dynasties().honor[a.index()] == 0);  // nothing happens until the tick

    const TickReport& r = sim.tick();
    CHECK(r.external_commands.applied == 1);
    CHECK(r.external_commands.rejected == 0);
    CHECK(sim.world().dynasties().honor[a.index()] == 40);
    CHECK(honor_seen_by_system == Fixed32::from_int(40));  // the system already saw the change

    REQUIRE(sim.command_log().size() == 1);
    CHECK(sim.command_log().entries()[0].date == Date::from_ymd(1703, 1, 2));
    CHECK(sim.command_log().entries()[0].source == CommandSource::External);
}

TEST_CASE("simulation: rejected external commands are logged but change nothing") {
    Simulation sim(make_world());
    // A dynasty may not adjust its own honor (system only).
    CHECK(sim.submit(Command{DynastyId(0), cmd::AdjustHonor{DynastyId(0), 500}}));
    const TickReport& r = sim.tick();
    CHECK(r.external_commands.applied == 0);
    CHECK(r.external_commands.rejected == 1);
    CHECK(sim.world().dynasties().honor[0] == 0);
    REQUIRE(sim.command_log().size() == 1);
    CHECK(sim.command_log().entries()[0].outcome == Rejection::NotAuthorized);
}

TEST_CASE("simulation: the external queue reports when it is full") {
    Simulation sim(make_world(), SimulationConfig{1000, 4, 4096});
    int accepted = 0;
    for (int i = 0; i < 10; ++i) {
        accepted += sim.submit(honor(1)) ? 1 : 0;
    }
    CHECK(accepted == 4);
    (void)sim.tick();
    CHECK(sim.world().dynasties().honor[0] == 4);
    CHECK(sim.submit(honor(1)));  // space again after the tick drained the queue
}

TEST_CASE("simulation: commands emitted by systems are applied on the next tick as Internal, before External "
          "ones") {
    Simulation sim(make_world());
    int emitted_on_tick = 0;
    sim.add_system(make_system("ai", Cadence::Daily, [&](World&, TickContext& ctx) {
        if (ctx.tick_index == 1) {
            ctx.emit(honor(5));
            ++emitted_on_tick;
        }
    }));

    (void)sim.tick();  // tick 1: the system emits
    CHECK(emitted_on_tick == 1);
    CHECK(sim.world().dynasties().honor[0] == 0);  // not applied within the same tick

    CHECK(sim.submit(honor(100)));
    const TickReport& r = sim.tick();  // tick 2: internal first, then external
    CHECK(r.internal_commands.applied == 1);
    CHECK(r.external_commands.applied == 1);
    CHECK(sim.world().dynasties().honor[0] == 105);

    const auto entries = sim.command_log().entries();
    REQUIRE(entries.size() == 2);
    CHECK(entries[0].source == CommandSource::Internal);
    CHECK(entries[1].source == CommandSource::External);
    CHECK(entries[0].date == entries[1].date);
}

TEST_CASE("simulation: the scratch arena is reset at the start of every tick") {
    std::vector<std::size_t> used_at_start;
    Simulation sim(make_world());
    sim.add_system(make_system("scratch", Cadence::Daily, [&](World&, TickContext& ctx) {
        used_at_start.push_back(ctx.scratch.bytes_used());
        (void)ctx.scratch.allocate_array<int>(1000);
    }));
    sim.run_days(5);
    REQUIRE(used_at_start.size() == 5);
    for (const std::size_t used : used_at_start) {
        CHECK(used == 0);
    }
}

TEST_CASE("simulation: a monthly staggered pass touches every province exactly once per month") {
    constexpr int kProvinces = 400;
    std::vector<int> touched(kProvinces, 0);

    Simulation sim(make_world(1, kProvinces));
    sim.add_system(make_system("economy", Cadence::Daily, [&](World& world, TickContext& ctx) {
        for (std::uint32_t id = 0; id < world.provinces().size(); ++id) {
            if (is_monthly_slot(id, ctx.date)) {
                ++touched[id];
            }
        }
    }));

    sim.run_days(31);  // 2 January .. 1 February 1703
    // January 1703: days 2..28 were visited (day 1 was before the first tick) and February 1 starts a new
    // cycle.
    int visited_this_month = 0;
    for (const int t : touched) {
        visited_this_month += t > 0 ? 1 : 0;
        CHECK(t <= 2);
    }
    CHECK(visited_this_month == kProvinces);
}

TEST_CASE("simulation: the serial executor covers the range exactly once, in order") {
    SerialExecutor executor;
    std::vector<std::pair<std::size_t, std::size_t>> chunks;
    executor.parallel_for(10, 4, [&](std::size_t b, std::size_t e) { chunks.emplace_back(b, e); });
    using Chunks = std::vector<std::pair<std::size_t, std::size_t>>;
    CHECK(chunks == Chunks{{0, 4}, {4, 8}, {8, 10}});

    chunks.clear();
    executor.parallel_for(0, 4, [&](std::size_t b, std::size_t e) { chunks.emplace_back(b, e); });
    CHECK(chunks.empty());

    chunks.clear();
    executor.parallel_for(
        3, 0, [&](std::size_t b, std::size_t e) { chunks.emplace_back(b, e); });  // grain 0 -> 1
    CHECK(chunks == Chunks{{0, 1}, {1, 2}, {2, 3}});
    CHECK(executor.concurrency() == 1);
}

TEST_CASE("simulation: systems can use the executor for data-parallel updates") {
    Simulation sim(make_world(1, 100));
    sim.add_system(make_system("growth", Cadence::Daily, [](World& world, TickContext& ctx) {
        auto population = world.province_population_mut();
        ctx.executor.parallel_for(population.size(), 16, [&](std::size_t begin, std::size_t end) {
            for (std::size_t i = begin; i < end; ++i) {
                population[i] = population[i] + Fixed32::from_ratio(1, 4);
            }
        });
    }));
    sim.run_days(8);
    for (const Fixed32 p : sim.world().provinces().population) {
        CHECK(p == Fixed32::from_int(12));  // 10 + 8 * 0.25
    }
}

namespace {

/// A small but non-trivial model: monthly births driven by the random "births" stream, a daily
/// health decay and an emitted honor reward. Exercises RNG, commands and world updates together.
void install_model(Simulation& sim) {
    sim.add_system(make_system("births", Cadence::Monthly, [](World& world, TickContext& ctx) {
        auto& rng = world.rng().stream("births");
        for (std::uint32_t d = 0; d < world.dynasties().size(); ++d) {
            if (rng.chance_permille(600)) {
                cmd::CreateCharacter child;
                child.dynasty = DynastyId(d);
                child.birth_date = ctx.date;
                child.sex = rng.chance_permille(500) ? Sex::Male : Sex::Female;
                ctx.emit(Command{DynastyId(), child});
                ctx.budget.spend(10);
            }
        }
    }));
    sim.add_system(make_system("ageing", Cadence::Daily, [](World& world, TickContext& ctx) {
        auto health = world.character_health_mut();
        world.characters().ids.for_each_alive([&](CharacterId id) {
            health[id.index()] = health[id.index()] - Fixed32::from_ratio(1, 64);
            ctx.budget.spend(1);
        });
    }));
    sim.add_system(make_system("rewards", Cadence::Quarterly, [](World&, TickContext& ctx) {
        ctx.emit(Command{DynastyId(), cmd::AdjustHonor{DynastyId(0), 25}});
    }));
}

WorldHash run_model(std::uint64_t seed, int days) {
    Simulation sim(make_world(seed));
    install_model(sim);
    sim.run_days(days);
    return sim.world().compute_hash();
}

}  // namespace

TEST_CASE("simulation: runs are deterministic (same seed, same hash) and seed-sensitive") {
    const WorldHash first = run_model(42, 800);
    const WorldHash second = run_model(42, 800);
    CHECK(first == second);

    CHECK(run_model(43, 800).total != first.total);
    CHECK(run_model(42, 801).total != first.total);
}

TEST_CASE("simulation: the world stays valid while the model runs") {
    Simulation sim(make_world(9));
    install_model(sim);
    for (int i = 0; i < 1000; ++i) {
        (void)sim.tick();
        if (i % 100 == 0) {
            const auto problems = sim.world().check_invariants();
            REQUIRE(problems.empty());
        }
    }
    CHECK(sim.world().characters().ids.alive_count() > 0);
}

TEST_CASE("simulation: replaying the logged external commands reproduces the run exactly") {
    // Original run: external commands arrive at arbitrary ticks.
    Simulation original(make_world(5));
    install_model(original);
    for (int day = 0; day < 400; ++day) {
        if (day % 37 == 0) {
            REQUIRE(original.submit(honor(day % 11 + 1)));
        }
        if (day % 91 == 0) {
            // an invalid one: rejected, and therefore not needed for the replay
            REQUIRE(original.submit(Command{DynastyId(1), cmd::AdjustHonor{DynastyId(1), 99}}));
        }
        (void)original.tick();
    }

    // Replay: same world, same systems, only the accepted external commands from the log.
    Simulation replay(make_world(5));
    install_model(replay);
    for (int day = 0; day < 400; ++day) {
        const Date next = replay.world().today() + 1;
        for (const Command& c : original.command_log().external_commands_on(next)) {
            REQUIRE(replay.submit(c));
        }
        (void)replay.tick();
    }

    CHECK(replay.world().compute_hash() == original.world().compute_hash());
    CHECK(replay.world().today() == original.world().today());
}

TEST_CASE("simulation: submit() from another thread is safe and every command is applied once") {
    constexpr int kCommands = 20000;
    Simulation sim(make_world(), SimulationConfig{1000, 1024, 4096});

    std::atomic<int> pushed{0};
    std::thread ui([&] {
        for (int i = 0; i < kCommands; ++i) {
            while (!sim.submit(honor(1))) {
                std::this_thread::yield();
            }
            pushed.fetch_add(1);
        }
    });

    std::size_t applied = 0;
    while (pushed.load() < kCommands || applied < static_cast<std::size_t>(kCommands)) {
        applied += sim.tick().external_commands.applied;
        if (pushed.load() >= kCommands && applied >= static_cast<std::size_t>(kCommands)) {
            break;
        }
    }
    ui.join();
    (void)sim.tick();  // drain anything left (nothing expected)

    CHECK(applied == static_cast<std::size_t>(kCommands));
    // honor is clamped, so verify through the log instead of the final value
    std::size_t accepted_in_log = 0;
    for (const LoggedCommand& e : sim.command_log().entries()) {
        accepted_in_log += e.accepted() ? 1 : 0;
    }
    CHECK(accepted_in_log == static_cast<std::size_t>(kCommands));
}
