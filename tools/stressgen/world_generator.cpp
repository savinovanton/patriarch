#include "pat/stress/world_generator.hpp"

#include <algorithm>
#include <utility>
#include <vector>

namespace pat::stress {

namespace {

constexpr std::int32_t kSkillRawMax = 20 * core::Fixed32::one_raw;
constexpr std::int32_t kScaleRawMax = 100 * core::Fixed32::one_raw;

core::Fixed32 random_fixed(core::Pcg32& rng, std::int32_t lo_raw, std::int32_t hi_raw) {
    return core::Fixed32::from_raw(rng.range(lo_raw, hi_raw));
}

// Synthetic values for the E7-T02 columns: plausible, inside the bounds, reproducible.
void fill_person(sim::CharacterInit& init,
                 core::Pcg32& rng,
                 std::uint32_t number,
                 const std::vector<sim::BranchId>& branches,
                 std::int32_t age) {
    init.name = core::StringId(1'000'000 + number);  // no interner here: opaque ids
    if (!branches.empty()) {
        init.branch = branches[rng.bounded(static_cast<std::uint32_t>(branches.size()))];
    }
    init.portrait = static_cast<std::uint16_t>(rng.bounded(64));
    init.legitimacy = rng.chance_permille(60) ? sim::Legitimacy::Illegitimate : sim::Legitimacy::Legitimate;
    for (core::Fixed32& skill : init.skills) {
        skill = random_fixed(rng, 0, kSkillRawMax);
    }
    init.fertility = random_fixed(rng, 0, kScaleRawMax);
    init.stress = random_fixed(rng, 0, kScaleRawMax / 2);
    init.piety = random_fixed(rng, 0, kScaleRawMax);
    init.ambition = random_fixed(rng, -kScaleRawMax, kScaleRawMax);
    init.loyalty_head = random_fixed(rng, -kScaleRawMax / 2, kScaleRawMax);
    init.loyalty_emperor = random_fixed(rng, -kScaleRawMax / 2, kScaleRawMax);
    const auto trait_count = rng.bounded(5);
    for (std::uint32_t t = 0; t < trait_count; ++t) {
        init.traits[t] = static_cast<std::uint16_t>(1 + rng.bounded(60));
    }
    init.traits = sim::sanitize_traits(init.traits);  // draws may repeat: sort and drop duplicates
    if (age >= 6 && age <= 16) {
        init.education.focus = static_cast<std::uint8_t>(1 + rng.bounded(sim::kEducationFocusCount));
        init.education.level = static_cast<std::uint8_t>(rng.bounded(60));
    }
    if (rng.chance_permille(100)) {  // about one in ten has a planned death somewhere ahead
        const std::int32_t earliest = (age + 1) * 365;  // always in the future of the snapshot date
        init.planned_death = init.birth_date + rng.range(earliest, std::max(earliest, 85 * 365));
    }
}

}  // namespace

core::Result<sim::World> generate_world(const WorldConfig& config) {
    using R = core::Result<sim::World>;
    constexpr std::uint32_t kMaxCount = 1'000'000;
    if (config.provinces > kMaxCount || config.characters > kMaxCount || config.dynasties > 10'000 ||
        config.archived > kMaxCount ||
        static_cast<std::uint64_t>(config.characters) + config.archived > kMaxCount) {
        return R::err({1, "counts exceed stress generator limits"});
    }
    if (config.archived > 0 && (!config.dynasty_mode || config.dynasties == 0)) {
        return R::err({1, "archived characters need dynasty mode and at least one dynasty"});
    }
    // The ownership index keeps a SparseSet of provinces per dynasty, which addresses global IDs
    // (the members of a dynasty are a plain list since E7-T04). Bound its worst-case memory before
    // allocating, including user-supplied count combinations outside the standard profiles.
    const auto index_slots = static_cast<std::uint64_t>(config.dynasties) * config.provinces;
    if (index_slots > 64'000'000) {
        return R::err({1, "requested counts exceed the ownership index memory limit"});
    }

    sim::World world(config.date, config.seed);
    if (config.dynasty_mode && !world.enable_dynasty_mode()) {
        return R::err({1, "cannot enable dynasty mode"});
    }
    auto& provinces_rng = world.rng().stream("stress.provinces");
    auto& characters_rng = world.rng().stream("stress.characters");
    auto& dynasties_rng = world.rng().stream("stress.dynasties");
    // Plausible values for the columns of E7-T02; a separate stream keeps the draws above unchanged.
    auto& persons_rng = world.rng().stream("stress.persons");
    std::vector<std::vector<sim::BranchId>> branches(config.dynasties);
    for (std::uint32_t d = 0; d < config.dynasties; ++d) {
        const auto dynasty = world.create_dynasty(core::StringId(d));
        world.add_honor(dynasty, dynasties_rng.range(0, 5000));
        world.set_dynasty_unity(dynasty, core::Fixed32::from_int(persons_rng.range(20, 90)));
        world.set_dynasty_succession_law(
            dynasty, static_cast<sim::SuccessionLaw>(persons_rng.bounded(sim::kSuccessionLawCount)));
        const auto branch_count = 1 + persons_rng.bounded(3);
        for (std::uint32_t b = 0; b < branch_count; ++b) {
            branches[d].push_back(world.create_branch(dynasty, core::StringId(config.dynasties + b)));
        }
    }
    for (std::uint32_t p = 0; p < config.provinces; ++p) {
        const sim::DynastyId owner(config.dynasties == 0 ? sim::DynastyId::kInvalid : p % config.dynasties);
        (void)world.create_province(core::StringId(config.dynasties + p),
                                    owner,
                                    core::Fixed32::from_int(provinces_rng.range(5, 250)));
    }

    const std::vector<sim::BranchId> no_branches;
    std::uint32_t person_number = 0;
    const auto groups = std::max(config.dynasties, 1U);
    std::vector<std::pair<sim::CharacterId, sim::CharacterId>> founders(groups);
    for (std::uint32_t d = 0; d < groups; ++d) {
        const auto count = config.characters / groups + (d < config.characters % groups ? 1U : 0U);
        const sim::DynastyId dynasty(config.dynasties == 0 ? sim::DynastyId::kInvalid : d);
        sim::CharacterId father;
        sim::CharacterId mother;
        for (std::uint32_t c = 0; c < count; ++c) {
            sim::CharacterInit init;
            init.dynasty = dynasty;
            const auto age = c == 0 ? 65 : (c == 1 ? 60 : characters_rng.range(0, 41));
            init.birth_date =
                sim::Date::from_ymd(config.date.year() - age, 1, 1) + characters_rng.range(0, 180);
            // Keep newborns at or before the snapshot date, even for a January snapshot.
            init.birth_date = std::min(init.birth_date, config.date);
            init.sex =
                c == 0 ? sim::Sex::Male
                       : (c == 1 || characters_rng.chance_permille(500) ? sim::Sex::Female : sim::Sex::Male);
            init.health = core::Fixed32::from_int(characters_rng.range(40, 100));
            if (c >= 2) {
                init.father = father;
                init.mother = mother;
            }
            fill_person(init, persons_rng, person_number++, dynasty.valid() ? branches[d] : no_branches, age);
            const auto id = world.create_character(init);
            if (c == 0) {
                father = id;
                if (dynasty.valid()) {
                    (void)world.set_dynasty_head(dynasty, id);
                }
            } else if (c == 1) {
                mother = id;
                (void)world.marry(father, mother);  // the founding couple (dynasty mode only)
            }
        }
        founders[d] = {father, mother};
    }

    // The dead (E7-T04): created after all the living, so that the ids 0..characters-1 are the living
    // ones (the workload refers to them), and archived at once. The stream is created only when it
    // is needed, so worlds without an archive keep their random state.
    if (config.archived > 0) {
        auto& archive_rng = world.rng().stream("stress.archive");
        for (std::uint32_t a = 0; a < config.archived; ++a) {
            const std::uint32_t d = a % groups;
            sim::CharacterInit init;
            init.dynasty = sim::DynastyId(d);
            const auto age = archive_rng.range(0, 41);
            init.birth_date = std::min(
                sim::Date::from_ymd(config.date.year() - age, 1, 1) + archive_rng.range(0, 180), config.date);
            init.sex = archive_rng.chance_permille(500) ? sim::Sex::Female : sim::Sex::Male;
            const auto [father, mother] = founders[d];
            if (father.valid() && mother.valid()) {  // children of the founding couple
                init.father = father;
                init.mother = mother;
            }
            fill_person(init, persons_rng, person_number++, branches[d], age);
            const auto id = world.create_character(init);
            const auto cause = static_cast<sim::CauseOfDeath>(
                1 + archive_rng.bounded(static_cast<std::uint32_t>(sim::kCauseOfDeathCount - 1)));
            if (!id.valid() || !world.die(id, cause)) {
                return R::err({1, "cannot archive a synthetic character"});
            }
        }
    }
    if (const auto violations = world.check_invariants(); !violations.empty()) {
        return R::err({2, violations.front()});
    }
    return R::ok(std::move(world));
}

}  // namespace pat::stress
