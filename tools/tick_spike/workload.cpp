#include "workload.hpp"

#include "pat/core/hash_stream.hpp"
#include "pat/sim/calendar.hpp"
#include "pat/sim/work_budget.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <utility>

namespace pat::spike {
namespace {

using Clock = std::chrono::steady_clock;
constexpr std::size_t kEstates = 6;
constexpr std::size_t kGoods = 10;
constexpr std::size_t kTraits = 8;
constexpr std::size_t kCandidates = 32;

struct MonthlySlice {
    std::size_t start = 0;
    std::size_t stride = 1;
    std::size_t count = 0;
};

MonthlySlice monthly_slice(std::size_t count, sim::Date date, MonthlyMode mode) {
    if (mode == MonthlyMode::Burst) {
        return {0, 1, date.day() == 1 ? count : 0};
    }
    const auto start = static_cast<std::size_t>(date.day() - 1);
    return {start, 28, start < 28 && start < count ? (count - 1 - start) / 28 + 1 : 0};
}

}  // namespace

core::Result<State> make_state(const stress::Config& config) {
    using R = core::Result<State>;
    auto generated = stress::generate(config);
    if (!generated) {
        return R::err(generated.error());
    }
    State state{std::move(generated).value()};
    const auto provinces = state.data.world.provinces().size();
    const auto characters = state.data.world.characters().ids.capacity();
    const auto dynasties = state.data.world.dynasties().size();
    state.nodes = std::max<std::size_t>(1, (provinces * 30 + 399) / 400);
    state.population.resize(provinces * kEstates);
    state.cash.resize(provinces * kEstates, 0);
    state.productivity.resize(provinces * kEstates * kGoods);
    state.production.resize(provinces * kGoods);
    state.prices.resize(state.nodes * kGoods, 100);
    constexpr std::array<std::int64_t, kEstates> kShares{3, 5, 12, 65, 10, 5};
    for (std::size_t p = 0; p < provinces; ++p) {
        const auto people = static_cast<std::int64_t>(state.data.world.provinces().population[p].raw()) *
                            1000 / core::Fixed32::one_raw;
        for (std::size_t g = 0; g < kEstates; ++g) {
            state.population[p * kEstates + g] = std::max<std::int64_t>(1, people * kShares[g] / 100);
        }
        for (std::size_t good = 0; good < kGoods; ++good) {
            state.production[p * kGoods + good] = people / 5;
        }
    }
    auto& economy_rng = state.data.world.rng().stream("spike.economy");
    for (auto& coefficient : state.productivity) {
        coefficient = static_cast<std::uint32_t>(economy_rng.range(16, 80));
    }
    state.production_before = state.production;
    state.prices_before = state.prices;
    state.traits.resize(characters * kTraits);
    auto& traits_rng = state.data.world.rng().stream("spike.traits");
    for (auto& trait : state.traits) {
        trait = traits_rng.bounded(101);
    }
    state.rolls.resize(characters);
    state.demographic_outcomes.resize(characters, 0);
    state.ai_jobs.resize(dynasties);
    state.ai_choice.resize(dynasties);
    state.ai_score.resize(dynasties, 0);
    return R::ok(std::move(state));
}

Sample
tick(State& state, sim::TaskExecutor& executor, std::size_t grain, MonthlyMode mode, std::uint64_t ai_quota) {
    Sample sample;
    const auto begin = Clock::now();
    auto& world = state.data.world;
    auto& load = state.data.workload;
    const auto date = world.today();
    const auto events = sim::events_for(date);
    sample.cadence = events.new_year ? 3 : (events.new_month ? 2 : (events.new_week ? 1 : 0));
    // All monthly tasks read the same previous-month snapshot. Their order/day does not change
    // month-end results, so burst/stagger comparisons execute equivalent numerical work.
    if (events.new_month) {
        std::copy(state.production.begin(), state.production.end(), state.production_before.begin());
        std::copy(state.prices.begin(), state.prices.end(), state.prices_before.begin());
    }
    auto measure = [&](Stage stage, const auto& body) {
        const auto t0 = Clock::now();
        body();
        sample.stage_us[stage] = std::chrono::duration<double, std::micro>(Clock::now() - t0).count();
    };
    const auto provinces = world.provinces().size();
    measure(Armies, [&] {
        const auto count = load.armies.owner.size();
        executor.parallel_for(count, grain, [&](std::size_t first, std::size_t last) {
            for (auto i = first; i < last; ++i) {
                auto progress = load.armies.progress[i].raw();
                progress +=
                    core::Fixed32::one_raw / 8 + static_cast<std::int32_t>(load.armies.soldiers[i] % 2000);
                if (progress >= core::Fixed32::one_raw) {
                    progress -= core::Fixed32::one_raw;
                    load.armies.province[i] = load.armies.destination[i];
                    load.armies.destination[i] = sim::ProvinceId(static_cast<std::uint32_t>(
                        (load.armies.destination[i].index() + 1 + load.armies.owner[i].index() % 17) %
                        provinces));
                }
                load.armies.progress[i] = core::Fixed32::from_raw(progress);
            }
        });
        state.work[Armies] += count;
    });
    if (events.new_week) {
        measure(Schemes, [&] {
            const auto count = load.schemes.actor.size();
            executor.parallel_for(count, grain, [&](std::size_t first, std::size_t last) {
                for (auto i = first; i < last; ++i) {
                    if (date < load.schemes.next_check[i]) {
                        continue;
                    }
                    const auto actor = load.schemes.actor[i].index();
                    const auto target = load.schemes.target[i].index();
                    const auto skill = state.traits[actor * kTraits + 5];
                    const auto defense = state.traits[target * kTraits + 6];
                    const auto gain = 1 + skill * 20 / (defense + 20);
                    load.schemes.progress_permille[i] = (load.schemes.progress_permille[i] + gain) % 1001;
                    load.schemes.next_check[i] = date + 7;
                }
            });
            state.work[Schemes] += count;
        });
    }
    const auto slice = monthly_slice(provinces, date, mode);
    if (slice.count != 0) {
        measure(Economy, [&] {
            executor.parallel_for(slice.count, grain, [&](std::size_t first, std::size_t last) {
                for (auto ordinal = first; ordinal < last; ++ordinal) {
                    const auto p = slice.start + ordinal * slice.stride;
                    std::array<std::int64_t, kGoods> production{};
                    for (std::size_t g = 0; g < kEstates; ++g) {
                        const auto estate = p * kEstates + g;
                        const auto people = state.population[estate];
                        std::int64_t income = 0;
                        for (std::size_t good = 0; good < kGoods; ++good) {
                            const auto quantity = people * state.productivity[estate * kGoods + good] / 100;
                            production[good] += quantity;
                            income += quantity * state.prices_before[(p % state.nodes) * kGoods + good] / 100;
                        }
                        const auto tax = income * static_cast<std::int64_t>(30 + 15 * g) / 1000;
                        state.cash[estate] += income - tax - people * 2;
                        state.population[estate] += people * static_cast<std::int64_t>(g + 1) / 10000;
                    }
                    for (std::size_t good = 0; good < kGoods; ++good) {
                        state.production[p * kGoods + good] = production[good];
                    }
                }
            });
            state.work[Economy] += slice.count;
        });
    }
    const auto nodes = monthly_slice(state.nodes, date, mode);
    if (nodes.count != 0) {
        measure(Trade, [&] {
            executor.parallel_for(nodes.count * kGoods, grain, [&](std::size_t first, std::size_t last) {
                for (auto job = first; job < last; ++job) {
                    const auto node = nodes.start + (job / kGoods) * nodes.stride;
                    const auto good = job % kGoods;
                    std::int64_t supply = 0;
                    std::int64_t demand = 0;
                    for (auto p = node; p < provinces; p += state.nodes) {
                        supply += state.production_before[p * kGoods + good];
                        demand += static_cast<std::int64_t>(world.provinces().population[p].raw()) *
                                  static_cast<std::int64_t>(1 + good % 3) / 1000;
                    }
                    const auto adjustment = (demand - supply) * 100 / (demand + supply + 1);
                    const auto item = node * kGoods + good;
                    state.prices[item] = std::clamp<std::int64_t>(
                        state.prices_before[item] * (1000 + adjustment) / 1000, 1, 100000);
                }
            });
            state.work[Trade] += nodes.count * kGoods;
        });
    }
    if (events.new_year) {
        measure(Demography, [&] {
            auto& rng = world.rng().stream("spike.demography");
            for (auto& roll : state.rolls) {
                roll = rng.bounded(10000);  // Fixed order; included in the measured serial fraction.
            }
            auto health = world.character_health_mut();
            const auto& characters = world.characters();
            const auto year = date.year();
            executor.parallel_for(health.size(), grain, [&](std::size_t first, std::size_t last) {
                for (auto i = first; i < last; ++i) {
                    const auto age = std::max(0, year - characters.birth_date[i].year());
                    const auto h = health[i].raw() / core::Fixed32::one_raw;
                    std::uint32_t fitness = 0;
                    for (std::size_t trait = 0; trait < kTraits; ++trait) {
                        fitness += state.traits[i * kTraits + trait];
                    }
                    const auto risk = static_cast<std::uint32_t>(age * age / 50 + (100 - h) * 3);
                    const auto roll = state.rolls[i];
                    const bool dead = h == 0 || roll < risk;
                    const bool birth = !dead && characters.sex[i] == sim::Sex::Female && age >= 18 &&
                                       age <= 45 && roll % 1000 < fitness / kTraits;
                    state.demographic_outcomes[i] =
                        static_cast<std::uint8_t>((dead ? 1 : 0) | (birth ? 2 : 0));
                    health[i] = core::Fixed32::from_int(
                        dead ? 0 : std::max(0, h - (age > 50 ? 1 : 0) - (roll < 30 ? 3 : 0)));
                }
            });
            state.work[Demography] += health.size();
        });
    }
    measure(Ai, [&] {
        const auto dynasties = world.dynasties().size();
        const auto characters = world.characters().ids.capacity();
        sim::WorkBudget budget;
        budget.reset(ai_quota);
        std::size_t jobs = 0;
        if (characters != 0) {
            while (jobs < dynasties && budget.try_spend(kCandidates)) {
                state.ai_jobs[jobs] =
                    sim::DynastyId(static_cast<std::uint32_t>((state.ai_cursor + jobs) % dynasties));
                ++jobs;
            }
        }
        sample.ai_wu = budget.used();
        if (dynasties != 0) {
            state.ai_cursor = (state.ai_cursor + jobs) % dynasties;
        }
        const auto day = static_cast<std::size_t>(date.days());
        executor.parallel_for(jobs, grain, [&](std::size_t first, std::size_t last) {
            for (auto job = first; job < last; ++job) {
                const auto d = state.ai_jobs[job].index();
                std::int64_t best_score = std::numeric_limits<std::int64_t>::min();
                sim::CharacterId best;
                for (std::size_t k = 0; k < kCandidates; ++k) {
                    const auto c = (d * 73 + k * 131 + day) % characters;
                    std::int64_t score = world.characters().health[c].raw() / core::Fixed32::one_raw;
                    for (std::size_t trait = 0; trait < kTraits; ++trait) {
                        score += static_cast<std::int64_t>(state.traits[c * kTraits + trait]) *
                                 static_cast<std::int64_t>(1 + (d + trait * 3) % 7);
                    }
                    const auto candidate = sim::CharacterId(static_cast<std::uint32_t>(c));
                    if (score > best_score || (score == best_score && candidate < best)) {
                        best_score = score;
                        best = candidate;
                    }
                }
                state.ai_choice[d] = best;
                state.ai_score[d] = (state.ai_score[d] + best_score) % 1000000007;
            }
        });
        state.work[Ai] += sample.ai_wu;
    });
    world.advance_day();
    sample.tick_us = std::chrono::duration<double, std::micro>(Clock::now() - begin).count();
    return sample;
}

std::uint64_t state_hash(const State& state) {
    core::HashStream hash;
    hash.u64(stress::compute_hash(state.data));
    hash.u64(state.ai_cursor);
    auto values = [&](const auto& vector) {
        hash.u64(vector.size());
        for (const auto value : vector) {
            hash.i64(value);
        }
    };
    values(state.population);
    values(state.cash);
    values(state.production);
    values(state.production_before);
    values(state.prices);
    values(state.prices_before);
    values(state.demographic_outcomes);
    values(state.ai_score);
    for (const auto id : state.ai_choice) {
        hash.id(id);
    }
    for (const auto count : state.work) {
        hash.u64(count);
    }
    return hash.value();
}

}  // namespace pat::spike
