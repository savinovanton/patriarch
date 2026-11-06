#include "pat/stress/world_generator.hpp"

#include <algorithm>
#include <utility>

namespace pat::stress {

core::Result<sim::World> generate_world(const WorldConfig& config) {
    using R = core::Result<sim::World>;
    constexpr std::uint32_t kMaxCount = 1'000'000;
    if (config.provinces > kMaxCount || config.characters > kMaxCount || config.dynasties > 10'000) {
        return R::err({1, "counts exceed stress generator limits"});
    }
    // World's per-dynasty SparseSets address global IDs. Bound their worst-case memory before
    // allocating, including user-supplied count combinations outside the standard profiles.
    const auto index_slots = static_cast<std::uint64_t>(config.dynasties) *
                             (static_cast<std::uint64_t>(config.characters) + config.provinces);
    if (index_slots > 64'000'000) {
        return R::err({1, "requested counts exceed the membership index memory limit"});
    }

    sim::World world(config.date, config.seed);
    auto& provinces_rng = world.rng().stream("stress.provinces");
    auto& characters_rng = world.rng().stream("stress.characters");
    auto& dynasties_rng = world.rng().stream("stress.dynasties");
    for (std::uint32_t d = 0; d < config.dynasties; ++d) {
        const auto dynasty = world.create_dynasty(core::StringId(d));
        world.add_honor(dynasty, dynasties_rng.range(0, 5000));
    }
    for (std::uint32_t p = 0; p < config.provinces; ++p) {
        const sim::DynastyId owner(config.dynasties == 0 ? sim::DynastyId::kInvalid : p % config.dynasties);
        (void)world.create_province(core::StringId(config.dynasties + p),
                                    owner,
                                    core::Fixed32::from_int(provinces_rng.range(5, 250)));
    }

    const auto groups = std::max(config.dynasties, 1U);
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
            const auto id = world.create_character(init);
            if (c == 0) {
                father = id;
                if (dynasty.valid()) {
                    (void)world.set_dynasty_head(dynasty, id);
                }
            } else if (c == 1) {
                mother = id;
            }
        }
    }
    if (const auto violations = world.check_invariants(); !violations.empty()) {
        return R::err({2, violations.front()});
    }
    return R::ok(std::move(world));
}

}  // namespace pat::stress
