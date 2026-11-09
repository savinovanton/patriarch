#include "pat/sim/mortality.hpp"

#include <algorithm>
#include <cstdint>

namespace pat::sim {
namespace {

constexpr std::string_view kMortalityStream = "demography.mortality";

class MortalitySystem final : public System {
public:
    explicit MortalitySystem(const DemographyRules& rules) : rules_(rules) {}

    [[nodiscard]] std::string_view name() const override { return kMortalitySystemName; }
    [[nodiscard]] Cadence cadence() const override { return Cadence::Daily; }

    void run(World& world, TickContext& context) override {
        for (const auto& [due, event] : world.take_due_life_events(context.date)) {
            context.budget.spend(1);
            if (event.kind == LifeEventKind::Birthday) {
                roll_year(world, context, event.character, due);
            } else {
                emit_death(world.characters(), context, event.character, due);
            }
        }
    }

private:
    void roll_year(World& world, TickContext& context, CharacterId id, Date birthday) const {
        auto& rng = world.rng().stream(kMortalityStream);
        const std::uint64_t roll = rng.next_u32();
        const std::uint64_t offset_roll = rng.next_u32();
        const auto& people = world.characters();
        const std::size_t index = id.index();
        const Date birth = people.birth_date[index];
        const int age = age_years(birth, birthday);
        // The model currently contains full-detail noble characters. Trait effects arrive with T08.
        const auto chance = mortality_permille(rules_, age, people.health[index], true).total;
        // Compare against Q32.32 per-mille directly. Both draws are consumed even for certainty,
        // immunity or an existing plan. Multiply-high maps the second draw onto 0..364.
        if (roll * 1000 >= static_cast<std::uint64_t>(chance.raw()) ||
            people.planned_death[index] != kNoDate) {
            return;
        }

        const auto offset = static_cast<std::int32_t>((offset_roll * 365) >> 32);
        const Date last_plan = std::min(birth + kMaxLifeSpanDays, Date::from_days(kSaneDateLimit));
        if (birthday > last_plan) {
            // Very old characters can still die even when their entire current year is outside
            // the range representable by the planned-death field.
            emit_death(people, context, id, birthday);
            return;
        }
        context.emit({DynastyId{}, cmd::PlanDeath{id, std::min(birthday + offset, last_plan)}});
    }

    void emit_death(const CharacterTable& people, TickContext& context, CharacterId id, Date due) const {
        const std::size_t index = id.index();
        CauseOfDeath cause = CauseOfDeath::Illness;
        if (age_years(people.birth_date[index], due) >= rules_.elder_from) {
            cause = CauseOfDeath::OldAge;
        } else if (people.health[index] >= core::Fixed32::from_int(rules_.high_health_from)) {
            cause = CauseOfDeath::Accident;
        }
        context.emit({DynastyId{}, cmd::Die{id, cause}});
    }

    const DemographyRules rules_;
};

}  // namespace

std::unique_ptr<System> make_mortality_system(const DemographyRules& rules) {
    return std::make_unique<MortalitySystem>(rules);
}

}  // namespace pat::sim
