#include "pat/core/bytes.hpp"
#include "pat/core/rng.hpp"
#include "pat/sim/calendar.hpp"
#include "pat/sim/command.hpp"
#include "pat/sim/command_log.hpp"
#include "pat/sim/economy/invariants.hpp"
#include "pat/sim/economy/policy.hpp"
#include "pat/sim/economy/rules.hpp"
#include "pat/sim/economy/systems.hpp"
#include "pat/sim/economy/tables.hpp"
#include "pat/sim/economy/taxes.hpp"
#include "pat/sim/replay.hpp"
#include "pat/sim/simulation.hpp"
#include "pat/sim/world.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

// E9-T12: the policies of a dynasty (GDD 03 section 9) and the commands SetTaxPolicy (tag 32) and
// SetRecruitPolicy (tag 33): validation, the log, replays, the level a province is processed at, the
// effect in the monthly pass, saves (section 40, schema 9) and the migration of schema 8.

using namespace pat::sim;
using namespace pat::sim::economy;
using pat::core::Fixed32;
using pat::core::StringId;

namespace {

const EconomyRules& rules() {
    static const EconomyRules value = EconomyRules::defaults();
    return value;
}

void require_valid(const World& world) {
    const auto problems = world.check_invariants();
    for (const auto& problem : problems) {
        MESSAGE(problem);
    }
    REQUIRE(problems.empty());
}

/// Three dynasties and `provinces` provinces: owned by dynasty 0, except the last one that is the
/// crown's; economy on, the levels of the rules known to the world; peasants and townsfolk set.
World make_world(std::uint32_t provinces, Date start = Date::from_ymd(1702, 12, 31), bool bind = true) {
    World world(start, 5);
    for (std::uint32_t d = 0; d < 3; ++d) {
        (void)world.create_dynasty(StringId(1 + d));
    }
    for (std::uint32_t p = 0; p < provinces; ++p) {
        (void)world.create_province(StringId(100 + p), DynastyId(0), Fixed32::from_int(1));
    }
    REQUIRE(world.enable_economy());
    if (bind) {
        bind_policy_levels(world, rules());
    }
    for (std::uint32_t p = 0; p < provinces; ++p) {
        (void)world.set_group_size(ProvinceId(p), Group::Peasants, 8000);
        (void)world.set_group_size(ProvinceId(p), Group::Townsfolk, 1500);
    }
    require_valid(world);
    return world;
}

Simulation economy_sim(World world) {
    Simulation sim(std::move(world));
    install_economy_systems(sim, rules());
    return sim;
}

Command tax(DynastyId actor, DynastyId dynasty, std::uint8_t level) {
    return Command{actor, cmd::SetTaxPolicy{dynasty, level}};
}
Command recruit(DynastyId actor, DynastyId dynasty, std::uint8_t level) {
    return Command{actor, cmd::SetRecruitPolicy{dynasty, level}};
}

Rejection verdict(const World& world, const Command& command) {
    const auto result = validate(world, command);
    return result.has_value() ? Rejection::None : result.error().reason;
}

}  // namespace

// ---- tags and the log -------------------------------------------------------------------------------

TEST_CASE("policy commands: tags 32 and 33, names, the log round-trips, old tags stay") {
    CHECK(static_cast<std::uint8_t>(type_of(tax(DynastyId(0), DynastyId(0), 2))) == 32);
    CHECK(static_cast<std::uint8_t>(type_of(recruit(DynastyId(0), DynastyId(0), 2))) == 33);
    CHECK(to_string(CommandType::SetTaxPolicy) == "SetTaxPolicy");
    CHECK(to_string(CommandType::SetRecruitPolicy) == "SetRecruitPolicy");
    // the tags that were released before stay where they are (they are in replays and saves)
    CHECK(static_cast<std::uint8_t>(CommandType::SetProvinceOwner) == 1);
    CHECK(static_cast<std::uint8_t>(CommandType::AdjustHonor) == 5);
    CHECK(static_cast<std::uint8_t>(CommandType::AssignProvinceAdministrator) == 35);

    CommandLog log;
    LoggedCommand entry;
    entry.date = Date::from_ymd(1703, 3, 4);
    entry.source = CommandSource::External;
    entry.command = tax(DynastyId(1), DynastyId(1), 3);
    log.append(entry);
    entry.command = recruit(DynastyId(), DynastyId(2), 0);
    entry.outcome = Rejection::InvalidState;
    log.append(entry);
    entry.command = Command{DynastyId(), cmd::AdjustHonor{DynastyId(1), 5}};
    entry.outcome = Rejection::None;
    log.append(entry);
    const auto bytes = log.serialize();
    const auto back = CommandLog::deserialize(bytes);
    REQUIRE(back.has_value());
    REQUIRE(back.value().entries().size() == 3);
    CHECK(back.value().entries()[0].command == tax(DynastyId(1), DynastyId(1), 3));
    CHECK(back.value().entries()[1].command == recruit(DynastyId(), DynastyId(2), 0));
    CHECK(back.value().entries()[1].outcome == Rejection::InvalidState);
    CHECK(back.value().entries()[2].command.payload.index() == CommandPayload(cmd::AdjustHonor{}).index());
    CHECK(back.value().serialize() == bytes);
}

TEST_CASE("policy commands: a replay (PRP1) with them is read back and reproduces the hashes") {
    const auto record = [](Replay* out) {
        World world = make_world(12);
        Simulation sim = economy_sim(world);
        ReplayRecorder recorder(5, 77, sim.world().today(), 10);
        for (int day = 0; day < 120; ++day) {
            if (day == 3) {
                REQUIRE(sim.submit(tax(DynastyId(0), DynastyId(0), 3)));
            }
            if (day == 20) {
                REQUIRE(sim.submit(recruit(DynastyId(), DynastyId(0), 2)));
            }
            if (day == 40) {
                REQUIRE(sim.submit(tax(DynastyId(1), DynastyId(0), 0)));  // refused: not the owner
            }
            if (day == 60) {
                REQUIRE(sim.submit(tax(DynastyId(0), DynastyId(0), 0)));
            }
            (void)sim.tick();
            recorder.after_tick(sim);
        }
        *out = recorder.finish(sim);
        return sim.world().compute_hash();
    };
    Replay replay;
    const WorldHash final_hash = record(&replay);
    REQUIRE(replay.commands.size() == 3);  // the refused one is not in a replay
    CHECK(replay.commands[0].command == tax(DynastyId(0), DynastyId(0), 3));
    CHECK(replay.commands[1].command == recruit(DynastyId(), DynastyId(0), 2));

    const std::vector<std::uint8_t> bytes = serialize_replay(replay);
    const auto read = deserialize_replay(bytes);
    REQUIRE(read.has_value());
    CHECK(read.value().commands == replay.commands);  // (the extended hash parts are not in the file)
    CHECK(serialize_replay(read.value()) == bytes);

    Simulation player = economy_sim(make_world(12));
    const ReplayResult played = play_replay(player, read.value());
    CHECK(played.error.empty());
    CHECK(played.ok);
    CHECK(player.world().compute_hash() == final_hash);

    Replay again;
    CHECK(record(&again) == final_hash);  // the same run, the same bytes
    CHECK(serialize_replay(again) == bytes);
}

// ---- validate ---------------------------------------------------------------------------------------

TEST_CASE("SetTaxPolicy and SetRecruitPolicy: every rejection and its reason") {
    World world = make_world(4);
    // the owner (any dynasty sets its own policy) and the system may
    CHECK(verdict(world, tax(DynastyId(1), DynastyId(1), 0)) == Rejection::None);
    CHECK(verdict(world, tax(DynastyId(), DynastyId(2), 3)) == Rejection::None);
    CHECK(verdict(world, recruit(DynastyId(2), DynastyId(2), 2)) == Rejection::None);
    // somebody else's policy: not authorized, whatever the level
    CHECK(verdict(world, tax(DynastyId(1), DynastyId(0), 1)) == Rejection::NotAuthorized);
    CHECK(verdict(world, recruit(DynastyId(1), DynastyId(0), 1)) == Rejection::NotAuthorized);
    CHECK(verdict(world, tax(DynastyId(7), DynastyId(0), 1)) ==
          Rejection::UnknownEntity);  // an unknown actor
    // a level the rules do not define: the tax has 4 levels, the recruit policy 3
    CHECK(verdict(world, tax(DynastyId(0), DynastyId(0), 3)) == Rejection::None);
    CHECK(verdict(world, tax(DynastyId(0), DynastyId(0), 4)) == Rejection::InvalidArgument);
    CHECK(verdict(world, tax(DynastyId(), DynastyId(0), 255)) == Rejection::InvalidArgument);
    CHECK(verdict(world, recruit(DynastyId(0), DynastyId(0), 2)) == Rejection::None);
    CHECK(verdict(world, recruit(DynastyId(0), DynastyId(0), 3)) == Rejection::InvalidArgument);
    // nobody to set it for
    CHECK(verdict(world, tax(DynastyId(), DynastyId(9), 1)) == Rejection::UnknownEntity);
    CHECK(verdict(world, tax(DynastyId(), DynastyId(), 1)) == Rejection::UnknownEntity);
    CHECK(verdict(world, recruit(DynastyId(0), DynastyId(9), 1)) == Rejection::UnknownEntity);
    // a rejected command changes nothing
    const WorldHash before = world.compute_hash();
    CHECK(verdict(world, tax(DynastyId(0), DynastyId(0), 4)) == Rejection::InvalidArgument);
    CommandLog log;
    const std::vector<Command> refused{tax(DynastyId(1), DynastyId(0), 2),
                                       tax(DynastyId(0), DynastyId(0), 9)};
    const ApplyStats stats = process_commands(world, refused, CommandSource::External, log);
    CHECK(stats.applied == 0);
    CHECK(stats.rejected == 2);
    CHECK(world.compute_hash() == before);
    CHECK(log.entries()[0].outcome == Rejection::NotAuthorized);
    CHECK(log.entries()[1].outcome == Rejection::InvalidArgument);
}

TEST_CASE("SetTaxPolicy: without economy it is refused; without the levels only the structural limit holds") {
    World none(Date::from_ymd(1703, 1, 1), 5);
    (void)none.create_dynasty(StringId(1));
    CHECK(verdict(none, tax(DynastyId(), DynastyId(0), 1)) == Rejection::InvalidState);
    CHECK_FALSE(none.set_economy_policy_levels(4, 3));
    CHECK_FALSE(none.set_dynasty_tax_policy(DynastyId(0), 1));

    World unbound = make_world(2, Date::from_ymd(1702, 12, 31), false);
    CHECK(unbound.economy().meta.tax_levels == 0);
    CHECK(verdict(unbound, tax(DynastyId(), DynastyId(0), 7)) == Rejection::None);  // below kMaxPolicyLevels
    CHECK(verdict(unbound, tax(DynastyId(), DynastyId(0), 8)) == Rejection::InvalidArgument);
    CHECK_FALSE(unbound.set_economy_policy_levels(9, 3));  // more than the structure allows
    CHECK(unbound.set_economy_policy_levels(4, 3));
    CHECK(verdict(unbound, tax(DynastyId(), DynastyId(0), 4)) == Rejection::InvalidArgument);

    // a running system binds the levels of its rules by itself
    Simulation sim = economy_sim(make_world(2, Date::from_ymd(1702, 12, 31), false));
    sim.run_days(1);
    CHECK(sim.world().economy().meta.tax_levels == rules().tax.level_count);
    CHECK(sim.world().economy().meta.recruit_levels == rules().recruit.level_count);
}

// ---- what the command does --------------------------------------------------------------------------

TEST_CASE("a policy command changes the choice of the dynasty and nothing else") {
    World world = make_world(6);
    world.economy_columns_mut().tax_policy[1] = 3;  // a decree of one province stays what it is
    CommandLog log;
    const std::vector<Command> commands{tax(DynastyId(0), DynastyId(0), 3),
                                        recruit(DynastyId(), DynastyId(2), 0)};
    const World before = world;
    const ApplyStats stats = process_commands(world, commands, CommandSource::External, log);
    CHECK(stats.applied == 2);

    EconomyState expected = before.economy();
    expected.dynasties.tax_policy[0] = 3;
    expected.dynasties.recruit_policy[2] = 0;
    CHECK(world.economy() == expected);
    CHECK(world.provinces().owner == before.provinces().owner);
    CHECK(world.dynasties().honor == before.dynasties().honor);
    CHECK(world.treasury(AccountRef::of(DynastyId(0))) == before.treasury(AccountRef::of(DynastyId(0))));
    CHECK(world.compute_hash() != before.compute_hash());
    require_valid(world);
    CHECK(check_rules_invariants(world, rules()).empty());

    // the same choice again is accepted and changes nothing more
    const WorldHash once = world.compute_hash();
    (void)process_commands(
        world, std::vector<Command>{tax(DynastyId(0), DynastyId(0), 3)}, CommandSource::External, log);
    CHECK(world.compute_hash() == once);
}

TEST_CASE("the level of a province: its decree, its owner's choice, the default of the rules") {
    World world = make_world(5);
    REQUIRE(world.set_province_owner_kind(ProvinceId(4), OwnerKind::Crown));
    (void)world.set_province_owner(ProvinceId(3), DynastyId(1));
    const std::uint8_t usual_tax = static_cast<std::uint8_t>(rules().tax.default_level);
    const std::uint8_t usual_recruit = static_cast<std::uint8_t>(rules().recruit.default_level);
    CHECK(usual_tax == 1);
    CHECK(usual_recruit == 1);

    // nobody has chosen: everybody is at the default of the rules, although the rows hold "not chosen"
    for (std::uint32_t p = 0; p < 5; ++p) {
        CHECK(world.economy().provinces.tax_policy[p] == kPolicyDefault);
        CHECK(effective_tax_level(world, rules(), ProvinceId(p)) == usual_tax);
        CHECK(effective_recruit_level(world, rules(), ProvinceId(p)) == usual_recruit);
    }
    // dynasty 0 chooses: all its provinces follow; the province of dynasty 1 and that of the crown do not
    REQUIRE(world.set_dynasty_tax_policy(DynastyId(0), 3));
    REQUIRE(world.set_dynasty_recruit_policy(DynastyId(0), 0));
    for (std::uint32_t p = 0; p < 3; ++p) {
        CHECK(effective_tax_level(world, rules(), ProvinceId(p)) == 3);
        CHECK(effective_recruit_level(world, rules(), ProvinceId(p)) == 0);
    }
    CHECK(effective_tax_level(world, rules(), ProvinceId(3)) == usual_tax);
    CHECK(effective_tax_level(world, rules(), ProvinceId(4)) == usual_tax);
    // a province that changes hands follows its new owner at once, and the crown never follows anybody
    (void)world.set_province_owner(ProvinceId(3), DynastyId(0));
    CHECK(effective_tax_level(world, rules(), ProvinceId(3)) == 3);
    (void)world.set_province_owner(ProvinceId(0), DynastyId(1));
    CHECK(effective_tax_level(world, rules(), ProvinceId(0)) == usual_tax);
    REQUIRE(world.set_province_owner_kind(ProvinceId(1), OwnerKind::Church));
    CHECK(effective_tax_level(world, rules(), ProvinceId(1)) == usual_tax);
    // a decree of the province itself beats the choice of the owner
    world.economy_columns_mut().tax_policy[2] = 0;
    CHECK(effective_tax_level(world, rules(), ProvinceId(2)) == 0);
    CHECK(effective_recruit_level(world, rules(), ProvinceId(2)) == 0);
    // unknown province, no economy: the default
    CHECK(effective_tax_level(world, rules(), ProvinceId(99)) == usual_tax);
    const World none(Date::from_ymd(1703, 1, 1), 1);
    CHECK(effective_recruit_level(none, rules(), ProvinceId(0)) == usual_recruit);
    require_valid(world);
}

TEST_CASE("a province that never had a command is taxed at the default level, not at level 0") {
    // T08 kept a new row at level 0 and every test set the default by hand; now the default is the
    // level a row without a choice is processed at (here: the usual x1.0, level 1)
    const auto first_month_income = [](bool chosen, std::uint8_t level) {
        World world = make_world(1);
        if (chosen) {
            REQUIRE(world.set_dynasty_tax_policy(DynastyId(0), level));
        }
        Simulation sim = economy_sim(std::move(world));
        sim.run_days(31);
        return sim.world().economy().provinces.income_last_month[0];
    };
    const Kopecks nothing_chosen = first_month_income(false, 0);
    CHECK(nothing_chosen > 0);
    CHECK(nothing_chosen == first_month_income(true, static_cast<std::uint8_t>(rules().tax.default_level)));
    CHECK(nothing_chosen > first_month_income(true, 0));
    CHECK(nothing_chosen < first_month_income(true, 3));
}

// ---- the effect in the monthly pass -----------------------------------------------------------------

namespace {

constexpr std::uint32_t kStaggered = 29;  // provinces 0..27 have the slot days 1..28; 28 has day 1 again

std::int32_t slot_day(std::uint32_t province) {
    return static_cast<std::int32_t>(province % 28) + 1;
}

/// The income of every province after `days` days; `issue_on` is the day (1-based, within the first
/// month) on whose tick the command is applied (0 = the command is never given).
std::vector<Kopecks> incomes(Date start, int issue_on, int days) {
    Simulation sim = economy_sim(make_world(kStaggered, start));
    if (issue_on > 0) {
        sim.run_days(issue_on - 1);
        REQUIRE(sim.submit(tax(DynastyId(0), DynastyId(0), 3)));
    }
    sim.run_days(days - (issue_on > 0 ? issue_on - 1 : 0));
    return sim.world().economy().provinces.income_last_month;
}

}  // namespace

TEST_CASE("the policy shows in the next monthly pass of each province, in months of 28, 29, 30 and 31 days") {
    struct Month {
        Date before;  // the day before the first of the month
        int length;
    };
    const std::array<Month, 4> months{{{Date::from_ymd(1702, 12, 31), 31},
                                       {Date::from_ymd(1703, 1, 31), 28},
                                       {Date::from_ymd(1703, 3, 31), 30},
                                       {Date::from_ymd(1704, 1, 31), 29}}};
    for (const Month& month : months) {
        CAPTURE(month.length);
        REQUIRE(Date::days_in_month((month.before + 1).year(), (month.before + 1).month()) == month.length);
        const std::vector<Kopecks> control = incomes(month.before, 0, month.length);
        for (const int issue_on : {1, 15, month.length}) {
            CAPTURE(issue_on);
            const std::vector<Kopecks> changed = incomes(month.before, issue_on, month.length);
            for (std::uint32_t p = 0; p < kStaggered; ++p) {
                CAPTURE(p);
                REQUIRE(control[p] > 0);
                // the command is applied at the start of the tick of `issue_on`, before the slots of the
                // day: a province whose slot came earlier was taxed as before, the others at the new level
                // (days 29..31 have no slots: the command of those days waits for the next month)
                if (slot_day(p) < issue_on) {
                    CHECK(changed[p] == control[p]);
                } else {
                    CHECK(changed[p] > control[p]);
                }
            }
        }
        // the month after, every province is taxed at the new level
        const std::vector<Kopecks> control2 = incomes(month.before, 0, month.length + 28);
        const std::vector<Kopecks> changed2 = incomes(month.before, month.length, month.length + 28);
        for (std::uint32_t p = 0; p < kStaggered; ++p) {
            CHECK(changed2[p] > control2[p]);
        }
    }
}

TEST_CASE("a policy chosen on a day without slots changes nothing until the next month") {
    // the 30th: no province is processed on the days 29..31, the command waits
    const Date start = Date::from_ymd(1702, 12, 31);
    Simulation a = economy_sim(make_world(kStaggered, start));
    Simulation b = economy_sim(make_world(kStaggered, start));
    a.run_days(29);
    b.run_days(29);
    REQUIRE(a.submit(tax(DynastyId(0), DynastyId(0), 3)));
    a.run_days(2);  // the 30th and 31st
    b.run_days(2);
    CHECK(a.world().economy().provinces == b.world().economy().provinces);
    CHECK(a.world().treasury(AccountRef::of(DynastyId(0))) ==
          b.world().treasury(AccountRef::of(DynastyId(0))));
    a.run_days(1);  // February 1st
    b.run_days(1);
    CHECK(a.world().treasury(AccountRef::of(DynastyId(0))) >
          b.world().treasury(AccountRef::of(DynastyId(0))));
}

TEST_CASE("the recruit policy reaches the mood of the people in the next pass") {
    // heavier levies weigh on the mood (F8): the recruit pressure of the level, read at the slot
    const auto mood_after = [](std::uint8_t level) {
        World world = make_world(1);
        REQUIRE(world.set_dynasty_recruit_policy(DynastyId(0), level));
        Simulation sim = economy_sim(std::move(world));
        sim.run_days(31 * 6);
        return sim.world().economy().groups.mood[index_of(Group::Peasants)].raw();
    };
    CHECK(mood_after(0) > mood_after(2));
}

TEST_CASE("a hand-over of a province moves it to the policy of the new owner at its next pass") {
    World world = make_world(kStaggered);
    REQUIRE(world.set_dynasty_tax_policy(DynastyId(0), 3));
    REQUIRE(world.set_dynasty_tax_policy(DynastyId(1), 0));
    Simulation sim = economy_sim(world);
    sim.run_days(31);
    const Kopecks heavy = sim.world().economy().provinces.income_last_month[5];
    REQUIRE(sim.submit(Command{DynastyId(), cmd::SetProvinceOwner{ProvinceId(5), DynastyId(1)}}));
    sim.run_days(28);
    CHECK(sim.world().economy().provinces.income_last_month[5] < heavy);  // dynasty 1 taxes lightly
    CHECK(sim.world().economy().provinces.income_last_month[6] >
          sim.world().economy().provinces.income_last_month[5]);
}

// ---- saves ------------------------------------------------------------------------------------------

TEST_CASE("a save in the middle of the month keeps the choices, and the run goes on to the same state") {
    for (const bool command_before_save : {true, false}) {
        CAPTURE(command_before_save);
        World world = make_world(kStaggered);
        Simulation straight = economy_sim(world);
        Simulation first = economy_sim(world);
        const Command command = tax(DynastyId(0), DynastyId(0), 2);
        const Command more = recruit(DynastyId(0), DynastyId(0), 0);
        straight.run_days(9);
        first.run_days(9);
        if (command_before_save) {
            REQUIRE(straight.submit(command));
            REQUIRE(first.submit(command));
            straight.run_days(5);
            first.run_days(5);
        }

        pat::core::ByteWriter out;
        first.world().save_state(out);
        pat::core::ByteReader in(out.data());
        auto loaded = World::load_state(in);
        REQUIRE(loaded.has_value());
        CHECK(loaded.value().compute_hash() == first.world().compute_hash());
        CHECK(loaded.value().economy().dynasties == first.world().economy().dynasties);
        CHECK(loaded.value().economy().meta.tax_levels == 4);
        CHECK(loaded.value().economy().meta.recruit_levels == 3);
        Simulation resumed = economy_sim(make_world(kStaggered));  // at tick 0, with the systems
        resumed.restore_state(std::move(loaded).value(), {});

        for (Simulation* sim : {&straight, &resumed}) {
            if (!command_before_save) {
                REQUIRE(sim->submit(command));
            }
            REQUIRE(sim->submit(more));
            sim->run_days(70);
        }
        CHECK(resumed.world().compute_hash() == straight.world().compute_hash());
        CHECK(resumed.world().economy() == straight.world().economy());
        CHECK(resumed.world().economy().dynasties.tax_policy[0] == 2);
        CHECK(resumed.world().economy().dynasties.recruit_policy[0] == 0);
        require_valid(resumed.world());
    }
}

namespace {

/// The state bytes of `world` as schema 8 wrote them: no section 40, and the word of the schema. The
/// extension block is the end of the state: the section table is found as the one offset from which a
/// count and ascending sections use up the bytes exactly.
std::vector<std::uint8_t> as_schema_8(const World& world) {
    pat::core::ByteWriter out;
    world.save_state(out);
    const std::span<const std::uint8_t> view = out.data();
    std::vector<std::uint8_t> bytes(view.begin(), view.end());
    const auto u16_at = [&](std::size_t at) {
        return static_cast<std::size_t>(bytes[at] | (bytes[at + 1] << 8));
    };
    const auto u32_at = [&](std::size_t at) {
        return static_cast<std::size_t>(u16_at(at)) | (u16_at(at + 2) << 16);
    };
    std::size_t table = 0;
    std::size_t found = 0;
    std::size_t last_section = 0;
    for (std::size_t at = 8; at + 2 <= bytes.size(); ++at) {
        const std::size_t count = u16_at(at);
        if (count < 3 || count > 64) {
            continue;
        }
        std::size_t cursor = at + 2;
        std::size_t previous_id = 0;
        std::size_t last = 0;
        bool ok = true;
        for (std::size_t s = 0; s < count && ok; ++s) {
            if (cursor + 8 > bytes.size()) {
                ok = false;
                break;
            }
            const std::size_t id = u16_at(cursor);
            ok = id > previous_id && id < 64 && u16_at(cursor + 2) == 1;
            previous_id = id;
            last = cursor;
            cursor += 8 + u32_at(cursor + 4);
        }
        if (ok && cursor == bytes.size() && previous_id == 40) {
            table = at;
            last_section = last;
            ++found;
        }
    }
    REQUIRE(found == 1);
    const std::size_t count = u16_at(table);
    bytes.resize(last_section);  // section 40 is the last one
    bytes[table] = static_cast<std::uint8_t>((count - 1) & 0xFF);
    bytes[table + 1] = static_cast<std::uint8_t>((count - 1) >> 8);
    bytes[0] = 8;  // the schema word, little endian
    bytes[1] = bytes[2] = bytes[3] = 0;
    return bytes;
}

}  // namespace

TEST_CASE("migration 8 -> 9: an older save has no choices, so every dynasty is at the default") {
    REQUIRE(World::kStateSchema == 9);
    World world = make_world(8);
    world.economy_columns_mut().tax_policy[2] = 2;  // a decree is a column of the provinces: it is kept
    const std::vector<std::uint8_t> old = as_schema_8(world);
    pat::core::ByteReader in(old);
    auto loaded = World::load_state(in);
    REQUIRE(loaded.has_value());
    const World& migrated = loaded.value();
    CHECK(migrated.economy().dynasties.tax_policy == std::vector<std::uint8_t>(3, kPolicyDefault));
    CHECK(migrated.economy().dynasties.recruit_policy == std::vector<std::uint8_t>(3, kPolicyDefault));
    CHECK(migrated.economy().meta.tax_levels == 0);  // the rules are not in the file: unknown
    CHECK(migrated.economy().provinces == world.economy().provinces);
    CHECK(migrated.compute_hash() == world.compute_hash());  // nothing was chosen, nothing differs
    require_valid(migrated);

    // the world of schema 9 and the migrated one run to the same state
    World migrated_copy = migrated;
    bind_policy_levels(migrated_copy, rules());
    Simulation a = economy_sim(std::move(migrated_copy));
    Simulation b = economy_sim(world);
    a.run_days(100);
    b.run_days(100);
    CHECK(a.world().compute_hash() == b.world().compute_hash());

    // a file of schema 8 that has a section 40 is not a file of schema 8
    pat::core::ByteWriter current;
    world.save_state(current);
    std::vector<std::uint8_t> lie(current.data().begin(), current.data().end());
    lie[0] = 8;
    pat::core::ByteReader lie_in(lie);
    CHECK_FALSE(World::load_state(lie_in).has_value());
}

TEST_CASE("a damaged policy section is refused") {
    World world = make_world(4);
    REQUIRE(world.set_dynasty_tax_policy(DynastyId(1), 2));
    pat::core::ByteWriter out;
    world.save_state(out);
    const std::span<const std::uint8_t> view = out.data();
    const std::vector<std::uint8_t> good(view.begin(), view.end());
    {
        pat::core::ByteReader in(good);
        REQUIRE(World::load_state(in).has_value());
    }
    // the section is the last one: u8 tax_levels | u8 recruit_levels | u32 rows | rows x 2 bytes
    const std::size_t body = 2 + 4 + 2 * 3;
    const std::size_t first_row = good.size() - 2 * 3;
    const auto load = [](const std::vector<std::uint8_t>& bytes) {
        pat::core::ByteReader in(bytes);
        return World::load_state(in).has_value();
    };
    std::vector<std::uint8_t> bad = good;
    bad[first_row + 2] = 9;  // a tax policy that is neither a level nor "not chosen"
    CHECK_FALSE(load(bad));
    bad = good;
    bad[first_row + 3] = 200;
    CHECK_FALSE(load(bad));
    bad = good;
    bad[good.size() - body] = 9;  // more levels than the structure has
    CHECK_FALSE(load(bad));
    bad = good;
    bad[good.size() - body + 2] = 7;  // the number of rows is not the number of dynasties
    CHECK_FALSE(load(bad));
    bad = good;
    bad.resize(good.size() - 1);  // truncated
    CHECK_FALSE(load(bad));
}

TEST_CASE("the rules invariant E-R1 reports a policy the rules do not define") {
    World world = make_world(3);
    CHECK(check_rules_invariants(world, rules()).empty());
    REQUIRE(world.set_economy_policy_levels(8, 8));  // a world built with other rules...
    auto problems = check_rules_invariants(world, rules());
    REQUIRE(problems.size() == 2);
    REQUIRE(
        world.set_dynasty_tax_policy(DynastyId(1), 6));  // ...lets a level through that these do not define
    REQUIRE(world.set_economy_policy_levels(4, 3));
    problems = check_rules_invariants(world, rules());
    REQUIRE(problems.size() == 1);
    CHECK(problems[0].find("dynasty 1 has the tax policy level 6") != std::string::npos);
    REQUIRE(world.set_dynasty_tax_policy(DynastyId(1), 3));
    CHECK(check_rules_invariants(world, rules()).empty());
}
