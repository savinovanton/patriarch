#include "pat/sim/conception.hpp"

#include "pat/sim/kinship.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <utility>

namespace pat::sim {
namespace {

using core::Fixed32;

constexpr std::int32_t kPermilleOne = 1000;

/// `roll` as a per-mille bucket 0...999 (multiply-high, no rejection).
[[nodiscard]] std::uint64_t permille_bucket(std::uint32_t roll) noexcept {
    return (static_cast<std::uint64_t>(roll) * kPermilleOne) >> 32;
}

[[nodiscard]] bool has_trait(const std::array<std::uint16_t, kTraitSlots>& traits,
                             std::uint16_t id) noexcept {
    return std::find(traits.begin(), traits.end(), id) != traits.end();
}

class ConceptionSystem final : public System {
public:
    explicit ConceptionSystem(BirthContent content)
        : rules_(content.rules), traits_(std::move(content.traits)) {
        if (content.names && content.given_name_ids.size() == content.names->given_names.size()) {
            names_ = std::move(content.names);
            for (std::size_t i = 0; i < content.given_name_ids.size(); ++i) {
                if (content.given_name_ids[i].valid()) {
                    name_by_id_.emplace_back(content.given_name_ids[i].value(), i);
                }
            }
            std::sort(name_by_id_.begin(), name_by_id_.end());
            given_name_ids_ = std::move(content.given_name_ids);
        }
    }

    [[nodiscard]] std::string_view name() const override { return kConceptionSystemName; }
    [[nodiscard]] Cadence cadence() const override { return Cadence::Daily; }

    void run(World& world, TickContext& context) override {
        for (const DueLabour& labour : world.take_due_labours(context.date)) {
            context.budget.spend(1);
            emit_birth(world, context, labour);
        }
        if (context.date.day() > 28) {
            return;  // days 29...31 select nobody (calendar.hpp)
        }
        for (const CharacterId id : world.living()) {
            if (is_monthly_slot(id.value(), context.date)) {
                consider_conception(world, context, id);
            }
        }
    }

private:
    // ---- conception -------------------------------------------------------------------------------

    void consider_conception(World& world, TickContext& context, CharacterId wife) const {
        const CharacterTable& people = world.characters();
        const std::size_t index = wife.index();
        if (people.sex[index] != Sex::Female) {
            return;
        }
        const CharacterId husband = people.spouse[index];
        if (!husband.valid() || !world.is_alive(husband) || people.sex[husband.index()] != Sex::Male ||
            world.is_pregnant(wife)) {
            return;
        }
        context.budget.spend(1);

        // Exactly three draws for every eligible wife, whatever the chance is.
        auto& rng = world.rng().stream(kConceptionStream);
        const std::uint32_t conception_roll = rng.next_u32();
        const std::uint32_t loss_roll = rng.next_u32();
        const std::uint32_t timing_roll = rng.next_u32();

        ConceptionInput input;
        input.wife_age = age_years(people.birth_date[index], context.date);
        input.wife_health = effective_health(world, wife);
        input.trait_permille = couple_trait_multiplier(people, wife, husband);
        input.together = true;  // there is no residence yet: spouses live together
        input.children_born = static_cast<std::int32_t>(kin::child_count(world, wife));
        input.living_count = static_cast<std::int32_t>(world.living().size());
        const auto chance = conception_permille(rules_, input).total;
        // Compare against Q32.32 per-mille directly (see mortality.cpp).
        if (static_cast<std::uint64_t>(conception_roll) * kPermilleOne >=
            static_cast<std::uint64_t>(chance.raw())) {
            return;
        }

        const bool miscarriage =
            permille_bucket(loss_roll) < static_cast<std::uint64_t>(rules_.miscarriage_permille);
        int months = rules_.pregnancy_months;
        if (miscarriage) {
            const std::uint64_t span = static_cast<std::uint64_t>(std::max(1, rules_.pregnancy_months - 1));
            months = 1 + static_cast<int>((static_cast<std::uint64_t>(timing_roll) * span) >> 32);
        }
        context.emit({DynastyId{},
                      cmd::Conceive{wife,
                                    husband,
                                    context.date,
                                    date_plus_months(context.date, months),
                                    miscarriage ? PregnancyEnd::Miscarriage : PregnancyEnd::AtTerm,
                                    Legitimacy::Legitimate}});
    }

    [[nodiscard]] std::int32_t
    couple_trait_multiplier(const CharacterTable& people, CharacterId wife, CharacterId husband) const {
        if (!traits_) {
            return kPermilleOne;
        }
        const std::int64_t product =
            static_cast<std::int64_t>(trait_fertility_multiplier(*traits_, people.traits[wife.index()])) *
            trait_fertility_multiplier(*traits_, people.traits[husband.index()]) / kPermilleOne;
        return static_cast<std::int32_t>(
            std::min<std::int64_t>(product, DemographyRules::kMaxMultiplierPermille));
    }

    [[nodiscard]] Fixed32 effective_health(const World& world, CharacterId id) const {
        return traits_ ? attribute_breakdown(world, *traits_, id, CharacterAttribute::Health).total
                       : world.characters().health[id.index()];
    }

    // ---- labour -----------------------------------------------------------------------------------

    void emit_birth(World& world, TickContext& context, const DueLabour& labour) const {
        const auto pregnancy = world.pregnancy_of(labour.mother);
        if (!pregnancy) {
            return;  // cannot happen: take_due_labours drops stale entries
        }
        const CharacterTable& people = world.characters();
        const CharacterId mother = pregnancy->mother;
        const CharacterId father = pregnancy->father;

        auto& birth_rng = world.rng().stream(kBirthStream);
        const std::uint32_t sex_roll = birth_rng.next_u32();
        const std::uint32_t stillbirth_roll = birth_rng.next_u32();
        const std::uint32_t risk_roll = birth_rng.next_u32();

        BirthOutcome outcome = BirthOutcome::Live;
        if (pregnancy->end == PregnancyEnd::Miscarriage) {
            outcome = BirthOutcome::Miscarriage;
        } else if (permille_bucket(stillbirth_roll) <
                   static_cast<std::uint64_t>(rules_.stillbirth_permille)) {
            outcome = BirthOutcome::Stillborn;
        }
        const auto risk = childbirth_risk_permille(rules_, effective_health(world, mother), false).total;
        const bool mother_dies =
            outcome != BirthOutcome::Miscarriage &&
            static_cast<std::uint64_t>(risk_roll) * kPermilleOne < static_cast<std::uint64_t>(risk.raw());

        // Genetics and names are drawn for every labour (fixed draw counts) and used for a live birth.
        CharacterInit child;
        child.sex = permille_bucket(sex_roll) < static_cast<std::uint64_t>(male_birth_permille(rules_))
                        ? Sex::Male
                        : Sex::Female;
        const bool father_alive = world.is_alive(father);
        auto& genetics_rng = world.rng().stream(kGeneticsStream);
        child.skills = inherit_skills(people, mother, father_alive ? father : CharacterId(), genetics_rng);
        child.traits = inherit_traits(people, mother, father_alive ? father : CharacterId(), genetics_rng);
        if (names_) {
            auto& names_rng = world.rng().stream(kNamesStream);
            const std::size_t name_index =
                choose_given_name(*names_, names_rng, naming_context(world, mother, father, child.sex));
            child.portrait = choose_portrait(*names_, names_rng, child.sex, LifeStage::Child);
            if (name_index != kNoName) {
                child.name = given_name_ids_[name_index];
            }
        }
        child.dynasty = people.dynasty[father.index()].valid() ? people.dynasty[father.index()]
                                                               : people.dynasty[mother.index()];
        child.branch = kin::inherited_branch(world, father, mother, child.dynasty);
        child.legitimacy = pregnancy->legitimacy;
        child.health = Fixed32::from_int(rules_.newborn_health);
        child.father = father;
        child.mother = mother;
        if (outcome != BirthOutcome::Live) {
            child = CharacterInit{};
        }
        context.emit({DynastyId{}, cmd::Birth{mother, outcome, child}});
        if (mother_dies) {
            // After the Birth, in the same batch: a death like any other (the contract of cmd::Die).
            context.emit({DynastyId{}, cmd::Die{mother, CauseOfDeath::Childbirth}});
        }
    }

    /// 0.3 x the average of the skills of the living parents, plus a spread (GDD 01 section 3.4). The
    /// dead keep no skills (the archive resets them), so a father who died during the pregnancy does not
    /// count. One draw per skill.
    [[nodiscard]] std::array<Fixed32, kSkillCount> inherit_skills(const CharacterTable& people,
                                                                  CharacterId mother,
                                                                  CharacterId father,
                                                                  core::Pcg32& rng) const {
        std::array<Fixed32, kSkillCount> skills{};
        const std::int64_t parents = father.valid() ? 2 : 1;
        const std::int64_t spread_raw = static_cast<std::int64_t>(rules_.skill_spread) * Fixed32::one_raw;
        for (std::size_t s = 0; s < kSkillCount; ++s) {
            const std::uint32_t draw = rng.next_u32();
            std::int64_t sum = people.skills[mother.index()][s].raw();
            if (father.valid()) {
                sum += people.skills[father.index()][s].raw();
            }
            const std::int64_t inherited = sum / parents * rules_.skill_inherit_permille / kPermilleOne;
            // Uniform in [-spread, +spread) points, at the resolution of a Fixed32.
            const std::int64_t spread =
                static_cast<std::int64_t>(
                    (static_cast<std::uint64_t>(draw) * static_cast<std::uint64_t>(2 * spread_raw)) >> 32) -
                spread_raw;
            skills[s] = std::clamp(
                Fixed32::from_raw(static_cast<std::int32_t>(inherited + spread)), Fixed32::zero(), kSkillMax);
        }
        return skills;
    }

    /// Every heritable trait of the catalogue (inherit_chance > 0, ascending id) is passed on with the
    /// chance of its definition for each parent who has it (two parents: 1 - (1 - c)^2), as long as the
    /// set stays legal (slots, opposites, at most three congenital traits). One draw per heritable trait.
    [[nodiscard]] std::array<std::uint16_t, kTraitSlots> inherit_traits(const CharacterTable& people,
                                                                        CharacterId mother,
                                                                        CharacterId father,
                                                                        core::Pcg32& rng) const {
        std::array<std::uint16_t, kTraitSlots> result{};
        if (!traits_) {
            return result;
        }
        std::size_t count = 0;
        for (const TraitDef& def : traits_->definitions) {
            if (def.inherit_chance <= Fixed32::zero()) {
                continue;
            }
            const std::uint32_t draw = rng.next_u32();
            const int carriers = (has_trait(people.traits[mother.index()], def.id) ? 1 : 0) +
                                 (father.valid() && has_trait(people.traits[father.index()], def.id) ? 1 : 0);
            if (carriers == 0 || count == kTraitSlots) {
                continue;
            }
            const Fixed32 chance = carriers == 1 ? def.inherit_chance
                                                 : def.inherit_chance + def.inherit_chance -
                                                       def.inherit_chance * def.inherit_chance;
            if ((draw >> 16) >= static_cast<std::uint32_t>(std::max(chance.raw(), 0))) {
                continue;
            }
            auto trial = result;
            trial[count] = def.id;
            if (check_trait_set(*traits_, trial) == TraitStatus::Ok) {
                result = trial;
                ++count;
            }
        }
        return sanitize_traits(result);
    }

    // ---- names ------------------------------------------------------------------------------------

    /// Index into the pool of the given name stored in a character's `name` column; kNoName if none.
    [[nodiscard]] std::size_t name_index_of(const CharacterTable& people, CharacterId id) const {
        if (!id.valid()) {
            return kNoName;
        }
        const std::uint32_t key = people.name[id.index()].value();
        const auto it = std::lower_bound(
            name_by_id_.begin(), name_by_id_.end(), std::pair<std::uint32_t, std::size_t>{key, 0});
        return it != name_by_id_.end() && it->first == key ? it->second : kNoName;
    }

    [[nodiscard]] std::string_view name_id_of(const CharacterTable& people, CharacterId id) const {
        const std::size_t index = name_index_of(people, id);
        return index == kNoName ? std::string_view() : std::string_view(names_->given_names[index].id);
    }

    /// The line of the family that gives a name: father and his father for a son, mother and her mother
    /// for a daughter; living brothers (or sisters) of the same sex, half-siblings included.
    [[nodiscard]] NamingContext
    naming_context(const World& world, CharacterId mother, CharacterId father, Sex sex) const {
        const CharacterTable& people = world.characters();
        NamingContext context(sex);
        const CharacterId parent = sex == Sex::Male ? father : mother;
        const CharacterId grandparent = parent.valid() ? (sex == Sex::Male ? people.father[parent.index()]
                                                                           : people.mother[parent.index()])
                                                       : CharacterId();
        context.parent_name = name_id_of(people, parent);
        context.grandparent_name = name_id_of(people, grandparent);
        siblings_.clear();
        for (const CharacterId parent_of_siblings : {mother, father}) {
            for (const CharacterId sibling : kin::children_of(world, parent_of_siblings)) {
                if (world.is_alive(sibling) && people.sex[sibling.index()] == sex) {
                    const std::string_view id = name_id_of(people, sibling);
                    if (!id.empty()) {
                        siblings_.push_back(id);
                    }
                }
            }
        }
        context.living_siblings = siblings_;
        return context;
    }

    const DemographyRules rules_;
    const std::shared_ptr<const TraitCatalog> traits_;
    std::shared_ptr<const NamePools> names_;
    std::vector<core::StringId> given_name_ids_;
    std::vector<std::pair<std::uint32_t, std::size_t>> name_by_id_;  // (StringId, pool index), ascending
    mutable std::vector<std::string_view> siblings_;                 // scratch for naming_context
};

}  // namespace

std::vector<core::StringId> intern_given_names(const NamePools& pools, core::StringInterner& interner) {
    std::vector<core::StringId> ids;
    ids.reserve(pools.given_names.size());
    for (const GivenName& name : pools.given_names) {
        ids.push_back(interner.intern(name.id));
    }
    return ids;
}

Date date_plus_months(Date date, int months) noexcept {
    const YearMonthDay ymd = date.ymd();
    const int index = ymd.year * 12 + (ymd.month - 1) + months;
    const int year = index / 12;
    const int month = index % 12 + 1;
    return Date::from_ymd(year, month, std::min(ymd.day, Date::days_in_month(year, month)));
}

std::unique_ptr<System> make_conception_system(BirthContent content) {
    return std::make_unique<ConceptionSystem>(std::move(content));
}

}  // namespace pat::sim
