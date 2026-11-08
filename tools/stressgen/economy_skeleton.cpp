#include "pat/stress/economy_skeleton.hpp"

#include "pat/sim/economy/types.hpp"
#include "pat/sim/system.hpp"

#include <memory>
#include <string_view>
#include <utility>

namespace pat::stress {

namespace {

class SettlementStub final : public sim::System {
public:
    explicit SettlementStub(std::shared_ptr<EconomySkeletonCounters> counters)
        : counters_(std::move(counters)) {}
    [[nodiscard]] std::string_view name() const override { return kEconomySettlementName; }
    [[nodiscard]] sim::Cadence cadence() const override { return sim::Cadence::Monthly; }
    void run(sim::World&, sim::TickContext&) override { ++counters_->settlement_runs; }

private:
    std::shared_ptr<EconomySkeletonCounters> counters_;
};

class ConstructionStub final : public sim::System {
public:
    explicit ConstructionStub(std::shared_ptr<EconomySkeletonCounters> counters)
        : counters_(std::move(counters)) {}
    [[nodiscard]] std::string_view name() const override { return kEconomyConstructionName; }
    [[nodiscard]] sim::Cadence cadence() const override { return sim::Cadence::Daily; }
    void run(sim::World&, sim::TickContext&) override { ++counters_->construction_runs; }

private:
    std::shared_ptr<EconomySkeletonCounters> counters_;
};

/// Walks the provinces of the day's stagger slot (index % 28 == day - 1, days 1..28) without touching
/// them: the same iteration the real system will do, so the count is the real number of slots.
class ProvincesStub final : public sim::System {
public:
    explicit ProvincesStub(std::shared_ptr<EconomySkeletonCounters> counters)
        : counters_(std::move(counters)) {}
    [[nodiscard]] std::string_view name() const override { return kEconomyProvincesName; }
    [[nodiscard]] sim::Cadence cadence() const override { return sim::Cadence::Daily; }
    void run(sim::World& world, sim::TickContext& context) override {
        ++counters_->province_runs;
        const int day = context.date.day();
        if (day > sim::economy::kStaggerDays) {
            return;
        }
        const auto count = static_cast<std::uint32_t>(world.provinces().size());
        constexpr auto kStride = static_cast<std::uint32_t>(sim::economy::kStaggerDays);
        for (auto index = static_cast<std::uint32_t>(day - 1); index < count; index += kStride) {
            ++counters_->slots_visited;
        }
    }

private:
    std::shared_ptr<EconomySkeletonCounters> counters_;
};

}  // namespace

std::shared_ptr<EconomySkeletonCounters> install_economy_skeleton(sim::Simulation& simulation) {
    auto counters = std::make_shared<EconomySkeletonCounters>();
    simulation.add_system(std::make_unique<SettlementStub>(counters));
    simulation.add_system(std::make_unique<ConstructionStub>(counters));
    simulation.add_system(std::make_unique<ProvincesStub>(counters));
    return counters;
}

}  // namespace pat::stress
