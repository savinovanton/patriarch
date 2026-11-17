#include "pat/sim/command_log.hpp"
#include "pat/sim/marriage_envoy.hpp"
#include "pat/sim/marriage_score.hpp"
#include "pat/sim/replay.hpp"
#include "pat/sim/save.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <span>
#include <vector>

#include "marriage_test_fixture.hpp"

namespace {

using namespace pat::sim;
using Denial = MarriageDenial;
using economy::AccountRef;
using economy::LedgerCode;

struct Scene : pat::test::marriage::Scene {
    MarriageRules rules = MarriageRules::defaults();

    Scene() {
        REQUIRE(world.set_dynasty_head(groom_house, groom));
        REQUIRE(world.set_dynasty_head(bride_house, bride));
    }

    Command send(DynastyId actor) const { return Command{actor, cmd::SendMarriageEnvoy{groom, bride, 0}}; }

    CommandContent content() const { return {nullptr, nullptr, &rules, nullptr, nullptr}; }

    void execute(const Command& command) {
        REQUIRE(validate(world, command, content()).has_value());
        apply(world, command, content());
        CHECK(world.check_invariants().empty());
    }

    void reject(const Command& command, Denial denial) {
        const auto before = world.compute_hash();
        const auto family_before = world.family();
        const auto result = validate(world, command, content());
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == static_cast<std::uint16_t>(denial));
        CommandLog log;
        const std::array commands{command};
        const auto stats = process_commands(world, commands, CommandSource::External, log, content());
        CHECK(stats.applied == 0);
        CHECK(stats.rejected == 1);
        CHECK(world.compute_hash() == before);
        CHECK(world.family() == family_before);
    }
};

bool contains(const MarriageDenials& reasons, Denial reason) {
    return std::find(reasons.begin(), reasons.end(), reason) != reasons.end();
}

}  // namespace

TEST_CASE("marriage envoy: send and withdraw persist complete rows without economy") {
    Scene scene;
    scene.rules.negotiation.answer_days = 17;
    scene.execute(scene.send(scene.bride_house));
    REQUIRE(scene.world.family().negotiations.size() == 1);
    const auto row = scene.world.family().negotiations.front();
    CHECK(row.id == 1);
    CHECK(row.initiator == scene.bride_house);
    CHECK(row.groom == scene.groom);
    CHECK(row.bride == scene.bride);
    CHECK(row.groom_house == scene.groom_house);
    CHECK(row.bride_house == scene.bride_house);
    CHECK(row.dowry_source == scene.bride_house);
    CHECK(row.sent == scene.world.today());
    CHECK(row.answer_due == row.sent + 17);
    CHECK(row.cost_kop == 0);
    CHECK(row.dowry_kop == standard_dowry(scene.world, scene.rules, scene.bride_house));
    CHECK(row.state == family::NegotiationState::Pending);
    CHECK(scene.world.family().next_negotiation_id == 2);
    const auto sent_notes = scene.world.chronicle(scene.bride_house);
    const auto sent_note = [](const ChronicleEntry& entry) {
        return entry.kind == ChronicleKind::Note && entry.param == 201;
    };
    REQUIRE(std::count_if(sent_notes.begin(), sent_notes.end(), sent_note) == 1);
    const auto note = std::find_if(sent_notes.begin(), sent_notes.end(), sent_note);
    CHECK(note->a == scene.groom);
    CHECK(note->b == scene.bride);
    const auto notes_before = scene.world.chronicle(scene.bride_house).size();
    scene.reject(Command{scene.groom_house, cmd::WithdrawMarriageEnvoy{row.id}}, Denial::NotInitiator);
    scene.execute(Command{scene.bride_house, cmd::WithdrawMarriageEnvoy{row.id}});
    CHECK(scene.world.family().negotiations.empty());
    REQUIRE(scene.world.family().outcomes.size() == 1);
    const auto& outcome = scene.world.family().outcomes.front();
    CHECK(outcome.negotiation == row.id);
    CHECK(outcome.initiator == scene.bride_house);
    CHECK(outcome.responder == scene.groom_house);
    CHECK(outcome.result == family::NegotiationResult::Withdrawn);
    CHECK(outcome.reason == 0);
    CHECK(outcome.score == 0);
    CHECK(std::all_of(outcome.terms.begin(), outcome.terms.end(), [](auto value) { return value == 0; }));
    CHECK(scene.world.chronicle(scene.bride_house).size() == notes_before);
    scene.reject(Command{scene.bride_house, cmd::WithdrawMarriageEnvoy{row.id}}, Denial::NegotiationNotFound);
}

TEST_CASE("marriage envoy: denials preserve all shared eligibility reasons and state") {
    Scene scene;
    SUBCASE("unknown") {
        scene.reject(Command{scene.groom_house, cmd::SendMarriageEnvoy{CharacterId(99999), scene.bride}},
                     Denial::NotAlive);
        const auto reasons = can_send_envoy(
            scene.world, scene.rules, scene.groom_house, CharacterId(99999), scene.bride, false);
        CHECK_FALSE(contains(reasons, Denial::TableFull));
    }
    SUBCASE("unauthorized") {
        scene.reject(scene.send(scene.third_house), Denial::NotYourMember);
    }
    SUBCASE("unknown actor keeps the domain refusal code") {
        const DynastyId unknown_house(99999);
        scene.reject(scene.send(unknown_house), Denial::NotYourMember);
        scene.execute(scene.send(scene.groom_house));
        const auto id = scene.world.family().negotiations.front().id;
        scene.reject(Command{unknown_house, cmd::WithdrawMarriageEnvoy{id}}, Denial::NotInitiator);
    }
    SUBCASE("shared reasons") {
        const auto head = scene.person(scene.groom_house, Sex::Male);
        REQUIRE(scene.world.set_dynasty_head(scene.groom_house, head));
        family::PersonStatus status;
        status.character = scene.groom;
        status.flags = family::kPersonTonsured | family::kPersonDisowned;
        status.tonsured_on = scene.world.today();
        status.disowned_on = scene.world.today();
        REQUIRE(scene.world.set_family_status(status));
        const MarriageCheckOptions options{true, false, scene.groom_house};
        const auto shared = check_marriage(scene.world, scene.rules, scene.groom, scene.bride, options);
        const auto envoy =
            can_send_envoy(scene.world, scene.rules, scene.groom_house, scene.groom, scene.bride, false);
        for (const auto denial : shared) {
            CHECK(contains(envoy, denial));
        }
        scene.reject(scene.send(scene.groom_house), Denial::Tonsured);
    }
    SUBCASE("archive") {
        REQUIRE(scene.world.die(scene.bride, CauseOfDeath::Illness));
        scene.reject(scene.send(scene.groom_house), Denial::NotAlive);
    }
    SUBCASE("duplicate and cap") {
        scene.rules.negotiation.max_parallel_negotiations = 1;
        scene.execute(scene.send(scene.groom_house));
        const auto reasons =
            can_send_envoy(scene.world, scene.rules, scene.groom_house, scene.groom, scene.bride, false);
        CHECK(contains(reasons, Denial::DuplicateNegotiation));
        CHECK(contains(reasons, Denial::HouseBusy));
        scene.reject(scene.send(scene.groom_house), Denial::InNegotiation);
        const auto other_groom = scene.person(scene.groom_house, Sex::Male);
        const auto other_bride = scene.person(scene.third_house, Sex::Female);
        scene.reject(Command{scene.groom_house, cmd::SendMarriageEnvoy{other_groom, other_bride}},
                     Denial::HouseBusy);
    }
    SUBCASE("refusal cooldown") {
        scene.execute(scene.send(scene.groom_house));
        const auto issued = scene.world.family().negotiations.front().id;
        family::NegotiationOutcome refused;
        refused.negotiation = issued;
        refused.initiator = scene.groom_house;
        refused.responder = scene.bride_house;
        refused.groom = scene.groom;
        refused.bride = scene.bride;
        refused.sent = scene.world.today();
        refused.answered = scene.world.today();
        refused.result = family::NegotiationResult::Refused;
        refused.threshold = 1;
        refused.reason = 1;
        REQUIRE(scene.world.append_family_outcome(refused).has_value());
        REQUIRE(scene.world.close_family_negotiation(issued));
        scene.reject(scene.send(scene.bride_house), Denial::RecentlyRefused);
        scene.rules.negotiation.refusal_cooldown_days = 0;
        scene.execute(scene.send(scene.bride_house));
    }
    SUBCASE("counter exhausted") {
        const_cast<family::State&>(scene.world.family()).next_negotiation_id =
            std::numeric_limits<family::NegotiationId>::max();
        scene.reject(scene.send(scene.groom_house), Denial::TableFull);
    }
    SUBCASE("withdrawal outcome counter exhausted") {
        scene.execute(scene.send(scene.groom_house));
        const_cast<family::State&>(scene.world.family()).next_outcome_seq =
            std::numeric_limits<family::OutcomeSequence>::max();
        scene.reject(Command{scene.groom_house, cmd::WithdrawMarriageEnvoy{1}}, Denial::TableFull);
    }
}

TEST_CASE("marriage envoy: expense and bounded refund affect only envoy ledger line") {
    Scene scene;
    REQUIRE(scene.world.enable_economy());
    const auto payer = AccountRef::of(scene.groom_house);
    const auto cost = envoy_cost(scene.rules, true);
    const Command command{scene.groom_house, cmd::SendMarriageEnvoy{scene.groom, scene.bride, 1}};
    SUBCASE("insufficient treasury") {
        REQUIRE(scene.world.set_initial_treasury(payer, cost - 1));
        scene.reject(command, Denial::InsufficientFunds);
        return;
    }
    REQUIRE(scene.world.set_initial_treasury(payer, cost));
    scene.rules.negotiation.withdraw_refund_permille = 333;
    SUBCASE("no refund") {
        scene.rules.negotiation.withdraw_refund_permille = 0;
    }
    SUBCASE("full refund") {
        scene.rules.negotiation.withdraw_refund_permille = 1000;
    }
    scene.execute(command);
    const auto& accounts = scene.world.economy().accounts;
    const auto payer_row = *scene.world.resolve_account(payer);
    CHECK(scene.world.treasury(payer) == 0);
    CHECK(accounts.open_line(payer_row, LedgerCode::MarriageEnvoy) == -cost);
    for (const auto code : economy::kLedgerCodes) {
        if (code != LedgerCode::MarriageEnvoy) {
            CHECK(accounts.open_line(payer_row, code) == 0);
        }
    }
    CHECK(scene.world.family().negotiations.front().cost_kop == cost);
    CHECK(scene.world.family().negotiations.front().dispensation == 1);
    scene.execute(Command{scene.groom_house, cmd::WithdrawMarriageEnvoy{1}});
    const auto refund = cost * scene.rules.negotiation.withdraw_refund_permille / 1000;
    CHECK(scene.world.treasury(payer) == refund);
    CHECK(accounts.open_line(payer_row, LedgerCode::MarriageEnvoy) == -cost + refund);
    CHECK(refund <= cost);
}

TEST_CASE("marriage envoy: system authority derives initiator and bypasses absent head") {
    Scene scene;
    REQUIRE(scene.world.set_dynasty_head(scene.groom_house, CharacterId()));
    scene.reject(scene.send(scene.groom_house), Denial::NoActingHead);
    scene.execute(scene.send(DynastyId()));
    CHECK(scene.world.family().negotiations.front().initiator == scene.groom_house);
    scene.execute(Command{DynastyId(), cmd::WithdrawMarriageEnvoy{1}});
}

TEST_CASE("marriage envoy: reversed command inputs retain canonical parties and system payer") {
    Scene scene;
    REQUIRE(scene.world.enable_economy());
    const auto cost = envoy_cost(scene.rules, false);
    REQUIRE(scene.world.set_initial_treasury(AccountRef::of(scene.groom_house), cost));
    scene.execute(Command{DynastyId(), cmd::SendMarriageEnvoy{scene.bride, scene.groom, 0}});
    REQUIRE(scene.world.family().negotiations.size() == 1);
    const auto& row = scene.world.family().negotiations.front();
    CHECK(row.groom == scene.groom);
    CHECK(row.bride == scene.bride);
    CHECK(row.initiator == scene.groom_house);
    CHECK(row.dowry_source == scene.bride_house);
    CHECK(row.cost_kop == cost);
    CHECK(scene.world.treasury(AccountRef::of(scene.groom_house)) == 0);
    CHECK(scene.world.treasury(AccountRef::of(scene.bride_house)) == 0);
    const auto notes = scene.world.chronicle(scene.groom_house);
    const auto sent_note = [](const ChronicleEntry& entry) {
        return entry.kind == ChronicleKind::Note && entry.param == 201;
    };
    REQUIRE(std::count_if(notes.begin(), notes.end(), sent_note) == 1);
    const auto note = std::find_if(notes.begin(), notes.end(), sent_note);
    CHECK(note->a == scene.groom);
    CHECK(note->b == scene.bride);
}

TEST_CASE("marriage envoy: pending negotiation can be withdrawn after answer date") {
    Scene scene;
    scene.rules.negotiation.answer_days = 1;
    scene.execute(scene.send(scene.groom_house));
    (void)scene.world.advance_day();
    (void)scene.world.advance_day();
    scene.execute(Command{scene.groom_house, cmd::WithdrawMarriageEnvoy{1}});
    REQUIRE(scene.world.family().outcomes.size() == 1);
    CHECK(scene.world.family().outcomes.front().answered == scene.world.today());
}

TEST_CASE("marriage envoy: regent imperial ward and dispute caretaker have authority") {
    Scene scene;
    const auto ward = scene.person(scene.groom_house, Sex::Male, Date::from_ymd(1695, 1, 1));
    SUBCASE("council regent") {
        REQUIRE(scene.world.set_dynasty_head(scene.groom_house, ward));
        family::Regency regency;
        regency.dynasty = scene.groom_house;
        regency.ward = ward;
        regency.regent = scene.groom;
        regency.kind = family::RegencyKind::Council;
        regency.started = scene.world.today();
        REQUIRE(scene.world.set_family_regency(regency));
        CHECK(family_acting_head(scene.world, scene.groom_house) == scene.groom);
    }
    SUBCASE("imperial ward") {
        REQUIRE(scene.world.set_dynasty_head(scene.groom_house, ward));
        family::Regency regency;
        regency.dynasty = scene.groom_house;
        regency.ward = ward;
        regency.kind = family::RegencyKind::Imperial;
        regency.started = scene.world.today();
        REQUIRE(scene.world.set_family_regency(regency));
        CHECK(family_acting_head(scene.world, scene.groom_house) == ward);
    }
    SUBCASE("caretaker") {
        const auto predecessor = scene.person(scene.groom_house, Sex::Male);
        REQUIRE(scene.world.die(predecessor, CauseOfDeath::Illness));
        REQUIRE(scene.world.set_dynasty_head(scene.groom_house, {}));
        family::Dispute dispute;
        dispute.dynasty = scene.groom_house;
        dispute.predecessor = predecessor;
        dispute.opened = scene.world.today();
        dispute.resolve_by = scene.world.today() + 90;
        dispute.claimants_n = 2;
        dispute.claimants[0] = {scene.groom, 30};
        dispute.claimants[1] = {ward, 20};
        REQUIRE(scene.world.set_family_dispute(dispute));
        CHECK(family_acting_head(scene.world, scene.groom_house) == scene.groom);
    }
    scene.execute(scene.send(scene.groom_house));
}

TEST_CASE("marriage envoy: configured content survives tick save continuation and replay") {
    Scene scene;
    auto rules = std::make_shared<MarriageRules>(scene.rules);
    rules->negotiation.answer_days = 9;
    rules->money.dowry_base_kop = 12345;
    rules->money.dowry_min_kop = 12345;
    rules->money.dowry_max_kop = 12345;
    SimulationConfig config;
    config.marriage_rules = rules;
    const World initial = scene.world;
    Simulation live(initial, config);
    ReplayRecorder recorder(299, 811, initial.today(), 1);
    REQUIRE(live.submit(scene.send(scene.groom_house)));
    (void)live.tick();
    recorder.after_tick(live);
    REQUIRE(live.world().family().negotiations.size() == 1);
    CHECK(live.world().family().negotiations.front().dowry_kop == 12345);
    CHECK(live.world().family().negotiations.front().answer_due == live.world().today() + 9);
    auto loaded = deserialize_save(serialize_save(live.world(), live.pending_internal()));
    REQUIRE(loaded.has_value());
    CHECK(loaded.value().world.compute_hash() == live.world().compute_hash());
    Simulation continued(std::move(loaded.value().world), config);
    REQUIRE(live.submit(Command{scene.groom_house, cmd::WithdrawMarriageEnvoy{1}}));
    REQUIRE(continued.submit(Command{scene.groom_house, cmd::WithdrawMarriageEnvoy{1}}));
    (void)live.tick();
    (void)continued.tick();
    recorder.after_tick(live);
    CHECK(continued.world().compute_hash() == live.world().compute_hash());
    const auto replay = recorder.finish(live);
    Simulation playback(initial, config);
    const auto result = play_replay(playback, replay);
    CHECK(result.ok);
    CHECK(result.error.empty());
    CHECK(playback.world().compute_hash() == live.world().compute_hash());
}

TEST_CASE("marriage envoy: configured faith reaches simulation validation") {
    Scene scene;
    const std::array<std::string_view, 3> faiths{"orthodox", "other", "orthodox"};
    SimulationConfig config;
    config.faith_policy = std::make_shared<FaithPolicy>(faiths);
    Simulation sim(scene.world, config);
    REQUIRE(sim.submit(scene.send(scene.groom_house)));
    CHECK(sim.tick().external_commands.rejected == 1);
    CHECK(sim.world().family().negotiations.empty());
}

TEST_CASE("marriage envoy: wire tags flags and truncation remain stable") {
    const std::array commands{
        Command{DynastyId(2), cmd::SendMarriageEnvoy{CharacterId(7), CharacterId(8), 1}},
        Command{DynastyId(2), cmd::WithdrawMarriageEnvoy{42}}};
    for (std::size_t i = 0; i < commands.size(); ++i) {
        pat::core::ByteWriter writer;
        write_command(writer, commands[i]);
        CHECK(writer.data().front() == static_cast<std::uint8_t>(64 + i));
        Command decoded;
        pat::core::ByteReader reader(writer.data());
        REQUIRE(read_command(reader, decoded));
        CHECK(decoded == commands[i]);
        CHECK(reader.at_end());
        for (std::size_t size = 0; size < writer.data().size(); ++size) {
            pat::core::ByteReader truncated(std::span<const std::uint8_t>(writer.data().data(), size));
            CHECK_FALSE(read_command(truncated, decoded));
        }
    }
    pat::core::ByteWriter writer;
    write_command(writer, commands.front());
    auto bad = writer.data();
    bad.back() = 2;
    pat::core::ByteReader reader(bad);
    Command decoded;
    CHECK_FALSE(read_command(reader, decoded));
    Scene scene;
    const auto invalid = validate(
        scene.world, Command{scene.groom_house, cmd::SendMarriageEnvoy{scene.groom, scene.bride, 2}});
    REQUIRE_FALSE(invalid.has_value());
    CHECK(invalid.error().reason == Rejection::InvalidArgument);
    CHECK(static_cast<int>(CommandType::AdjustHonor) == 5);
}
