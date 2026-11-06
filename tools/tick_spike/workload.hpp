#pragma once

#include "pat/sim/task_executor.hpp"
#include "pat/stress/dataset.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace pat::spike {

enum class MonthlyMode { Burst, Stagger };
enum Stage : std::size_t { Armies, Schemes, Economy, Trade, Demography, Ai, kStages };
inline constexpr std::array<std::string_view, kStages> kStageNames{
    "armies.daily", "schemes.weekly", "economy.monthly", "trade.monthly", "demography.yearly", "ai.daily"};

struct State {
    stress::Dataset data;
    std::size_t nodes = 30;
    std::vector<std::int64_t> population;  // province x 6 estates
    std::vector<std::int64_t> cash;
    std::vector<std::int64_t> production;  // province x 10 goods
    std::vector<std::int64_t> production_before;
    std::vector<std::int64_t> prices;  // node x 10 goods
    std::vector<std::int64_t> prices_before;
    std::vector<std::uint32_t> productivity;  // province x estate x good
    std::vector<std::uint32_t> traits;        // character x 8 attributes
    std::vector<std::uint32_t> rolls;
    std::vector<std::uint8_t> demographic_outcomes;
    std::vector<sim::DynastyId> ai_jobs;
    std::vector<sim::CharacterId> ai_choice;
    std::vector<std::int64_t> ai_score;
    std::size_t ai_cursor = 0;
    std::array<std::uint64_t, kStages> work{};
};

struct Sample {
    double tick_us = 0;
    std::array<double, kStages> stage_us{};
    std::uint64_t ai_wu = 0;
    // Highest calendar pulse on this tick: 0 daily, 1 weekly, 2 monthly, 3 yearly.
    int cadence = 0;
};

[[nodiscard]] core::Result<State> make_state(const stress::Config& config);
Sample
tick(State& state, sim::TaskExecutor& executor, std::size_t grain, MonthlyMode mode, std::uint64_t ai_quota);
[[nodiscard]] std::uint64_t state_hash(const State& state);

}  // namespace pat::spike
