#include "pat/stress/economy_skeleton.hpp"

#include "pat/sim/economy/rules.hpp"
#include "pat/sim/economy/systems.hpp"
#include "pat/sim/economy/types.hpp"
#include "pat/sim/system.hpp"

#include <memory>
#include <string_view>
#include <utility>

namespace pat::stress {

namespace {

/// `economy.settlement`: counts the months and then runs the REAL system of E9-T09 (flat expenses,
/// rotation of the ring of the statement) under the same name, so `system.economy.settlement` measures it.
class SettlementStub final : public sim::System {
public:
    explicit SettlementStub(std::shared_ptr<EconomySkeletonCounters> counters)
        : counters_(std::move(counters)),
          real_(sim::economy::make_settlement_system(sim::economy::EconomyRules::defaults())) {}
    [[nodiscard]] std::string_view name() const override { return kEconomySettlementName; }
    [[nodiscard]] sim::Cadence cadence() const override { return sim::Cadence::Monthly; }
    void run(sim::World& world, sim::TickContext& context) override {
        ++counters_->settlement_runs;
        real_->run(world, context);
    }

private:
    std::shared_ptr<EconomySkeletonCounters> counters_;
    std::unique_ptr<sim::System> real_;
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

/// `economy.provinces`: counts the slots of the day (the same stagger iteration as the real system, so
/// the count is the real number of slots) and then runs the REAL system of E9-T06 (growth of the
/// population) under the same name, so the telemetry metric `system.economy.provinces` measures it.
class ProvincesStub final : public sim::System {
public:
    explicit ProvincesStub(std::shared_ptr<EconomySkeletonCounters> counters)
        : counters_(std::move(counters)),
          real_(sim::economy::make_provinces_system(sim::economy::EconomyRules::defaults())) {}
    [[nodiscard]] std::string_view name() const override { return kEconomyProvincesName; }
    [[nodiscard]] sim::Cadence cadence() const override { return sim::Cadence::Daily; }
    void run(sim::World& world, sim::TickContext& context) override {
        ++counters_->province_runs;
        const int day = context.date.day();
        if (day <= sim::economy::kStaggerDays) {
            const auto count = static_cast<std::uint32_t>(world.provinces().size());
            constexpr auto kStride = static_cast<std::uint32_t>(sim::economy::kStaggerDays);
            for (auto index = static_cast<std::uint32_t>(day - 1); index < count; index += kStride) {
                ++counters_->slots_visited;
            }
        }
        real_->run(world, context);
    }

private:
    std::shared_ptr<EconomySkeletonCounters> counters_;
    std::unique_ptr<sim::System> real_;
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
