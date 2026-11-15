#include "pat/sim/marriage_candidates.hpp"

#include <benchmark/benchmark.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace pat::bench {
namespace {

// E8-T06 acceptance workload: 5,000 living nobles in 100 houses. Parents, spouses,
// godparents and directed opinions exercise the complete query, including unavailable rows.
// Setup and all validation stay outside the measured loop; the optimized budget is 1 ms/query.
void marriage_candidates(benchmark::State& state) {
    constexpr std::size_t kHouses = 100;
    constexpr std::size_t kPeoplePerHouse = 50;
    sim::World world(sim::Date::from_ymd(1703, 1, 1), 301);
    core::StringInterner names;
    std::array<sim::DynastyId, kHouses> houses{};
    std::array<std::array<sim::CharacterId, kPeoplePerHouse>, kHouses> people{};
    if (!world.enable_dynasty_mode()) {
        state.SkipWithError("cannot enable dynasty mode");
        return;
    }
    for (std::size_t h = 0; h < kHouses; ++h) {
        houses[h] = world.create_dynasty(names.intern("House " + std::to_string(h)));
        world.add_honor(houses[h], static_cast<std::int32_t>(h * 17) - 200);
        for (std::size_t p = 0; p < kPeoplePerHouse; ++p) {
            sim::CharacterInit init;
            init.dynasty = houses[h];
            init.name = names.intern("Person " + std::to_string(h) + " " + std::to_string(p));
            init.sex = p % 2 == 0 ? sim::Sex::Male : sim::Sex::Female;
            const auto age = p < 10 ? 75 + p % 5 : p < 30 ? 45 + p % 8 : 16 + p % 14;
            init.birth_date = sim::Date::from_ymd(1703 - static_cast<int>(age), 1, 1);
            init.health = core::Fixed32::from_int(40 + static_cast<int>(p % 50));
            init.fertility = core::Fixed32::from_int(50);
            init.skills.fill(core::Fixed32::from_int(static_cast<int>(p % 17)));
            if (p >= 10) {
                const auto parent = p < 30 ? ((p - 10) / 4) * 2 : 10 + ((p - 30) / 4) * 2;
                init.father = people[h][parent];
                init.mother = people[h][parent + 1];
            }
            people[h][p] = world.create_character(init);
            if (!people[h][p].valid()) {
                state.SkipWithError("cannot create benchmark population");
                return;
            }
        }
        if (!world.set_dynasty_head(houses[h], people[h][0])) {
            state.SkipWithError("cannot set benchmark house head");
            return;
        }
    }
    for (std::size_t h = 0; h < kHouses; ++h) {
        const auto next = (h + 1) % kHouses;
        // Cross-house spouses avoid sibling marriages in the fixture.
        for (const std::size_t p : {std::size_t{0}, std::size_t{10}, std::size_t{20}}) {
            if (!world.marry(people[h][p], people[next][p + 1])) {
                state.SkipWithError("cannot create benchmark spouse links");
                return;
            }
        }
        for (std::size_t p = 30; p < kPeoplePerHouse; ++p) {
            if (!world.set_character_godparents(people[h][p], {people[next][10], people[next][11]})) {
                state.SkipWithError("cannot create benchmark godparent links");
                return;
            }
        }
        for (std::size_t offset = 1; offset <= 5; ++offset) {
            if (!world.add_opinion(people[h][2],
                                   people[(h + offset) % kHouses][0],
                                   core::Fixed32::from_int(static_cast<int>((h + offset) % 81) - 40),
                                   1)) {
                state.SkipWithError("cannot create benchmark opinions");
                return;
            }
        }
    }
    const auto errors = world.check_invariants();
    if (!errors.empty()) {
        state.SkipWithError(errors.front().c_str());
        return;
    }
    const auto rules = sim::MarriageRules::defaults();
    sim::CandidateQuery query;
    query.member = people[0][30];
    query.names = &names;
    for (auto _ : state) {
        auto result = sim::find_candidates(world, rules, query);
        benchmark::DoNotOptimize(result);
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(kHouses * kPeoplePerHouse));
    state.SetLabel("5000 living / 100 houses; full query; budget <= 1 ms");
}

BENCHMARK(marriage_candidates)->Name("sim.marriage_candidates")->UseRealTime()->Unit(benchmark::kMicrosecond);

}  // namespace
}  // namespace pat::bench
