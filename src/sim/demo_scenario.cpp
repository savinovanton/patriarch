#include "pat/sim/demo_scenario.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>

namespace pat::sim::demo {

namespace {

using core::Fixed32;

constexpr Fixed32 kDailyDecay = Fixed32::from_raw(Fixed32::one_raw / 64);

/// Monthly: dynasties with few living members tend to have children.
class BirthsSystem final : public System {
public:
    [[nodiscard]] std::string_view name() const override { return "demo.births"; }
    [[nodiscard]] Cadence cadence() const override { return Cadence::Monthly; }
    void run(World& world, TickContext& context) override {
        core::Pcg32& rng = world.rng().stream("demo.births");
        for (std::uint32_t d = 0; d < world.dynasties().size(); ++d) {
            const DynastyId dynasty(d);
            const std::size_t members = world.members_of(dynasty).size();
            const std::int32_t chance = members < 12 ? 400 : 50;  // per mille per month
            context.budget.spend(1);
            if (!rng.chance_permille(chance)) {
                continue;
            }
            cmd::CreateCharacter child;
            child.dynasty = dynasty;
            child.birth_date = context.date;
            child.sex = rng.chance_permille(500) ? Sex::Male : Sex::Female;
            context.emit(Command{DynastyId(), child});
        }
    }
};

/// Daily: everybody loses health; characters at zero health die (via a command).
class AgeingSystem final : public System {
public:
    [[nodiscard]] std::string_view name() const override { return "demo.ageing"; }
    [[nodiscard]] Cadence cadence() const override { return Cadence::Daily; }
    void run(World& world, TickContext& context) override {
        auto health = world.character_health_mut();
        world.characters().ids.for_each_alive([&](CharacterId id) {
            Fixed32& h = health[id.index()];
            h = h - kDailyDecay;
            if (h.raw() <= 0) {
                context.emit(Command{DynastyId(), cmd::KillCharacter{id}});
            }
            context.budget.spend(1);
        });
    }
};

/// Daily, but each province is visited only on its monthly stagger slot.
class GrowthSystem final : public System {
public:
    [[nodiscard]] std::string_view name() const override { return "demo.growth"; }
    [[nodiscard]] Cadence cadence() const override { return Cadence::Daily; }
    void run(World& world, TickContext& context) override {
        core::Pcg32& rng = world.rng().stream("demo.growth");
        auto population = world.province_population_mut();
        for (std::uint32_t id = 0; id < population.size(); ++id) {
            if (!is_monthly_slot(id, context.date)) {
                continue;
            }
            const std::int32_t permille = rng.range(-3, 8);
            Fixed32 next = population[id] + population[id].mul_permille(permille);
            if (next < Fixed32::from_int(1)) {
                next = Fixed32::from_int(1);
            }
            population[id] = next;
            context.budget.spend(2);
        }
    }
};

/// Monthly: a dynasty without a head promotes its oldest living member.
class SuccessionSystem final : public System {
public:
    [[nodiscard]] std::string_view name() const override { return "demo.succession"; }
    [[nodiscard]] Cadence cadence() const override { return Cadence::Monthly; }
    void run(World& world, TickContext& context) override {
        for (std::uint32_t d = 0; d < world.dynasties().size(); ++d) {
            const DynastyId dynasty(d);
            if (world.dynasties().head[d].valid()) {
                continue;
            }
            CharacterId oldest;
            for (const CharacterId member : world.members_of(dynasty)) {
                const Date born = world.characters().birth_date[member.index()];
                if (!oldest.valid() || born < world.characters().birth_date[oldest.index()] ||
                    (born == world.characters().birth_date[oldest.index()] && member < oldest)) {
                    oldest = member;
                }
                context.budget.spend(1);
            }
            if (oldest.valid()) {
                context.emit(Command{dynasty, cmd::SetDynastyHead{dynasty, oldest}});
            }
        }
    }
};

/// Quarterly: the system rewards dynasties for the land they hold.
class RewardsSystem final : public System {
public:
    [[nodiscard]] std::string_view name() const override { return "demo.rewards"; }
    [[nodiscard]] Cadence cadence() const override { return Cadence::Quarterly; }
    void run(World& world, TickContext& context) override {
        for (std::uint32_t d = 0; d < world.dynasties().size(); ++d) {
            const auto owned = static_cast<std::int32_t>(world.provinces_of(DynastyId(d)).size());
            context.emit(Command{DynastyId(), cmd::AdjustHonor{DynastyId(d), owned * 2}});
            context.budget.spend(1);
        }
    }
};

}  // namespace

World make_world(const Config& config) {
    World world(Date::from_ymd(1703, 1, 1), config.seed);
    core::Pcg32& rng = world.rng().stream("demo.setup");

    for (std::uint32_t d = 0; d < config.dynasties; ++d) {
        (void)world.create_dynasty(core::StringId(d));
    }
    for (std::uint32_t p = 0; p < config.provinces; ++p) {
        const DynastyId owner(config.dynasties == 0 ? DynastyId::kInvalid : p % config.dynasties);
        (void)world.create_province(core::StringId(1000 + p), owner, Fixed32::from_int(rng.range(5, 60)));
    }
    for (std::uint32_t d = 0; d < config.dynasties; ++d) {
        for (std::uint32_t c = 0; c < config.characters_per_dynasty; ++c) {
            CharacterInit init;
            init.dynasty = DynastyId(d);
            init.birth_date = Date::from_ymd(1650, 1, 1) + rng.range(0, 40 * 365);
            init.sex = (c % 2 == 0) ? Sex::Male : Sex::Female;
            init.health = Fixed32::from_int(rng.range(40, 100));
            const CharacterId id = world.create_character(init);
            if (c == 0) {
                (void)world.set_dynasty_head(DynastyId(d), id);
            }
        }
    }
    return world;
}

void install_systems(Simulation& simulation) {
    simulation.add_system(std::make_unique<BirthsSystem>());
    simulation.add_system(std::make_unique<AgeingSystem>());
    simulation.add_system(std::make_unique<GrowthSystem>());
    simulation.add_system(std::make_unique<SuccessionSystem>());
    simulation.add_system(std::make_unique<RewardsSystem>());
}

Simulation make_simulation(const Config& config, SimulationConfig simulation_config) {
    Simulation simulation(make_world(config), simulation_config);
    install_systems(simulation);
    return simulation;
}

}  // namespace pat::sim::demo
