#include "pat/sim/command_log.hpp"
#include "pat/sim/replay.hpp"
#include "pat/sim/save.hpp"
#include "pat/sim/succession_commands.hpp"

#include <algorithm>
#include <array>
#include <memory>

#include "marriage_test_fixture.hpp"

namespace {
using namespace pat::sim;
using pat::core::Fixed32;
using Denial = SuccessionDenial;
using economy::AccountRef;

struct Scene : pat::test::marriage::Scene {
    SuccessionRules rules = SuccessionRules::defaults();
    CharacterId head;
    CharacterId elder;
    CharacterId younger;
    Scene() {
        head = person(groom_house, Sex::Male, Date::from_ymd(1640, 1, 1));
        elder = person(groom_house, Sex::Male, Date::from_ymd(1660, 1, 1), head);
        younger = person(groom_house, Sex::Male, Date::from_ymd(1670, 1, 1), head);
        REQUIRE(world.set_dynasty_head(groom_house, head));
        world.add_honor(groom_house, 100);
        world.set_dynasty_unity(groom_house, Fixed32::from_int(80));
        REQUIRE(world.enable_economy());
        REQUIRE(world.set_initial_treasury(AccountRef::of(groom_house), 90000));
    }
    CommandContent content() const { return {nullptr, nullptr, nullptr, &rules}; }
    void execute(const Command& command) {
        REQUIRE(validate(world, command, content()).has_value());
        apply(world, command, content());
        CHECK(world.check_invariants().empty());
    }
    void rejected(const SuccessionCommand& payload, Denial expected) {
        const Command command = std::visit([&](const auto& c) { return Command{groom_house, c}; }, payload);
        const auto before = world.compute_hash();
        const auto reasons = check_succession_command(world, rules, groom_house, payload);
        REQUIRE(std::ranges::find(reasons, expected) != reasons.end());
        const auto verdict = validate(world, command, content());
        REQUIRE_FALSE(verdict.has_value());
        CHECK(verdict.error().code == static_cast<std::uint16_t>(reasons.front()));
        CommandLog log;
        const std::array commands{command};
        const auto stats = process_commands(world, commands, CommandSource::External, log, content());
        CHECK(stats.applied == 0);
        CHECK(stats.rejected == 1);
        CHECK(world.compute_hash() == before);
    }
    void status(CharacterId id, std::uint8_t flag) {
        family::PersonStatus row;
        row.character = id;
        row.flags = flag;
        if ((flag & family::kPersonTonsured) != 0)
            row.tonsured_on = world.today();
        if ((flag & family::kPersonDisowned) != 0)
            row.disowned_on = world.today();
        REQUIRE(world.set_family_status(row));
    }
};
}  // namespace

TEST_CASE("succession commands: reform preview is pure and matches applied resolution and costs") {
    Scene s;
    s.rules.reform_money_kop = 12345;
    s.rules.reform_honor = 17;
    s.rules.reform_unity = 6;
    const auto hash = s.world.compute_hash();
    economy::Breakdown points, money;
    const auto preview = preview_law_change(
        s.world, s.rules, s.groom_house, SuccessionLaw::Appointment, s.younger, &points, &money);
    CHECK(s.world.compute_hash() == hash);
    CHECK(preview.denials.empty());
    CHECK(points.is_consistent());
    CHECK(money.is_consistent());
    CHECK(money.root().value == -12345);
    REQUIRE_FALSE(preview.losers.empty());
    CHECK(preview.losers.front().character == s.elder);
    CHECK(preview.losers.front().opinion_delta == -25);
    const auto honor = s.world.dynasties().honor[s.groom_house.index()];
    s.execute({s.groom_house, cmd::ChangeSuccessionLaw{SuccessionLaw::Appointment, s.younger}});
    CHECK(s.world.dynasties().head[s.groom_house.index()] == s.head);
    CHECK(s.world.dynasties().honor[s.groom_house.index()] == honor - 17);
    CHECK(s.world.dynasties().unity[s.groom_house.index()] == Fixed32::from_int(74));
    CHECK(s.world.treasury(AccountRef::of(s.groom_house)) == 77655);
    const auto account = *s.world.resolve_account(AccountRef::of(s.groom_house));
    CHECK(s.world.economy().accounts.open_line(account, economy::LedgerCode::CourtUpkeep) == -12345);
    CHECK(s.world.get_opinion(s.elder, s.head) == Fixed32::from_int(-25));
    CHECK(s.world.family_succession(s.groom_house)->law_changed_on == s.world.today());
    CHECK(s.world.chronicle(s.groom_house).back().param == 214);
    // Reactions target the excluded predecessor; the entire forecast remains exact.
    CHECK(resolve_successor(s.world, s.rules, s.groom_house, s.head) == preview.after);
    s.rejected(cmd::ChangeSuccessionLaw{SuccessionLaw::Partition, {}}, Denial::ReformCooldown);
}

TEST_CASE("succession commands: reform rejects each policy, heir and affordability failure atomically") {
    Scene s;
    s.rejected(cmd::ChangeSuccessionLaw{SuccessionLaw::Majorat, {}}, Denial::SameLaw);
    s.rejected(cmd::ChangeSuccessionLaw{SuccessionLaw::Minorat, {}}, Denial::LawNotSelectable);
    s.rejected(cmd::ChangeSuccessionLaw{static_cast<SuccessionLaw>(255), {}}, Denial::LawNotSelectable);
    s.rejected(cmd::ChangeSuccessionLaw{SuccessionLaw::Appointment, {}}, Denial::HeirRequired);
    s.rejected(cmd::ChangeSuccessionLaw{SuccessionLaw::Appointment, s.bride}, Denial::HeirNotMember);
    s.rejected(cmd::ChangeSuccessionLaw{SuccessionLaw::Appointment, s.head}, Denial::HeirIsHead);
    s.rejected(cmd::ChangeSuccessionLaw{SuccessionLaw::Appointment, CharacterId(9999)}, Denial::HeirNotAlive);
    s.status(s.younger, family::kPersonDisowned);
    s.rejected(cmd::ChangeSuccessionLaw{SuccessionLaw::Appointment, s.younger}, Denial::HeirIneligible);
    s.rules.reform_money_kop = 90001;
    s.rejected(cmd::ChangeSuccessionLaw{SuccessionLaw::Partition, {}}, Denial::InsufficientFunds);
}

TEST_CASE("succession commands: reform loser reactions retain the first heir and a bounded stable list") {
    Scene s;
    for (int i = 0; i < 10; ++i)
        (void)s.person(s.groom_house, Sex::Male, Date::from_ymd(1671 + i, 1, 1), s.head);
    const auto heir = s.person(s.groom_house, Sex::Male, Date::from_ymd(1685, 1, 1), s.head);
    s.rules.reform_max_losers = 255;  // The structural cap remains eight even with a larger content value.
    const auto preview =
        preview_law_change(s.world, s.rules, s.groom_house, SuccessionLaw::Appointment, heir);
    REQUIRE(preview.losers.size() == 8);
    CHECK(preview.losers.front().character == s.elder);
    CHECK(preview.losers.front().opinion_delta == -25);
    for (std::size_t i = 1; i < preview.losers.size(); ++i)
        CHECK(preview.losers[i].opinion_delta == -5);
    s.rules.reform_max_losers = 2;
    const auto smaller =
        preview_law_change(s.world, s.rules, s.groom_house, SuccessionLaw::Appointment, heir);
    REQUIRE(smaller.losers.size() == 2);
    CHECK(smaller.losers[0] == preview.losers[0]);
    CHECK(smaller.losers[1] == preview.losers[1]);
}

TEST_CASE("succession commands: SetHeir permits minors, validates guardians and preserves absent choice") {
    Scene s;
    const auto child = s.person(s.groom_house, Sex::Male, Date::from_ymd(1700, 1, 1), s.head);
    s.rejected(cmd::SetHeir{child, {}}, Denial::LawNotAppointment);
    s.world.set_dynasty_succession_law(s.groom_house, SuccessionLaw::Appointment);
    s.rejected(cmd::SetHeir{child, child}, Denial::InvalidGuardian);
    s.rejected(cmd::SetHeir{child, s.bride}, Denial::InvalidGuardian);
    s.execute({s.groom_house, cmd::SetHeir{child, s.younger}});
    CHECK(s.world.family_succession(s.groom_house)->guardian == s.younger);
    CHECK(s.world.dynasties().unity[s.groom_house.index()] == Fixed32::from_int(77));
    CHECK(s.world.get_opinion(s.elder, s.head) == Fixed32::from_int(-15));
    const auto outcome = resolve_successor(s.world, s.rules, s.groom_house, s.head);
    CHECK(outcome.winner == child);
    CHECK(outcome.minor);
    CHECK(s.world.family_regency(s.groom_house) == nullptr);  // T17 applies regency on accession.
    s.execute({s.groom_house, cmd::SetHeir{s.elder, {}}});
    CHECK(s.world.family_succession(s.groom_house)->guardian == s.younger);
    CHECK(s.world.chronicle(s.groom_house).back().param == 215);
}

TEST_CASE("succession commands: disown excludes the target and revokes grants without moving ownership") {
    Scene s;
    const auto province = s.world.create_province({}, s.groom_house, Fixed32::from_int(1));
    REQUIRE(s.world.set_province_administrator(province, s.elder));
    REQUIRE(s.world.set_family_land_grant({province, s.elder, s.world.today()}));
    s.rejected(cmd::Disown{s.head}, Denial::CannotDisownHead);
    s.rejected(cmd::Disown{s.bride}, Denial::NotYourHouse);
    s.rejected(cmd::Disown{CharacterId(9999)}, Denial::NotAlive);
    s.status(s.younger, family::kPersonTonsured);
    s.rejected(cmd::Disown{s.younger}, Denial::AlreadyTonsured);
    const auto treasury = s.world.treasury(AccountRef::of(s.groom_house));
    s.execute({s.groom_house, cmd::Disown{s.elder}});
    CHECK((s.world.family_status(s.elder)->flags & family::kPersonDisowned) != 0);
    CHECK(s.world.family_status(s.elder)->disowned_on == s.world.today());
    CHECK(s.world.characters().loyalty_head[s.elder.index()] == Fixed32::from_int(-30));
    CHECK(s.world.get_opinion(s.elder, s.head) == Fixed32::from_int(-60));
    CHECK(s.world.dynasties().unity[s.groom_house.index()] == Fixed32::from_int(75));
    CHECK(s.world.provinces().owner[province.index()] == s.groom_house);
    CHECK(s.world.family_land_grant(province) == nullptr);
    CHECK_FALSE(s.world.economy().provinces.administrator[province.index()].valid());
    CHECK(s.world.treasury(AccountRef::of(s.groom_house)) == treasury);
    CHECK_FALSE(succession_candidate_eligible(s.world, s.rules, s.groom_house, s.head, s.elder));
    CHECK(s.world.chronicle(s.groom_house).back().param == 216);
    s.rejected(cmd::Disown{s.elder}, Denial::AlreadyDisowned);
}

TEST_CASE("succession commands: regency and dispute roles cannot be rewritten") {
    Scene s;
    const auto child = s.person(s.groom_house, Sex::Male, Date::from_ymd(1700, 1, 1), s.head);
    REQUIRE(s.world.set_dynasty_head(s.groom_house, child));
    REQUIRE(s.world.set_family_regency(
        {s.groom_house, child, s.elder, family::RegencyKind::Guardian, s.world.today(), 0, 0, {}}));
    s.world.set_dynasty_succession_law(s.groom_house, SuccessionLaw::Appointment);
    s.rejected(cmd::SetHeir{s.younger, {}}, Denial::RegencyActive);
    s.rejected(cmd::ChangeSuccessionLaw{SuccessionLaw::Partition, {}}, Denial::RegencyActive);
    s.rejected(cmd::Disown{s.elder}, Denial::ActiveRole);
    REQUIRE(s.world.clear_family_regency(s.groom_house));
    REQUIRE(s.world.set_dynasty_head(s.groom_house, {}));
    family::Dispute dispute;
    dispute.dynasty = s.groom_house;
    REQUIRE(s.world.die(s.head, CauseOfDeath::Illness));
    dispute.predecessor = s.head;
    dispute.opened = s.world.today();
    dispute.resolve_by = Date::from_days(s.world.today().days() + 30);
    dispute.claimants_n = 2;
    dispute.claimants[0] = {s.elder, 10};
    dispute.claimants[1] = {s.younger, 5};
    REQUIRE(s.world.set_family_dispute(dispute));
    s.rejected(cmd::ChangeSuccessionLaw{SuccessionLaw::Partition, {}}, Denial::DisputeOpen);
    s.rejected(cmd::Disown{s.younger}, Denial::DisputeOpen);
}

TEST_CASE("succession commands: actor, head, extinction and dynasty mode failures are pure") {
    Scene s;
    CHECK_FALSE(validate(s.world, {DynastyId{}, cmd::Disown{s.elder}}, s.content()).has_value());
    CHECK_FALSE(validate(s.world, {DynastyId(9999), cmd::Disown{s.elder}}, s.content()).has_value());
    REQUIRE(s.world.set_dynasty_head(s.groom_house, {}));
    s.rejected(cmd::Disown{s.elder}, Denial::NoActingHead);
    const auto empty = s.world.create_dynasty({});
    const auto reasons = check_succession_command(s.world, s.rules, empty, cmd::Disown{s.elder});
    CHECK(std::ranges::find(reasons, Denial::DynastyExtinct) != reasons.end());
    World legacy(s.world.today(), 1);
    const auto house = legacy.create_dynasty({});
    const auto mode = check_succession_command(legacy, s.rules, house, cmd::Disown{});
    CHECK(std::ranges::find(mode, Denial::DynastyModeRequired) != mode.end());
}

TEST_CASE("succession commands: a saturated reform ledger line rejects before any consequences") {
    Scene s;
    // Existing postings can saturate one line while an offsetting line preserves an affordable treasury.
    for (economy::Kopecks posted = 0; posted < economy::kMaxLedgerLineKop;
         posted += economy::kMaxPostingKop) {
        REQUIRE(s.world.post_money(AccountRef::of(s.groom_house),
                                   economy::LedgerCode::CourtUpkeep,
                                   -economy::kMaxPostingKop) == -economy::kMaxPostingKop);
        REQUIRE(s.world.post_money(AccountRef::of(s.groom_house),
                                   economy::LedgerCode::Adjustment,
                                   economy::kMaxPostingKop) == economy::kMaxPostingKop);
    }
    CHECK(s.world.treasury(AccountRef::of(s.groom_house)) == 90000);
    CHECK(s.world.check_invariants().empty());
    s.rejected(cmd::ChangeSuccessionLaw{SuccessionLaw::Partition, {}}, Denial::MoneyPostingBlocked);
}

TEST_CASE("succession commands: reform cooldown boundary and no-economy accounting use one policy") {
    Scene s;
    s.rules.reform_cooldown_days = 10;
    REQUIRE(
        s.world.set_family_succession({s.groom_house, {}, {}, Date::from_days(s.world.today().days() - 9)}));
    s.rejected(cmd::ChangeSuccessionLaw{SuccessionLaw::Partition, {}}, Denial::ReformCooldown);
    REQUIRE(
        s.world.set_family_succession({s.groom_house, {}, {}, Date::from_days(s.world.today().days() - 10)}));
    s.execute({s.groom_house, cmd::ChangeSuccessionLaw{SuccessionLaw::Partition, {}}});
    pat::test::marriage::Scene bare;
    REQUIRE(bare.world.set_dynasty_head(bare.groom_house, bare.groom));
    const auto heir = bare.person(bare.groom_house, Sex::Male);
    const auto preview =
        preview_law_change(bare.world, s.rules, bare.groom_house, SuccessionLaw::Appointment, heir);
    CHECK(preview.cost.money_kop == 0);
    CHECK(preview.denials.empty());
    const Command command{bare.groom_house, cmd::ChangeSuccessionLaw{SuccessionLaw::Appointment, heir}};
    REQUIRE(validate(bare.world, command, CommandContent{nullptr, nullptr, nullptr, &s.rules}).has_value());
    apply(bare.world, command, CommandContent{nullptr, nullptr, nullptr, &s.rules});
    CHECK(bare.world.check_invariants().empty());
}

TEST_CASE("succession commands: illegitimacy, female policy and guardian status share eligibility") {
    Scene s;
    s.world.set_dynasty_succession_law(s.groom_house, SuccessionLaw::Appointment);
    CharacterInit init;
    init.dynasty = s.groom_house;
    init.sex = Sex::Male;
    init.birth_date = Date::from_ymd(1680, 1, 1);
    init.legitimacy = Legitimacy::Illegitimate;
    const auto illegitimate = s.world.create_character(init);
    REQUIRE(illegitimate.valid());
    s.rejected(cmd::SetHeir{illegitimate, {}}, Denial::HeirIneligible);
    s.rules.illegitimate_inherit = true;
    s.execute({s.groom_house, cmd::SetHeir{illegitimate, {}}});
    const auto daughter = s.person(s.groom_house, Sex::Female);
    s.execute({s.groom_house, cmd::SetHeir{daughter, {}}});  // Appointment overrides IfNoAdultMale.
    s.rules.female_succession = FemaleSuccession::Never;
    s.rejected(cmd::SetHeir{daughter, {}}, Denial::HeirIneligible);
    s.status(s.younger, family::kPersonTonsured);
    s.rejected(cmd::SetHeir{s.elder, s.younger}, Denial::InvalidGuardian);
}

TEST_CASE(
    "succession commands: configurable rules, commands and persistent results survive saves and replay") {
    Scene s;
    s.rules.reform_money_kop = 700;
    s.rules.appointment_skip_unity = 1;
    const World initial = s.world;
    SimulationConfig config;
    config.succession_rules = std::make_shared<SuccessionRules>(s.rules);
    Simulation sim(initial, config);
    ReplayRecorder recorder(299, 310, initial.today(), 1);
    auto saved = deserialize_save(serialize_save(initial, {}));
    REQUIRE(saved.has_value());
    Simulation resumed(std::move(saved.value().world), config);
    const std::array<Command, 3> commands{
        {{s.groom_house, cmd::ChangeSuccessionLaw{SuccessionLaw::Appointment, s.younger}},
         {s.groom_house, cmd::SetHeir{s.elder, s.younger}},
         {s.groom_house, cmd::Disown{s.elder}}}};
    for (const auto& command : commands) {
        REQUIRE(sim.submit(command));
        REQUIRE(resumed.submit(command));
        CHECK(sim.tick().external_commands.applied == 1);
        CHECK(resumed.tick().external_commands.applied == 1);
        recorder.after_tick(sim);
    }
    CHECK(resumed.world().compute_hash() == sim.world().compute_hash());
    auto after = deserialize_save(serialize_save(sim.world(), sim.pending_internal()));
    REQUIRE(after.has_value());
    CHECK(after.value().world.compute_hash() == sim.world().compute_hash());
    auto replay = deserialize_replay(serialize_replay(recorder.finish(sim)));
    REQUIRE(replay.has_value());
    Simulation playback(initial, config);
    CHECK(play_replay(playback, replay.value()).ok);
    CHECK(playback.world().compute_hash() == sim.world().compute_hash());
    CHECK(playback.world().check_invariants().empty());
    CHECK(sim.command_log().size() == 3);
    CHECK(static_cast<std::uint16_t>(type_of(commands[0])) == 71);
    CHECK(static_cast<std::uint16_t>(type_of(commands[1])) == 70);
    CHECK(static_cast<std::uint16_t>(type_of(commands[2])) == 72);
}
