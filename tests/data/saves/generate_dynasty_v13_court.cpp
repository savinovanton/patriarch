// Generate once with main 7a359af, before E10-T03 saves the court and influence tables (schema 14).
// Reproduction and the captured hashes: generate_dynasty_v13_court.md.

#include "pat/core/fixed.hpp"
#include "pat/sim/events/state.hpp"
#include "pat/sim/save.hpp"
#include "pat/sim/world.hpp"

#include <cinttypes>
#include <cstdio>
#include <filesystem>

using namespace pat::sim;
using pat::core::Fixed32;
using pat::core::Fixed64;
using pat::core::StringId;

static_assert(World::kStateSchema == 13, "This generator must use the pre-E10-T03 code");
static_assert(kSaveFormatVersion == 2);

namespace {

bool checked(bool ok, const char* operation) {
    if (!ok) {
        std::fprintf(stderr, "failed: %s\n", operation);
    }
    return ok;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: generate_dynasty_v13_court <output.psv>\n");
        return 2;
    }

    const Date today = Date::from_ymd(1703, 6, 2);
    World world(today, 1403);
    if (!checked(world.enable_dynasty_mode(), "enable dynasty mode")) {
        return 3;
    }
    CharacterId heads[3];
    for (std::uint32_t i = 0; i < 3; ++i) {
        const DynastyId house = world.create_dynasty(StringId(110 + i));
        const ProvinceId province =
            world.create_province(StringId(210 + i), house, Fixed32::from_int(500 + 50 * i));
        CharacterInit init;
        init.dynasty = house;
        init.sex = Sex::Male;
        init.birth_date = Date::from_ymd(1665 + static_cast<int>(i), 3, 4);
        init.name = StringId(310 + i);
        heads[i] = world.create_character(init);
        if (!checked(province.valid() && heads[i].valid() && world.set_dynasty_head(house, heads[i]),
                     "create province and dynasty head")) {
            return 3;
        }
    }
    // Event rows of schema 13 (E11-T03) next to the court, which schema 13 does not save.
    const auto character = [](CharacterId id) {
        return events::Scope::make(events::ScopeKind::Character, id.value());
    };
    const auto house = [](std::uint32_t id) { return events::Scope::make(events::ScopeKind::Dynasty, id); };
    events::Timer timer;
    timer.due = today + 12;
    timer.event_key = 0x5151;
    timer.scope = house(1);
    if (!checked(world.set_event_flag(character(heads[0]), 0x4141) &&
                     world.set_event_var(house(2), 0x4242, Fixed64::from_int(3)) &&
                     world.add_event_timer(timer).has_value() &&
                     world.set_event_cooldown({house(0), 0x4343, today + 30}),
                 "event rows")) {
        return 3;
    }

    const auto problems = world.check_invariants();
    for (const auto& problem : problems) {
        std::fprintf(stderr, "%s\n", problem.c_str());
    }
    if (!problems.empty()) {
        return 4;
    }
    const WorldHash original = world.compute_hash();
    SaveSummary summary = summary_from_world(world);
    summary.scenario = "schema13.court.fixture";
    summary.player_dynasty = 0;
    summary.name = "Schema 13 before E10-T03";
    const auto bytes = serialize_save(world, {}, summary);
    auto loaded = deserialize_save(bytes);
    if (!checked(loaded.has_value(), "read fixture with original schema-13 reader") ||
        !checked(loaded.value().world.compute_hash() == original, "original full world hash") ||
        !checked(loaded.value().world.event_state() == world.event_state(), "original event equality")) {
        return 4;
    }
    const auto written = write_file_atomic(std::filesystem::path(argv[1]), bytes);
    if (!checked(written.has_value(), "write fixture")) {
        return 5;
    }
    std::printf("source=7a359af schema=%u container=%u bytes=%zu\n",
                World::kStateSchema,
                static_cast<unsigned>(kSaveFormatVersion),
                bytes.size());
    std::printf("total=%" PRIu64 " calendar=%" PRIu64 " rng=%" PRIu64 " dynasties=%" PRIu64
                " provinces=%" PRIu64 " characters=%" PRIu64 "\n",
                original.total,
                original.calendar,
                original.rng,
                original.dynasties,
                original.provinces,
                original.characters);
    for (std::size_t i = 0; i < original.ext.size(); ++i) {
        if (original.ext[i] != 0) {
            std::printf("ext[%zu]=%" PRIu64 "\n", i, original.ext[i]);
        }
    }
    return 0;
}
