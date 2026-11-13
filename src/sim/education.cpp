#include "pat/sim/education.hpp"

#include <algorithm>
#include <utility>

namespace pat::sim {
namespace {

using core::Fixed32;

class EducationSystem final : public System {
public:
    explicit EducationSystem(EducationContent content)
        : rules_(content.rules), traits_(std::move(content.traits)),
          player_dynasties_(std::move(content.player_dynasties)) {
        std::sort(player_dynasties_.begin(), player_dynasties_.end());
    }

    [[nodiscard]] std::string_view name() const override { return kEducationSystemName; }
    [[nodiscard]] Cadence cadence() const override { return Cadence::Daily; }

    void run(World& world, TickContext& context) override {
        if (!world.dynasty_mode()) {
            return;
        }
        for (const DueAdulthood& due : world.take_due_adulthoods(context.date)) {
            context.budget.spend(1);
            emit_come_of_age(world, context, due.character);
        }
        if (context.date.day() > 28) {
            return;  // days 29...31 select nobody (calendar.hpp)
        }
        for (const CharacterId id : world.living()) {
            if (is_monthly_slot(id.value(), context.date)) {
                monthly_step(world, context, id);
            }
        }
    }

private:
    [[nodiscard]] Fixed32 skill_of(const World& world, CharacterId id, Skill skill) const {
        return traits_ ? skill_breakdown(world, *traits_, id, skill).total
                       : world.characters().skills[id.index()][static_cast<std::size_t>(skill)];
    }

    /// The effective skill of the tutor in `skill`; 0 for no tutor, a dead one or one who is not of age.
    [[nodiscard]] Fixed32 tutor_skill(const World& world, CharacterId tutor, Skill skill, Date today) const {
        if (!tutor.valid() || !world.is_alive(tutor) ||
            age_years(world.characters().birth_date[tutor.index()], today) < kComingOfAgeYears) {
            return Fixed32::zero();
        }
        return skill_of(world, tutor, skill);
    }

    [[nodiscard]] bool is_player(DynastyId dynasty) const {
        return std::binary_search(player_dynasties_.begin(), player_dynasties_.end(), dynasty);
    }

    // ---- the monthly step ---------------------------------------------------------------------------

    void monthly_step(World& world, TickContext& context, CharacterId id) const {
        const CharacterTable& people = world.characters();
        const std::size_t index = id.index();
        const int age = age_years(people.birth_date[index], context.date);
        if (age >= kComingOfAgeYears || age < rules_.adolescent_from) {
            return;
        }
        context.budget.spend(1);
        const std::uint8_t focus = people.education_focus[index];
        if (focus == 0) {
            const DynastyId dynasty = people.dynasty[index];
            if (dynasty.valid() && !is_player(dynasty)) {
                choose_for_ai(world, context, id, dynasty);
            }
            return;
        }
        CharacterId tutor = people.education_tutor[index];
        if (tutor.valid() && !world.is_alive(tutor)) {
            tutor = CharacterId();  // the tutor died: the lessons go on without him
        }
        const int level = people.education_level[index];
        const int gain = education_monthly_gain(
            rules_, tutor_skill(world, tutor, static_cast<Skill>(focus - 1), context.date));
        (void)world.set_character_education(
            id,
            Education{
                focus, tutor, static_cast<std::uint8_t>(std::min<int>(level + gain, kMaxEducationLevel))});
    }

    /// The automatic choice: the best of the five skills of the child (the first on a tie) and, as the
    /// tutor, the best adult of the house in that skill (the smallest id on a tie), if there is one.
    void choose_for_ai(World& world, TickContext& context, CharacterId child, DynastyId dynasty) const {
        std::size_t best_skill = 0;
        Fixed32 best_value = skill_of(world, child, Skill::Stewardship);
        for (std::size_t s = 1; s < kSkillCount; ++s) {
            const Fixed32 value = skill_of(world, child, static_cast<Skill>(s));
            if (value > best_value) {
                best_value = value;
                best_skill = s;
            }
        }
        const auto skill = static_cast<Skill>(best_skill);
        CharacterId tutor;
        Fixed32 tutor_value = Fixed32::zero();
        for (const CharacterId member : world.members_of(dynasty)) {
            if (member == child ||
                age_years(world.characters().birth_date[member.index()], context.date) < kComingOfAgeYears) {
                continue;
            }
            const Fixed32 value = skill_of(world, member, skill);
            if (value > tutor_value || (value == tutor_value && tutor.valid() && member < tutor)) {
                tutor = member;
                tutor_value = value;
            }
        }
        context.emit(
            {DynastyId{}, cmd::SetEducation{child, static_cast<std::uint8_t>(best_skill + 1), tutor}});
    }

    // ---- coming of age ------------------------------------------------------------------------------

    void emit_come_of_age(const World& world, TickContext& context, CharacterId id) const {
        const CharacterTable& people = world.characters();
        const std::size_t index = id.index();
        const std::uint8_t focus = people.education_focus[index];
        std::uint16_t trait = 0;
        Fixed32 bonus = Fixed32::zero();
        if (focus != 0) {
            const auto skill = static_cast<Skill>(focus - 1);
            const EducationOutcome outcome =
                education_outcome(rules_,
                                  people.education_level[index],
                                  tutor_skill(world, people.education_tutor[index], skill, context.date));
            bonus = outcome.bonus;
            if (outcome.earns_trait && traits_) {
                const std::uint16_t candidate = education_trait_for(*traits_, skill);
                if (candidate != 0 && check_grant_trait(world, *traits_, id, candidate) == TraitStatus::Ok) {
                    trait = candidate;
                }
            }
        }
        context.emit({DynastyId{}, cmd::ComeOfAge{id, trait, bonus}});
    }

    const DemographyRules rules_;
    const std::shared_ptr<const TraitCatalog> traits_;
    std::vector<DynastyId> player_dynasties_;
};

}  // namespace

std::uint16_t education_trait_for(const TraitCatalog& catalog, Skill skill) noexcept {
    const auto index = static_cast<std::size_t>(skill);
    std::uint16_t best = 0;
    Fixed32 best_modifier = Fixed32::zero();
    for (const TraitDef& def : catalog.definitions) {
        if (def.category == TraitCategory::Education && index < kSkillCount &&
            def.skill_modifiers[index] > best_modifier) {
            best = def.id;
            best_modifier = def.skill_modifiers[index];
        }
    }
    return best;
}

std::unique_ptr<System> make_education_system(EducationContent content) {
    return std::make_unique<EducationSystem>(std::move(content));
}

}  // namespace pat::sim
