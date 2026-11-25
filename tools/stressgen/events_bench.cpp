#include "pat/stress/events_bench.hpp"

#include "pat/core/rng.hpp"
#include "pat/sim/events/character_triggers.hpp"
#include "pat/sim/events/dispatch.hpp"
#include "pat/sim/events/event_set.hpp"
#include "pat/sim/world.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace pat::stress {

namespace {

namespace ev = sim::events;

std::uint64_t now_ns() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

/// A mix of subscriptions (docs/dev/event-engine.md section 12.6).
struct Mix {
    const char* name;
    std::uint32_t character_monthly;
    std::uint32_t province_monthly;
    std::uint32_t dynasty_monthly;
    std::uint32_t character_actions;
    std::uint32_t province_actions;
};

constexpr std::array<Mix, 3> kMixes{{
    {"m1_110", 40, 15, 10, 38, 7},
    {"e2t04_500", 80, 20, 0, 320, 80},
    {"release_550", 200, 75, 50, 190, 35},
}};

struct Flow {
    const char* name;
    std::uint32_t character_commands;  // per day
    std::uint32_t province_commands;   // per day
};

constexpr std::array<Flow, 2> kFlows{{
    {"realistic", 3, 0},  // about 1000 journal records a year (section 12.6)
    {"dense", 500, 40},   // 10% of the characters and provinces every day, as E2-T04
}};

ev::Program finish(ev::ProgramBuilder& b) {
    auto r = b.finish();
    return r ? std::move(r).value() : ev::Program{};
}

/// A cheap character trigger of 2..7 instructions, like the M1 events.
ev::Program character_trigger(core::Pcg32& rng) {
    ev::ProgramBuilder b;
    switch (rng.bounded(5)) {
        case 0:
            b.emit(ev::Op::kPred, ev::kTrigIsAdult, 0);
            break;
        case 1:
            b.emit(ev::Op::kLoad, ev::kValAge, 0);
            b.push_int(static_cast<std::int32_t>(16 + rng.bounded(40)));
            b.emit(ev::Op::kGe);
            break;
        case 2: {
            const auto no = b.new_label();
            b.emit(ev::Op::kPred, ev::kTrigIsFemale, 0);
            b.emit_jump(ev::Op::kJfKeep, no);
            b.emit(ev::Op::kDrop);
            b.emit(ev::Op::kLoad, ev::kValAge, 0);
            b.push_int(static_cast<std::int32_t>(16 + rng.bounded(30)));
            b.emit(ev::Op::kGe);
            b.bind(no);
            break;
        }
        case 3:
            b.emit(ev::Op::kPred, ev::kTrigIsMarried, 0);
            break;
        default:
            b.emit(ev::Op::kPred, ev::kTrigIsHeadOfDynasty, 0);
            break;
    }
    b.emit(ev::Op::kRet);
    return finish(b);
}

ev::Program dynasty_trigger(core::Pcg32& rng) {
    ev::ProgramBuilder b;
    b.emit(ev::Op::kLoad, ev::kValHonor, 0);
    b.push_int(static_cast<std::int32_t>(rng.bounded(60)) - 30);
    b.emit(ev::Op::kGe);
    b.emit(ev::Op::kRet);
    return finish(b);
}

ev::Program constant(std::int32_t value) {
    ev::ProgramBuilder b;
    b.push_int(value);
    b.emit(ev::Op::kRet);
    return finish(b);
}

std::shared_ptr<ev::EventSet> make_set(const Mix& mix, std::uint64_t seed) {
    core::Pcg32 rng(seed, 77);
    auto set = std::make_shared<ev::EventSet>();
    std::uint32_t n = 0;
    const auto add = [&](ev::ScopeKind scope, std::vector<ev::OnSub> on, bool pulse) {
        ev::EventDef def;
        def.id = std::format("bench.{}.{}", mix.name, n++);
        def.key = ev::event_key(def.id);
        def.scope = scope;
        def.on = std::move(on);
        def.priority = static_cast<std::int32_t>(rng.bounded(100));
        if (pulse) {
            def.mtth_days = static_cast<std::int32_t>(12 + rng.bounded(49)) * 30;
            def.mtth_ppm[static_cast<std::size_t>(ev::PulseKind::Monthly)] = ev::mtth_ppm(30, def.mtth_days);
        } else if (rng.bounded(10) < 3) {
            def.chance = constant(static_cast<std::int32_t>(20 + rng.bounded(61)));
        }
        if (scope == ev::ScopeKind::Character) {
            def.trigger = character_trigger(rng);
        } else if (scope == ev::ScopeKind::Dynasty) {
            def.trigger = dynasty_trigger(rng);
        }
        if (rng.bounded(4) == 0) {
            def.cooldown_days = 365;
        } else if (rng.bounded(10) == 0) {
            def.once = ev::OnceMode::Scope;
        }
        set->events.push_back(std::move(def));
    };
    // the on-actions of the characters: death, marriage (both spouses), trait, coming of age
    constexpr std::array<std::pair<std::uint16_t, std::uint8_t>, 5> kCharacterActions{{
        {ev::action::kDeath, 0},
        {ev::action::kMarriage, 0},
        {ev::action::kMarriage, 1},
        {ev::action::kTraitGained, 0},
        {ev::action::kComingOfAge, 0},
    }};
    for (std::uint32_t i = 0; i < mix.character_monthly; ++i) {
        add(ev::ScopeKind::Character, {ev::OnSub{2, 0}}, true);
    }
    for (std::uint32_t i = 0; i < mix.province_monthly; ++i) {
        add(ev::ScopeKind::Province, {ev::OnSub{2, 0}}, true);
    }
    for (std::uint32_t i = 0; i < mix.dynasty_monthly; ++i) {
        add(ev::ScopeKind::Dynasty, {ev::OnSub{2, 0}}, true);
    }
    for (std::uint32_t i = 0; i < mix.character_actions; ++i) {
        std::vector<ev::OnSub> on;
        const auto first =
            kCharacterActions[rng.bounded(static_cast<std::uint32_t>(kCharacterActions.size()))];
        on.push_back(ev::OnSub{first.first, first.second});
        if (rng.bounded(7) == 0) {  // every seventh listens to a second action, as in E2-T04
            const auto second =
                kCharacterActions[rng.bounded(static_cast<std::uint32_t>(kCharacterActions.size()))];
            if (second != first) {
                on.push_back(ev::OnSub{second.first, second.second});
            }
        }
        add(ev::ScopeKind::Character, std::move(on), false);
    }
    for (std::uint32_t i = 0; i < mix.province_actions; ++i) {
        add(ev::ScopeKind::Province, {ev::OnSub{ev::action::kProvinceOwnerChanged, 0}}, false);
    }
    std::ranges::sort(set->events,
                      [](const ev::EventDef& a, const ev::EventDef& b) { return a.key < b.key; });
    return set;
}

/// The commands of one day (handed to the dispatcher as the log of the tick, not applied).
void make_commands(const sim::World& world,
                   const Flow& flow,
                   core::Pcg32& rng,
                   std::vector<sim::LoggedCommand>& out) {
    out.clear();
    const auto living = world.living();
    if (living.size() < 2) {
        return;
    }
    const auto person = [&] { return living[rng.bounded(static_cast<std::uint32_t>(living.size()))]; };
    for (std::uint32_t i = 0; i < flow.character_commands; ++i) {
        sim::LoggedCommand entry;
        entry.date = world.today();
        entry.source = sim::CommandSource::Internal;
        switch (rng.bounded(4)) {
            case 0:
                entry.command.payload = sim::cmd::Die{person(), sim::CauseOfDeath::Illness};
                break;
            case 1:
                entry.command.payload = sim::cmd::Marry{person(), person()};
                break;
            case 2:
                entry.command.payload =
                    sim::cmd::GrantTrait{person(), static_cast<std::uint16_t>(1 + rng.bounded(40))};
                break;
            default:
                entry.command.payload = sim::cmd::ComeOfAge{person(), 0, {}};
                break;
        }
        out.push_back(entry);
    }
    const auto provinces = static_cast<std::uint32_t>(world.provinces().size());
    const auto dynasties = static_cast<std::uint32_t>(world.dynasties().size());
    for (std::uint32_t i = 0; i < flow.province_commands && provinces > 0 && dynasties > 0; ++i) {
        sim::LoggedCommand entry;
        entry.date = world.today();
        entry.source = sim::CommandSource::Internal;
        entry.command.payload = sim::cmd::SetProvinceOwner{sim::ProvinceId(rng.bounded(provinces)),
                                                           sim::DynastyId(rng.bounded(dynasties))};
        out.push_back(entry);
    }
}

struct Series {
    std::vector<double> us;

    void add(std::uint64_t ns) { us.push_back(static_cast<double>(ns) / 1000.0); }
    [[nodiscard]] std::string json() const {
        if (us.empty()) {
            return "{}";
        }
        std::vector<double> sorted = us;
        std::ranges::sort(sorted);
        double sum = 0;
        for (const double v : us) {
            sum += v;
        }
        const std::size_t p99 = std::min(sorted.size() - 1, (sorted.size() * 99) / 100);
        return std::format(R"({{"mean_us":{:.2f},"p99_us":{:.2f},"max_us":{:.2f}}})",
                           sum / static_cast<double>(us.size()),
                           sorted[p99],
                           sorted.back());
    }
};

struct Totals {
    std::uint64_t candidates = 0;  // before the roll at generation (every pulse slot)
    std::uint64_t gated = 0;       // what reaches the gates
    std::uint64_t vm_runs = 0;
    std::uint64_t launches = 0;
    std::uint64_t occurrences = 0;
    std::uint64_t rolls = 0;
};

}  // namespace

core::Result<std::string> run_events_bench(const Config& config, const EventsBenchOptions& options) {
    auto dataset = generate(config);
    if (!dataset) {
        return core::Result<std::string>::err(dataset.error());
    }
    const sim::World& base = dataset.value().world;
    ev::Catalog catalog = ev::Catalog::engine();
    ev::register_character_triggers(catalog);

    std::string scenarios;
    for (const Mix& mix : kMixes) {
        const auto set = make_set(mix, options.seed);
        for (const Flow& flow : kFlows) {
            sim::World fast = base;
            sim::World slow = base;
            ev::Dispatcher indexed(set, catalog);
            ev::Dispatcher scan(set, catalog);
            scan.set_mode(ev::DispatchMode::FullScan);
            indexed.set_clock(&now_ns);
            indexed.rebaseline(fast);
            scan.rebaseline(slow);
            core::Pcg32 rng(options.seed, 5);
            std::vector<sim::LoggedCommand> commands;
            Series total;
            Series sources;
            Series candidates;
            Series gates;
            Series vm;
            Series fire;
            Series full_scan;
            Totals totals;
            std::uint64_t mismatches = 0;
            for (std::uint32_t day = 0; day < options.warmup + options.days; ++day) {
                (void)fast.advance_day();
                (void)slow.advance_day();
                make_commands(fast, flow, rng, commands);
                const std::uint64_t t0 = now_ns();
                indexed.begin(fast);
                indexed.fire_timers(fast);
                indexed.collect_occurrences(fast, commands);
                const std::uint64_t t1 = now_ns();
                indexed.collect_candidates(fast);
                const std::uint64_t t2 = now_ns();
                indexed.gate(fast);
                const std::uint64_t t3 = now_ns();
                indexed.fire(fast);
                indexed.finish(fast);
                const std::uint64_t t4 = now_ns();
                scan.run(slow, commands);
                const std::uint64_t t5 = now_ns();
                if (scan.launches() != indexed.launches() || scan.candidates() != indexed.candidates()) {
                    ++mismatches;
                }
                if (day < options.warmup) {
                    continue;
                }
                total.add(t4 - t0);
                sources.add(t1 - t0);
                candidates.add(t2 - t1);
                gates.add(t3 - t2 - std::min(t3 - t2, indexed.vm_ns()));
                vm.add(indexed.vm_ns());
                fire.add(t4 - t3);
                full_scan.add(t5 - t4);
                const ev::DispatchStats& st = indexed.stats();
                totals.candidates += st.action_candidates + st.pulse_slots;
                totals.gated += st.action_candidates + st.pulse_candidates;
                totals.vm_runs += st.vm_runs;
                totals.launches += st.launches;
                totals.occurrences += st.occurrences;
                totals.rolls += st.rolls;
            }
            const auto days = static_cast<double>(options.days);
            if (!scenarios.empty()) {
                scenarios += ",";
            }
            scenarios += std::format(
                R"({{"events":"{}","flow":"{}","subscriptions":{},"per_day":{{"occurrences":{:.1f},"candidates":{:.1f},"gated":{:.1f},"rolls":{:.1f},"vm_runs":{:.1f},"launches":{:.2f}}},"total":{},"sources":{},"candidates":{},"gates":{},"vm":{},"fire":{},"full_scan_total":{},"index_equals_full_scan":{},"mismatched_days":{}}})",
                mix.name,
                flow.name,
                indexed.index().subscriptions(),
                static_cast<double>(totals.occurrences) / days,
                static_cast<double>(totals.candidates) / days,
                static_cast<double>(totals.gated) / days,
                static_cast<double>(totals.rolls) / days,
                static_cast<double>(totals.vm_runs) / days,
                static_cast<double>(totals.launches) / days,
                total.json(),
                sources.json(),
                candidates.json(),
                gates.json(),
                vm.json(),
                fire.json(),
                full_scan.json(),
                mismatches == 0 ? "true" : "false",
                mismatches);
        }
    }
    return core::Result<std::string>::ok(std::format(
        R"({{"tool":"stressgen --events-bench","characters":{},"provinces":{},"dynasties":{},"days":{},"warmup":{},"seed":{},"scenarios":[{}]}})",
        base.living().size(),
        base.provinces().size(),
        base.dynasties().size(),
        options.days,
        options.warmup,
        options.seed,
        scenarios));
}

}  // namespace pat::stress
