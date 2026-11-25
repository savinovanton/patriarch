#include "pat/sim/command_log.hpp"
#include "pat/sim/family_systems.hpp"
#include "pat/sim/marriage_check.hpp"
#include "pat/sim/marriage_tonsure.hpp"
#include "pat/sim/replay.hpp"
#include "pat/sim/save.hpp"
#include "pat/sim/succession.hpp"

#include <algorithm>
#include <array>
#include <memory>
#include <span>

#include "marriage_test_fixture.hpp"

namespace {

using namespace pat::sim;
using economy::AccountRef;

struct Scene : pat::test::marriage::Scene {
    MarriageRules rules = MarriageRules::defaults();
    CharacterId head;
    CharacterId other_head;

    Scene() {
        head = person(groom_house, Sex::Male);
        other_head = person(bride_house, Sex::Male);
        REQUIRE(world.set_dynasty_head(groom_house, head));
        REQUIRE(world.set_dynasty_head(bride_house, other_head));
        world.add_honor(groom_house, 200);
        world.add_unity(groom_house, pat::core::Fixed32::from_int(50), 0);
    }

    CommandContent content() const { return {nullptr, nullptr, &rules}; }

    void execute(const Command& command) {
        REQUIRE(validate(world, command, content()).has_value());
        apply(world, command, content());
        CHECK(world.check_invariants().empty());
    }

    void reject(const Command& command, std::uint16_t code = 0) {
        const auto before = world.compute_hash();
        const auto verdict = validate(world, command, content());
        REQUIRE_FALSE(verdict.has_value());
        if (code != 0) {
            CHECK(verdict.error().code == code);
        }
        CommandLog log;
        const std::array commands{command};
        const auto stats = process_commands(world, commands, CommandSource::External, log, content());
        CHECK(stats.applied == 0);
        CHECK(stats.rejected == 1);
        CHECK(world.compute_hash() == before);
    }

    family::MarriageId wed(std::int64_t dowry = 1000) {
        execute(Command{{}, cmd::Betroth{groom, bride, dowry, bride_house, false}});
        execute(Command{{}, cmd::Wed{world.family().betrothals.front().id}});
        return world.family().marriages.back().id;
    }
};

std::uint16_t code(MarriageDenial denial) {
    return static_cast<std::uint16_t>(denial);
}

}  // namespace

TEST_CASE("marriage tonsure: ends a recorded marriage, refunds once and records reasoned effects") {
    Scene s;
    REQUIRE(s.world.enable_economy());
    REQUIRE(s.world.set_initial_treasury(AccountRef::of(s.bride_house), 1000));
    const auto id = s.wed();
    REQUIRE(s.world.characters().dynasty[s.bride.index()] == s.groom_house);
    const auto honor = s.world.dynasties().honor[s.groom_house.index()];
    const auto unity = s.world.dynasties().unity[s.groom_house.index()];
    const auto opinion = s.world.get_opinion(s.other_head, s.head);
    const auto history_size = s.world.standing_changes(s.groom_house).size();
    const auto before_money =
        s.world.treasury(AccountRef::of(s.groom_house)) + s.world.treasury(AccountRef::of(s.bride_house));
    const Command command{s.groom_house, cmd::Tonsure{s.bride}};
    s.execute(command);
    CHECK_FALSE(s.world.characters().spouse[s.groom.index()].valid());
    CHECK_FALSE(s.world.characters().spouse[s.bride.index()].valid());
    CHECK(s.world.characters().ex_spouses[s.groom.index()][0] == s.bride);
    CHECK(s.world.characters().ex_spouses[s.bride.index()][0] == s.groom);
    CHECK(s.world.is_alive(s.bride));
    CHECK(s.world.characters().dynasty[s.bride.index()] == s.groom_house);
    CHECK(s.world.is_tonsured(s.bride));
    REQUIRE(s.world.family_status(s.bride) != nullptr);
    CHECK(s.world.family_status(s.bride)->tonsured_on == s.world.today());
    const auto& marriage = *s.world.find_family_marriage(id);
    CHECK(marriage.state == family::MarriageState::Closed);
    CHECK(marriage.end_reason == family::MarriageEndReason::Tonsure);
    CHECK(marriage.ended == s.world.today());
    CHECK(marriage.refunded_kop == 500);
    CHECK(marriage.claim_kop == 0);
    CHECK(s.world.treasury(AccountRef::of(s.groom_house)) == 500);
    CHECK(s.world.treasury(AccountRef::of(s.bride_house)) == 500);
    CHECK(s.world.treasury(AccountRef::of(s.groom_house)) + s.world.treasury(AccountRef::of(s.bride_house)) ==
          before_money);
    CHECK(s.world.dynasties().honor[s.groom_house.index()] == honor - s.rules.consequences.tonsure_honor);
    CHECK(s.world.dynasties().unity[s.groom_house.index()] ==
          unity - pat::core::Fixed32::from_int(s.rules.consequences.tonsure_unity));
    CHECK(s.world.get_opinion(s.other_head, s.head) ==
          opinion - pat::core::Fixed32::from_int(s.rules.consequences.tonsure_opinion));
    const auto history = s.world.standing_changes(s.groom_house);
    REQUIRE(history.size() == history_size + 2);
    CHECK(history[history_size].reason == kStandingReasonTonsure);
    CHECK(history[history_size + 1].reason == kStandingReasonTonsure);
    for (const auto house : {s.groom_house, s.bride_house}) {
        const auto notes = s.world.chronicle(house);
        REQUIRE_FALSE(notes.empty());
        CHECK(notes.back().kind == ChronicleKind::Note);
        CHECK(notes.back().param == 207);
        CHECK(notes.back().a == s.bride);
        CHECK(notes.back().b == s.groom);
    }
    s.reject(command, code(MarriageDenial::Tonsured));
}

TEST_CASE("marriage tonsure: scenario spouses need no E8 record or economy") {
    Scene s;
    REQUIRE(s.world.marry(s.groom, s.bride));
    const auto opinion = s.world.get_opinion(s.other_head, s.head);
    s.execute(Command{s.groom_house, cmd::Tonsure{s.bride}});
    CHECK(s.world.family().marriages.empty());
    CHECK(s.world.is_tonsured(s.bride));
    CHECK(s.world.get_opinion(s.other_head, s.head) ==
          opinion - pat::core::Fixed32::from_int(s.rules.consequences.tonsure_opinion));
}

TEST_CASE("marriage tonsure: scenario remarriage preserves an older record awaiting widowhood") {
    Scene s;
    const auto id = s.wed();
    REQUIRE(s.world.die(s.groom, CauseOfDeath::Illness));
    const auto previous = *s.world.find_family_marriage(id);
    REQUIRE(previous.state == family::MarriageState::Active);
    const auto husband = s.person(s.third_house, Sex::Male);
    REQUIRE(s.world.set_dynasty_head(s.third_house, husband));
    REQUIRE(s.world.marry(husband, s.bride));
    const auto prior_opinion = s.world.get_opinion(s.other_head, s.head);
    const auto spouse_opinion = s.world.get_opinion(husband, s.head);
    s.execute(Command{s.groom_house, cmd::Tonsure{s.bride}});
    CHECK(*s.world.find_family_marriage(id) == previous);
    CHECK_FALSE(s.world.characters().spouse[husband.index()].valid());
    CHECK(s.world.is_tonsured(s.bride));
    CHECK(s.world.get_opinion(s.other_head, s.head) == prior_opinion);
    CHECK(s.world.get_opinion(husband, s.head) ==
          spouse_opinion - pat::core::Fixed32::from_int(s.rules.consequences.tonsure_opinion));
}

TEST_CASE("marriage tonsure: current membership grants authority, original marriage houses do not") {
    Scene s;
    SUBCASE("either current spouse's house may act") {
        REQUIRE(s.world.marry(s.groom, s.bride));
        s.execute(Command{s.bride_house, cmd::Tonsure{s.groom}});
    }
    SUBCASE("a transferred bride's old house cannot act") {
        (void)s.wed();
        s.reject(Command{s.bride_house, cmd::Tonsure{s.bride}}, code(MarriageDenial::NotYourSpouse));
        s.execute(Command{s.groom_house, cmd::Tonsure{s.bride}});
    }
    SUBCASE("an unrelated house cannot act") {
        REQUIRE(s.world.marry(s.groom, s.bride));
        s.reject(Command{s.third_house, cmd::Tonsure{s.bride}}, code(MarriageDenial::NotYourSpouse));
    }
}

TEST_CASE("marriage tonsure: unmarried children and relatives preserve genealogy and disownment") {
    Scene s;
    const auto child = s.person(s.groom_house, Sex::Male, Date::from_ymd(1698, 1, 1), s.groom);
    family::PersonStatus status;
    status.character = child;
    status.flags = family::kPersonDisowned;
    status.disowned_on = s.world.today();
    REQUIRE(s.world.set_family_status(status));
    const auto opinion = s.world.get_opinion(s.other_head, s.head);
    s.execute(Command{s.groom_house, cmd::Tonsure{child}});
    CHECK(s.world.characters().father[child.index()] == s.groom);
    REQUIRE(s.world.family_status(child) != nullptr);
    CHECK(s.world.family_status(child)->flags == family::kPersonStatusFlags);
    CHECK(s.world.family_status(child)->disowned_on == status.disowned_on);
    CHECK(s.world.get_opinion(s.other_head, s.head) == opinion);
    CHECK(s.world.family().marriages.empty());
    REQUIRE(s.world.die(child, CauseOfDeath::Illness));
    CHECK(s.world.is_archived(child));
    CHECK(s.world.is_tonsured(child));
    CHECK(s.world.check_invariants().empty());
    s.reject(Command{s.groom_house, cmd::Tonsure{child}}, code(MarriageDenial::NotAlive));
}

TEST_CASE("marriage tonsure: rejects absent authority, heads, dead and missing targets atomically") {
    Scene s;
    SUBCASE("head") {
        s.reject(Command{s.groom_house, cmd::Tonsure{s.head}}, code(MarriageDenial::CannotTonsureHead));
    }
    SUBCASE("other house's head") {
        REQUIRE(s.world.marry(s.groom, s.other_head));
        s.reject(Command{s.groom_house, cmd::Tonsure{s.other_head}}, code(MarriageDenial::CannotTonsureHead));
    }
    SUBCASE("no acting head") {
        REQUIRE(s.world.set_dynasty_head(s.groom_house, {}));
        s.reject(Command{s.groom_house, cmd::Tonsure{s.groom}}, code(MarriageDenial::NoActingHead));
    }
    SUBCASE("system") {
        s.reject(Command{{}, cmd::Tonsure{s.groom}});
    }
    SUBCASE("unknown actor") {
        s.reject(Command{DynastyId(999), cmd::Tonsure{s.groom}});
    }
    SUBCASE("unknown target") {
        s.reject(Command{s.groom_house, cmd::Tonsure{CharacterId(999)}}, code(MarriageDenial::NotAlive));
    }
    SUBCASE("dead target") {
        REQUIRE(s.world.die(s.groom, CauseOfDeath::Illness));
        s.reject(Command{s.groom_house, cmd::Tonsure{s.groom}}, code(MarriageDenial::NotAlive));
    }
    SUBCASE("not dynasty mode") {
        World bare(s.world.today(), 299);
        CHECK_FALSE(validate(bare, Command{{}, cmd::Tonsure{}}, s.content()).has_value());
    }
}

TEST_CASE("marriage tonsure: preserves active regency, dispute and land roles on refusal") {
    Scene s;
    SUBCASE("actor regency") {
        const auto child = s.person(s.groom_house, Sex::Male, Date::from_ymd(1698, 1, 1));
        REQUIRE(s.world.set_dynasty_head(s.groom_house, child));
        family::Regency row;
        row.dynasty = s.groom_house;
        row.ward = child;
        row.regent = s.head;
        row.kind = family::RegencyKind::Guardian;
        row.started = s.world.today();
        REQUIRE(s.world.set_family_regency(row));
        s.reject(Command{s.groom_house, cmd::Tonsure{s.groom}}, 4);
    }
    SUBCASE("foreign mother's regency") {
        const auto child = s.person(s.bride_house, Sex::Male, Date::from_ymd(1698, 1, 1), {}, s.bride);
        REQUIRE(s.world.set_dynasty_head(s.bride_house, child));
        REQUIRE(s.world.marry(s.groom, s.bride));
        family::Regency row;
        row.dynasty = s.bride_house;
        row.ward = child;
        row.regent = s.bride;
        row.started = s.world.today();
        REQUIRE(s.world.set_family_regency(row));
        s.reject(Command{s.groom_house, cmd::Tonsure{s.bride}});
    }
    SUBCASE("dispute claimant") {
        REQUIRE(s.world.die(s.head, CauseOfDeath::Illness));
        REQUIRE(s.world.set_dynasty_head(s.groom_house, {}));
        const auto second = s.person(s.groom_house, Sex::Male);
        family::Dispute row;
        row.dynasty = s.groom_house;
        row.predecessor = s.head;
        row.opened = s.world.today();
        row.resolve_by = s.world.today() + 30;
        row.claimants_n = 2;
        row.claimants[0] = {s.groom, 20};
        row.claimants[1] = {second, 10};
        REQUIRE(s.world.set_family_dispute(row));
        s.reject(Command{s.groom_house, cmd::Tonsure{s.groom}});
    }
    SUBCASE("land holder") {
        const auto province = s.world.create_province({}, s.groom_house, pat::core::Fixed32::from_int(100));
        family::LandGrant row;
        row.province = province;
        row.holder = s.groom;
        row.granted_on = s.world.today();
        REQUIRE(s.world.set_family_land_grant(row));
        s.reject(Command{s.groom_house, cmd::Tonsure{s.groom}});
    }
}

TEST_CASE("marriage tonsure: removes engagements and heir choice but preserves guardian and law history") {
    Scene s;
    family::SuccessionState choice;
    choice.dynasty = s.groom_house;
    choice.designated_heir = s.groom;
    choice.guardian = s.groom;
    choice.law_changed_on = s.world.today();
    REQUIRE(s.world.set_family_succession(choice));
    SUBCASE("negotiation") {
        s.execute(Command{s.groom_house, cmd::SendMarriageEnvoy{s.groom, s.bride, 0}});
    }
    SUBCASE("betrothal") {
        s.execute(Command{{}, cmd::Betroth{s.groom, s.bride, 0, s.bride_house, false}});
    }
    s.execute(Command{s.groom_house, cmd::Tonsure{s.groom}});
    CHECK(s.world.engagement_of(s.groom) == 0);
    CHECK(s.world.engagement_of(s.bride) == 0);
    CHECK(s.world.family().negotiations.empty());
    CHECK(s.world.family().betrothals.empty());
    REQUIRE(s.world.family_succession(s.groom_house) != nullptr);
    CHECK_FALSE(s.world.family_succession(s.groom_house)->designated_heir.valid());
    CHECK(s.world.family_succession(s.groom_house)->guardian == s.groom);
    CHECK(s.world.family_succession(s.groom_house)->law_changed_on == choice.law_changed_on);
    CHECK_FALSE(s.world.set_dynasty_head(s.groom_house, s.groom));
    s.reject(Command{{}, cmd::SetDynastyHead{s.groom_house, s.groom}});
    CHECK_FALSE(s.world.can_marry(s.groom, s.bride));
    s.reject(Command{{}, cmd::Marry{s.groom, s.bride}});
    const auto denials = check_marriage(s.world, s.rules, s.groom, s.bride);
    CHECK(std::ranges::find(denials, MarriageDenial::Tonsured) != denials.end());
    CHECK(fallback_successor(s.world, s.groom_house, s.head) != s.groom);
    CHECK_FALSE(s.world.set_family_succession(choice));
}

TEST_CASE("marriage tonsure: releases province administration and blocks reappointment") {
    Scene s;
    const auto province = s.world.create_province({}, s.groom_house, pat::core::Fixed32::from_int(100));
    REQUIRE(s.world.enable_economy());
    REQUIRE(s.world.set_province_administrator(province, s.groom));
    s.execute(Command{s.groom_house, cmd::Tonsure{s.groom}});
    CHECK_FALSE(s.world.economy().provinces.administrator[province.index()].valid());
    CHECK_FALSE(s.world.set_province_administrator(province, s.groom));
    s.reject(Command{s.groom_house, cmd::AssignProvinceAdministrator{province, s.groom}});
}

TEST_CASE("marriage tonsure: command tag 69 round trips and rejects truncation") {
    const Command command{DynastyId(2), cmd::Tonsure{CharacterId(123)}};
    pat::core::ByteWriter writer;
    write_command(writer, command);
    CHECK(writer.data().front() == 69);
    CHECK(type_of(command) == CommandType::Tonsure);
    CHECK(to_string(type_of(command)) == "Tonsure");
    pat::core::ByteReader reader(writer.data());
    Command decoded;
    REQUIRE(read_command(reader, decoded));
    CHECK(decoded == command);
    CHECK(reader.at_end());
    for (std::size_t size = 0; size < writer.data().size(); ++size) {
        pat::core::ByteReader truncated(std::span<const std::uint8_t>(writer.data().data(), size));
        CHECK_FALSE(read_command(truncated, decoded));
    }
}

TEST_CASE("marriage tonsure: saves before and after tonsure and replay preserve the hash") {
    Scene s;
    REQUIRE(s.world.enable_economy());
    REQUIRE(s.world.set_initial_treasury(AccountRef::of(s.bride_house), 1000));
    (void)s.wed();
    const World initial = s.world;
    auto rules = std::make_shared<MarriageRules>(s.rules);
    Simulation sim(initial);
    install_family_systems(sim, rules);
    ReplayRecorder recorder(299, 809, initial.today(), 1);
    auto saved = deserialize_save(serialize_save(initial, {}));
    REQUIRE(saved.has_value());
    Simulation resumed(std::move(saved.value().world));
    install_family_systems(resumed, rules);
    const Command command{s.groom_house, cmd::Tonsure{s.bride}};
    REQUIRE(sim.submit(command));
    REQUIRE(resumed.submit(command));
    (void)sim.tick();
    (void)resumed.tick();
    recorder.after_tick(sim);
    CHECK(sim.world().is_tonsured(s.bride));
    CHECK(resumed.world().compute_hash() == sim.world().compute_hash());
    auto after = deserialize_save(serialize_save(sim.world(), sim.pending_internal()));
    REQUIRE(after.has_value());
    CHECK(after.value().world.compute_hash() == sim.world().compute_hash());
    REQUIRE(after.value().world.die(s.bride, CauseOfDeath::Illness));
    auto archived = deserialize_save(serialize_save(after.value().world, {}));
    REQUIRE(archived.has_value());
    CHECK(archived.value().world.is_archived(s.bride));
    CHECK(archived.value().world.is_tonsured(s.bride));
    auto replay = deserialize_replay(serialize_replay(recorder.finish(sim)));
    REQUIRE(replay.has_value());
    Simulation playback(initial);
    install_family_systems(playback, rules);
    const auto result = play_replay(playback, replay.value());
    CHECK(result.ok);
    CHECK(result.error.empty());
    CHECK(playback.world().compute_hash() == sim.world().compute_hash());
}
