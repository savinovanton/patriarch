#include "pat/sim/health.hpp"

#include <utility>

namespace pat::sim {
namespace {

using core::Fixed32;

class HealthSystem final : public System {
public:
    HealthSystem(const DemographyRules& rules, std::shared_ptr<const TraitCatalog> traits)
        : rules_(rules), traits_(std::move(traits)) {}

    [[nodiscard]] std::string_view name() const override { return kHealthSystemName; }
    [[nodiscard]] Cadence cadence() const override { return Cadence::Daily; }

    void run(World& world, TickContext& context) override {
        if (!world.dynasty_mode() || context.date.day() > 28) {
            return;  // days 29...31 select nobody (calendar.hpp)
        }
        for (const CharacterId id : world.living()) {
            if (is_monthly_slot(id.value(), context.date)) {
                context.budget.spend(1);
                step(world, context.date, id);
            }
        }
    }

private:
    [[nodiscard]] Fixed32 effective(const World& world, CharacterId id, CharacterAttribute attribute) const {
        if (traits_) {
            return attribute_breakdown(world, *traits_, id, attribute).total;
        }
        const CharacterTable& people = world.characters();
        return attribute == CharacterAttribute::Health ? people.health[id.index()]
                                                       : people.stress[id.index()];
    }

    void step(World& world, Date date, CharacterId id) const {
        const CharacterTable& people = world.characters();
        const std::size_t index = id.index();
        const std::int32_t age = age_years(people.birth_date[index], date);

        // Exactly one draw for every character, whatever the rules say.
        const std::uint32_t roll = world.rng().stream(kHealthStream).next_u32();

        const Fixed32 health_before = effective(world, id, CharacterAttribute::Health);
        const Fixed32 stress_before = effective(world, id, CharacterAttribute::Stress);
        const DynastyId dynasty = people.dynasty[index];
        const bool head = dynasty.valid() && world.dynasties().head[dynasty.index()] == id;

        const VitalsChange health = monthly_health_change(rules_, age, health_before, stress_before, roll);
        const VitalsChange stress = monthly_stress_change(rules_, people.stress[index], health_before, head);
        world.set_character_health(id, people.health[index] + health.total);
        world.set_character_stress(id, people.stress[index] + stress.total);

        // Fertility follows the health of the moment after the step.
        world.set_character_fertility(
            id, derived_fertility(rules_, age, effective(world, id, CharacterAttribute::Health)));

        const StressLevel level_before = stress_level(rules_, stress_before);
        const StressLevel level_after =
            stress_level(rules_, effective(world, id, CharacterAttribute::Stress));
        if (level_after > level_before && dynasty.valid()) {
            (void)world.record_chronicle_note(dynasty,
                                              id,
                                              CharacterId(),
                                              level_after == StressLevel::Breaking ? kNoteStressBreaking
                                                                                   : kNoteStressStrained);
        }
    }

    const DemographyRules rules_;
    const std::shared_ptr<const TraitCatalog> traits_;
};

}  // namespace

std::unique_ptr<System> make_health_system(const DemographyRules& rules,
                                           std::shared_ptr<const TraitCatalog> traits) {
    return std::make_unique<HealthSystem>(rules, std::move(traits));
}

}  // namespace pat::sim
