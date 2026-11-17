#include "pat/sim/command_log.hpp"
#include "pat/sim/dynasty_relation.hpp"
#include "pat/sim/family_systems.hpp"
#include "pat/sim/marriage_envoy.hpp"
#include "pat/sim/marriage_negotiations.hpp"
#include "pat/sim/marriage_score.hpp"
#include "pat/sim/replay.hpp"
#include "pat/sim/save.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <numeric>
#include <span>

#include "marriage_test_fixture.hpp"

namespace {

using namespace pat::sim;
using Result = family::NegotiationResult;

struct Scene : pat::test::marriage::Scene {
    MarriageRules rules = MarriageRules::defaults();

    explicit Scene(Date today = Date::from_ymd(1703, 5, 20),
                   Date groom_birth = Date::from_ymd(1670, 1, 1),
                   Date bride_birth = Date::from_ymd(1670, 1, 1))
        : pat::test::marriage::Scene(today, groom_birth, bride_birth) {
        REQUIRE(world.set_dynasty_head(groom_house, groom));
        REQUIRE(world.set_dynasty_head(bride_house, bride));
    }

    CommandContent content(const FaithPolicy* faith = nullptr) const {
        return {nullptr, nullptr, &rules, nullptr, faith};
    }

    family::Negotiation send(DynastyId initiator, bool dispensation = false) {
        const Command command{initiator,
                              cmd::SendMarriageEnvoy{groom, bride, static_cast<std::uint8_t>(dispensation)}};
        REQUIRE(validate(world, command, content()).has_value());
        apply(world, command, content());
        REQUIRE(world.family().negotiations.size() == 1);
        return world.family().negotiations.front();
    }

    void advance_to(Date date) {
        while (world.today() < date) {
            (void)world.advance_day();
        }
    }

    Command answer(family::NegotiationId id, const FaithPolicy* faith = nullptr) {
        const auto payload = compute_marriage_response(world, rules, id, faith);
        REQUIRE(payload.has_value());
        return Command{DynastyId(), *payload};
    }

    void execute(const Command& command, const FaithPolicy* faith = nullptr) {
        REQUIRE(validate(world, command, content(faith)).has_value());
        apply(world, command, content(faith));
        CHECK(world.check_invariants().empty());
    }

    void reject(const Command& command) {
        const auto before = world.compute_hash();
        const auto family_before = world.family();
        CommandLog log;
        const std::array commands{command};
        const auto stats = process_commands(world, commands, CommandSource::Internal, log, content());
        CHECK(stats.applied == 0);
        CHECK(stats.rejected == 1);
        CHECK(world.compute_hash() == before);
        CHECK(world.family() == family_before);
    }
};

}  // namespace

TEST_CASE("marriage negotiations: response preserves the authoritative score and explanation") {
    for (const bool accepted : {false, true}) {
        CAPTURE(accepted);
        Scene scene;
        scene.rules.score.accept_threshold = accepted ? -1000 : 1000;
        const auto row = scene.send(scene.groom_house);
        scene.advance_to(row.answer_due);
        MatchContext context;
        context.initiator = row.initiator;
        context.relation = dynasty_relation(scene.world, row.groom_house, row.bride_house, scene.rules);
        const auto evaluation =
            evaluate_match(scene.world, scene.rules, row.groom, row.bride, row.dowry_kop, context);
        REQUIRE(evaluation.has_value());
        economy::Breakdown explanation;
        const auto& oracle = accept_score(*evaluation, MatchSide::Bride, &explanation);
        CHECK(explanation.is_consistent());
        for (std::size_t term = 0; term < oracle.terms.size(); ++term) {
            const auto node = explanation.find(score_term_code(static_cast<ScoreTerm>(term)));
            REQUIRE(node != economy::kNoNode);
            CHECK(explanation.node(node).value ==
                  static_cast<std::int64_t>(oracle.terms[term]) * pat::core::Fixed32::one_raw);
        }
        const auto before = scene.world.compute_hash();
        const Command response = scene.answer(row.id);
        CHECK(scene.world.compute_hash() == before);
        CHECK(scene.answer(row.id) == response);
        const auto& payload = std::get<cmd::RespondMarriage>(response.payload);
        CHECK(payload.result == (accepted ? Result::Accepted : Result::Refused));
        CHECK(payload.score == oracle.score);
        CHECK(payload.threshold == oracle.threshold);
        CHECK(payload.terms == oracle.terms);
        CHECK(payload.reason == oracle.refusal_code);
        CHECK(std::accumulate(payload.terms.begin(), payload.terms.end(), 0) == payload.score);
        CHECK(explanation.total() == pat::core::Fixed32::from_int(oracle.score).raw());
        scene.execute(response);
        CHECK(scene.world.family().negotiations.empty());
        REQUIRE(scene.world.family().outcomes.size() == 1);
        const auto& outcome = scene.world.family().outcomes.front();
        CHECK(outcome.result == payload.result);
        CHECK(outcome.responder == scene.bride_house);
        CHECK(outcome.sent == row.sent);
        CHECK(outcome.answered == scene.world.today());
        CHECK(outcome.score == oracle.score);
        CHECK(outcome.threshold == oracle.threshold);
        CHECK(outcome.terms == oracle.terms);
        CHECK(outcome.reason == oracle.refusal_code);
        const auto action = marriage_answered_action(outcome);
        REQUIRE(action.has_value());
        CHECK(action->initiator == row.initiator);
        CHECK(action->responder == scene.bride_house);
        CHECK(action->groom == row.groom);
        CHECK(action->bride == row.bride);
        CHECK(action->accepted == accepted);
        CHECK(action->reason == oracle.refusal_code);
        const auto notes = scene.world.chronicle(row.initiator);
        const auto answer_note = [accepted](const ChronicleEntry& entry) {
            return entry.kind == ChronicleKind::Note && entry.param == (accepted ? 202 : 203);
        };
        REQUIRE(std::count_if(notes.begin(), notes.end(), answer_note) == 1);
        const auto note = std::find_if(notes.begin(), notes.end(), answer_note);
        CHECK(note->a == row.groom);
        CHECK(note->b == row.bride);
        const auto engagement_note = [](const ChronicleEntry& entry) {
            return entry.kind == ChronicleKind::Note && entry.param == 204;
        };
        CHECK(std::count_if(notes.begin(), notes.end(), engagement_note) == (accepted ? 1 : 0));
        const auto delta = accepted ? scene.rules.consequences.marriage_accepted_opinion
                                    : -scene.rules.consequences.marriage_refused_opinion;
        CHECK(scene.world.get_opinion(scene.bride, scene.groom) == pat::core::Fixed32::from_int(delta));
        CHECK(scene.world.get_opinion(scene.groom, scene.bride) == pat::core::Fixed32{});
        const auto sources = scene.world.opinion_sources(scene.bride, scene.groom);
        REQUIRE(sources.size() == 1);
        CHECK(sources.front().reason ==
              (accepted ? kOpinionReasonMarriageAccepted : kOpinionReasonMarriageRefused));
        CHECK_FALSE(scene.world.characters().spouse[scene.groom.index()].valid());
        CHECK_FALSE(scene.world.characters().spouse[scene.bride.index()].valid());
        if (accepted) {
            REQUIRE(scene.world.family().betrothals.size() == 1);
            const auto& betrothal = scene.world.family().betrothals.front();
            CHECK(betrothal.origin == family::BetrothalOrigin::Negotiation);
            CHECK(betrothal.groom == row.groom);
            CHECK(betrothal.bride == row.bride);
            CHECK(betrothal.dowry_kop == row.dowry_kop);
            CHECK(betrothal.dowry_source == row.dowry_source);
            CHECK(scene.world.family_betrothal_of(scene.groom) != nullptr);
        } else {
            CHECK(scene.world.family().betrothals.empty());
            CHECK(scene.world.family_negotiation_of(scene.groom) == nullptr);
            CHECK(scene.world.family_negotiation_of(scene.bride) == nullptr);
        }
        scene.reject(response);
        CHECK_FALSE(compute_marriage_response(scene.world, scene.rules, row.id).has_value());
    }
}

TEST_CASE("marriage negotiations: changed eligibility voids without opinion or duplicate death outcomes") {
    Scene scene;
    const auto row = scene.send(scene.groom_house);
    SUBCASE("participant married elsewhere") {
        const auto other = scene.person(scene.third_house, Sex::Female);
        REQUIRE(scene.world.marry(scene.groom, other));
        CHECK(scene.world.family().negotiations.empty());
        REQUIRE(scene.world.family().outcomes.size() == 1);
        CHECK(scene.world.family().outcomes.front().result == Result::Void);
        CHECK(scene.world.family().outcomes.front().reason ==
              static_cast<std::uint16_t>(MarriageDenial::AlreadyMarried));
        CHECK_FALSE(compute_marriage_response(scene.world, scene.rules, row.id).has_value());
    }
    SUBCASE("participant tonsured") {
        const auto replacement_head = scene.person(scene.bride_house, Sex::Male);
        REQUIRE(scene.world.set_dynasty_head(scene.bride_house, replacement_head));
        family::PersonStatus status;
        status.character = scene.bride;
        status.flags = family::kPersonTonsured;
        status.tonsured_on = scene.world.today();
        REQUIRE(scene.world.set_family_status(status));
        scene.advance_to(row.answer_due);
        const auto response = scene.answer(row.id);
        CHECK(std::get<cmd::RespondMarriage>(response.payload).result == Result::Void);
        scene.execute(response);
        CHECK(scene.world.family().negotiations.empty());
        CHECK(scene.world.family().betrothals.empty());
        REQUIRE(scene.world.family().outcomes.size() == 1);
        CHECK(scene.world.get_opinion(scene.bride, scene.groom) == pat::core::Fixed32{});
    }
    SUBCASE("configured faith changes") {
        scene.advance_to(row.answer_due);
        const std::array<std::string_view, 3> faiths{"orthodox", "other", "orthodox"};
        const FaithPolicy faith(faiths);
        const auto response = scene.answer(row.id, &faith);
        CHECK(std::get<cmd::RespondMarriage>(response.payload).result == Result::Void);
        CHECK(std::get<cmd::RespondMarriage>(response.payload).reason ==
              static_cast<std::uint16_t>(MarriageDenial::FaithMismatch));
        scene.execute(response, &faith);
        CHECK(scene.world.get_opinion(scene.bride, scene.groom) == pat::core::Fixed32{});
    }
    SUBCASE("participant dies and its house becomes extinct") {
        REQUIRE(scene.world.die(scene.bride, CauseOfDeath::Illness));
        CHECK(scene.world.dynasty_extinct(scene.bride_house));
        CHECK(scene.world.family().negotiations.empty());
        REQUIRE(scene.world.family().outcomes.size() == 1);
        CHECK(scene.world.family().outcomes.front().result == Result::Void);
        CHECK_FALSE(marriage_answered_action(scene.world.family().outcomes.front()).has_value());
        CHECK_FALSE(compute_marriage_response(scene.world, scene.rules, row.id).has_value());
    }
}

TEST_CASE("marriage negotiations: unauthorized and inconsistent responses are atomic") {
    Scene scene;
    scene.rules.score.accept_threshold = -1000;
    const auto row = scene.send(scene.groom_house);
    scene.advance_to(row.answer_due);
    auto response = scene.answer(row.id);
    SUBCASE("house actor") {
        response.actor = scene.bride_house;
    }
    SUBCASE("score") {
        ++std::get<cmd::RespondMarriage>(response.payload).score;
    }
    SUBCASE("term") {
        ++std::get<cmd::RespondMarriage>(response.payload).terms[0];
    }
    SUBCASE("threshold") {
        auto& payload = std::get<cmd::RespondMarriage>(response.payload);
        payload.threshold = payload.score + 1;
    }
    SUBCASE("reason") {
        ++std::get<cmd::RespondMarriage>(response.payload).reason;
    }
    SUBCASE("result") {
        std::get<cmd::RespondMarriage>(response.payload).result = Result::Withdrawn;
    }
    SUBCASE("unknown negotiation") {
        std::get<cmd::RespondMarriage>(response.payload).negotiation = row.id + 1;
    }
    SUBCASE("betrothal counter exhausted") {
        const_cast<family::State&>(scene.world.family()).next_betrothal_id =
            std::numeric_limits<family::BetrothalId>::max();
    }
    SUBCASE("outcome counter exhausted") {
        const_cast<family::State&>(scene.world.family()).next_outcome_seq =
            std::numeric_limits<family::OutcomeSequence>::max();
    }
    SUBCASE("eligibility changed after emission") {
        const auto replacement_head = scene.person(scene.bride_house, Sex::Male);
        REQUIRE(scene.world.set_dynasty_head(scene.bride_house, replacement_head));
        family::PersonStatus status;
        status.character = scene.bride;
        status.flags = family::kPersonTonsured;
        status.tonsured_on = scene.world.today();
        REQUIRE(scene.world.set_family_status(status));
    }
    scene.reject(response);
}

TEST_CASE("marriage negotiations: response wire tag 66 rejects truncation and unsupported results") {
    cmd::RespondMarriage payload;
    payload.negotiation = 17;
    payload.result = Result::Refused;
    payload.reason = 10003;
    payload.score = -23;
    payload.threshold = 2;
    payload.terms = {-10, 0, -13, 0, 0, 0, 0, 0};
    const Command command{DynastyId(), payload};
    pat::core::ByteWriter writer;
    write_command(writer, command);
    CHECK(writer.data().front() == 66);
    Command decoded;
    pat::core::ByteReader reader(writer.data());
    REQUIRE(read_command(reader, decoded));
    CHECK(reader.at_end());
    CHECK(decoded == command);
    for (std::size_t size = 0; size < writer.data().size(); ++size) {
        pat::core::ByteReader truncated(std::span<const std::uint8_t>(writer.data().data(), size));
        CHECK_FALSE(read_command(truncated, decoded));
    }
    for (auto result :
         {Result::Accepted, Result::Refused, Result::Void, Result::Withdrawn, static_cast<Result>(255)}) {
        payload.result = result;
        pat::core::ByteWriter bytes;
        write_command(bytes, Command{DynastyId(), payload});
        pat::core::ByteReader input(bytes.data());
        CHECK(read_command(input, decoded) ==
              (result == Result::Accepted || result == Result::Refused || result == Result::Void));
    }
}

TEST_CASE("marriage negotiations: responder direction and dispensation effects do not transfer dowry") {
    Scene scene;
    scene.rules.score.accept_threshold = -1000;
    REQUIRE(scene.world.enable_economy());
    const auto cost = envoy_cost(scene.rules, true);
    REQUIRE(scene.world.set_initial_treasury(economy::AccountRef::of(scene.bride_house), cost + 98765));
    const auto row = scene.send(scene.bride_house, true);
    const auto treasury = scene.world.treasury(economy::AccountRef::of(scene.bride_house));
    const auto honor = scene.world.dynasties().honor[scene.bride_house.index()];
    scene.advance_to(row.answer_due);
    scene.execute(scene.answer(row.id));
    REQUIRE(scene.world.family().outcomes.size() == 1);
    CHECK(scene.world.family().outcomes.front().responder == scene.groom_house);
    CHECK(scene.world.get_opinion(scene.groom, scene.bride) ==
          pat::core::Fixed32::from_int(scene.rules.consequences.marriage_accepted_opinion));
    CHECK(scene.world.dynasties().honor[scene.bride_house.index()] ==
          honor - scene.rules.consequences.dispensation_honor);
    CHECK(scene.world.treasury(economy::AccountRef::of(scene.bride_house)) == treasury);
    CHECK(scene.world.treasury(economy::AccountRef::of(scene.groom_house)) == 0);
    REQUIRE(scene.world.family().betrothals.size() == 1);
    CHECK(scene.world.family().betrothals.front().dispensation == 1);
    CHECK(scene.world.family().marriages.empty());
}

TEST_CASE("marriage negotiations: daily answers cross calendar boundaries exactly once") {
    const std::array due_dates{Date::from_ymd(1703, 1, 28),
                               Date::from_ymd(1703, 1, 29),
                               Date::from_ymd(1703, 1, 30),
                               Date::from_ymd(1703, 1, 31),
                               Date::from_ymd(1703, 2, 28),
                               Date::from_ymd(1704, 2, 29),
                               Date::from_ymd(1700, 2, 29)};
    for (const auto due : due_dates) {
        CAPTURE(due.days());
        Scene scene(due - 2);
        scene.rules.negotiation.answer_days = 2;
        scene.rules.score.accept_threshold = -1000;
        const auto row = scene.send(scene.groom_house);
        CHECK(row.answer_due == due);
        auto rules = std::make_shared<MarriageRules>(scene.rules);
        Simulation sim(scene.world);
        install_family_systems(sim, rules);
        (void)sim.tick();
        CHECK(sim.pending_internal().empty());
        CHECK(sim.world().family().outcomes.empty());
        (void)sim.tick();
        CHECK(sim.world().today() == due);
        REQUIRE(sim.pending_internal().size() == 1);
        CHECK(type_of(sim.pending_internal().front()) == CommandType::RespondMarriage);
        CHECK(sim.world().family().outcomes.empty());
        (void)sim.tick();
        CHECK(sim.world().today() == due + 1);
        REQUIRE(sim.world().family().outcomes.size() == 1);
        CHECK(sim.world().family().outcomes.front().answered == due + 1);
        CHECK(sim.pending_internal().empty());
        sim.run_days(2);
        CHECK(sim.world().family().outcomes.size() == 1);
        CHECK(sim.world().family().betrothals.size() == 1);
    }
}

TEST_CASE("marriage negotiations: default simulation leaves family systems uninstalled") {
    Scene scene;
    scene.rules.negotiation.answer_days = 1;
    (void)scene.send(scene.groom_house);
    Simulation sim(scene.world);
    CHECK(sim.system_count() == 0);
    sim.run_days(5);
    CHECK(sim.world().family().negotiations.size() == 1);
    CHECK(sim.world().family().outcomes.empty());
    CHECK(sim.pending_internal().empty());
}

TEST_CASE("marriage negotiations: save during waiting and pending answer replays configured rules") {
    Scene scene;
    scene.rules.negotiation.answer_days = 3;
    scene.rules.score.accept_threshold = -1000;
    scene.rules.money.dowry_base_kop = 12345;
    scene.rules.money.dowry_min_kop = 12345;
    scene.rules.money.dowry_max_kop = 12345;
    auto rules = std::make_shared<MarriageRules>(scene.rules);
    const World initial = scene.world;
    Simulation sim(initial);
    install_family_systems(sim, rules);
    ReplayRecorder recorder(299, 809, initial.today(), 1);
    REQUIRE(sim.submit(Command{scene.groom_house, cmd::SendMarriageEnvoy{scene.groom, scene.bride, 0}}));
    (void)sim.tick();
    recorder.after_tick(sim);
    REQUIRE(sim.world().family().negotiations.size() == 1);
    CHECK(sim.world().family().negotiations.front().dowry_kop == 12345);
    auto waiting = deserialize_save(serialize_save(sim.world(), sim.pending_internal()));
    REQUIRE(waiting.has_value());
    Simulation from_wait(initial);
    install_family_systems(from_wait, rules);
    from_wait.restore_state(std::move(waiting.value().world), std::move(waiting.value().pending_internal));
    for (int day = 0; day < 3; ++day) {
        (void)sim.tick();
        (void)from_wait.tick();
        recorder.after_tick(sim);
        CHECK(from_wait.world().compute_hash() == sim.world().compute_hash());
        CHECK(std::equal(sim.pending_internal().begin(),
                         sim.pending_internal().end(),
                         from_wait.pending_internal().begin(),
                         from_wait.pending_internal().end()));
    }
    REQUIRE(sim.pending_internal().size() == 1);
    auto pending = deserialize_save(serialize_save(sim.world(), sim.pending_internal()));
    REQUIRE(pending.has_value());
    REQUIRE(pending.value().pending_internal.size() == 1);
    CHECK(pending.value().pending_internal.front() == sim.pending_internal().front());
    Simulation from_pending(initial);
    install_family_systems(from_pending, rules);
    from_pending.restore_state(std::move(pending.value().world), std::move(pending.value().pending_internal));
    (void)sim.tick();
    (void)from_wait.tick();
    (void)from_pending.tick();
    recorder.after_tick(sim);
    REQUIRE(sim.world().family().outcomes.size() == 1);
    CHECK(sim.world().family().outcomes.front().result == Result::Accepted);
    CHECK(from_wait.world().compute_hash() == sim.world().compute_hash());
    CHECK(from_pending.world().compute_hash() == sim.world().compute_hash());
    auto replay = deserialize_replay(serialize_replay(recorder.finish(sim)));
    REQUIRE(replay.has_value());
    Simulation playback(initial);
    install_family_systems(playback, rules);
    const auto result = play_replay(playback, replay.value());
    CHECK(result.ok);
    CHECK(result.error.empty());
    CHECK(playback.world().compute_hash() == sim.world().compute_hash());
}

TEST_CASE("marriage negotiations: decision is frozen at emission while eligibility is rechecked") {
    Scene scene;
    const auto row = scene.send(scene.groom_house);
    scene.advance_to(row.answer_due);
    MatchContext context;
    context.initiator = row.initiator;
    const auto neutral =
        evaluate_match(scene.world, scene.rules, row.groom, row.bride, row.dowry_kop, context);
    REQUIRE(neutral.has_value());
    scene.rules.score.accept_threshold = neutral->score(MatchSide::Bride).score;
    REQUIRE(scene.world.add_opinion(scene.bride, scene.groom, pat::core::Fixed32::from_int(100), 0));
    const auto response = scene.answer(row.id);
    const auto payload = std::get<cmd::RespondMarriage>(response.payload);
    REQUIRE(payload.result == Result::Accepted);
    REQUIRE(scene.world.add_opinion(scene.bride, scene.groom, pat::core::Fixed32::from_int(-200), 0));
    (void)scene.world.advance_day();
    const auto changed = scene.answer(row.id);
    CHECK(std::get<cmd::RespondMarriage>(changed.payload).result == Result::Refused);
    scene.execute(response);
    REQUIRE(scene.world.family().outcomes.size() == 1);
    CHECK(scene.world.family().outcomes.front().result == Result::Accepted);
    CHECK(scene.world.family().outcomes.front().score == payload.score);
    CHECK(scene.world.family().outcomes.front().terms == payload.terms);
    CHECK(scene.world.family().outcomes.front().answered == row.answer_due + 1);
}

TEST_CASE("marriage negotiations: configured system faith is shared with answer validation") {
    Scene scene;
    scene.rules.negotiation.answer_days = 1;
    const auto row = scene.send(scene.groom_house);
    std::array<std::string_view, 3> faiths{"orthodox", "other", "orthodox"};
    const auto faith = std::make_shared<FaithPolicy>(faiths);
    Simulation sim(scene.world);
    install_family_systems(sim, std::make_shared<MarriageRules>(scene.rules), faith);
    (void)sim.tick();
    REQUIRE(sim.pending_internal().size() == 1);
    const auto& response = std::get<cmd::RespondMarriage>(sim.pending_internal().front().payload);
    CHECK(response.negotiation == row.id);
    CHECK(response.result == Result::Void);
    CHECK(response.reason == static_cast<std::uint16_t>(MarriageDenial::FaithMismatch));
    (void)sim.tick();
    REQUIRE(sim.world().family().outcomes.size() == 1);
    CHECK(sim.world().family().outcomes.front().result == Result::Void);
    CHECK(sim.world().family().betrothals.empty());
}

TEST_CASE("marriage negotiations: changing a participant house cancels once before the answer") {
    Scene scene;
    REQUIRE(scene.world.enable_economy());
    const auto payer = economy::AccountRef::of(scene.groom_house);
    const auto cost = envoy_cost(scene.rules, false);
    REQUIRE(scene.world.set_initial_treasury(payer, cost + 1000));
    const auto row = scene.send(scene.groom_house);
    REQUIRE(row.cost_kop == cost);
    REQUIRE(scene.world.today() < row.answer_due);
    const auto paid_treasury = scene.world.treasury(payer);
    CHECK(paid_treasury == 1000);
    scene.world.set_character_dynasty(scene.bride, scene.third_house);
    CHECK(scene.world.check_invariants().empty());
    CHECK(scene.world.family().negotiations.empty());
    CHECK(scene.world.family_negotiation_of(scene.groom) == nullptr);
    CHECK(scene.world.family_negotiation_of(scene.bride) == nullptr);
    REQUIRE(scene.world.family().outcomes.size() == 1);
    const auto outcome = scene.world.family().outcomes.front();
    CHECK(outcome.result == Result::Void);
    CHECK(outcome.reason == static_cast<std::uint16_t>(MarriageDenial::HouseChanged));
    CHECK(outcome.initiator == row.initiator);
    CHECK(outcome.responder == row.bride_house);
    CHECK(outcome.responder != scene.third_house);
    CHECK(outcome.groom == row.groom);
    CHECK(outcome.bride == row.bride);
    CHECK(outcome.sent == row.sent);
    CHECK(outcome.answered == scene.world.today());
    CHECK(scene.world.treasury(payer) == paid_treasury);
    CHECK(scene.world.get_opinion(scene.bride, scene.groom) == pat::core::Fixed32{});
    CHECK(scene.world.get_opinion(scene.groom, scene.bride) == pat::core::Fixed32{});
    CHECK_FALSE(marriage_answered_action(outcome).has_value());
    scene.world.set_character_dynasty(scene.bride, scene.bride_house);
    CHECK(scene.world.family().outcomes.size() == 1);
    Simulation sim(scene.world);
    install_family_systems(sim, std::make_shared<MarriageRules>(scene.rules));
    sim.run_days(scene.rules.negotiation.answer_days + 2);
    CHECK(sim.world().check_invariants().empty());
    REQUIRE(sim.world().family().outcomes.size() == 1);
    CHECK(sim.world().family().outcomes.front() == outcome);
    CHECK(sim.world().family().betrothals.empty());
    CHECK(sim.pending_internal().empty());
    CHECK(sim.world().treasury(payer) == paid_treasury);
}

TEST_CASE("marriage negotiations: wedding dates use adulthood and 365 actual Julian days") {
    struct Dates {
        Date groom_birth;
        Date bride_birth;
        Date not_before;
        Date deadline;
    };
    const std::array cases{Dates{Date::from_ymd(1670, 1, 1),
                                 Date::from_ymd(1670, 1, 1),
                                 Date::from_ymd(1704, 2, 29),
                                 Date::from_ymd(1705, 2, 28)},
                           Dates{Date::from_ymd(1692, 2, 29),
                                 Date::from_ymd(1690, 1, 1),
                                 Date::from_ymd(1708, 2, 29),
                                 Date::from_ymd(1709, 2, 28)},
                           Dates{Date::from_ymd(1687, 1, 1),
                                 Date::from_ymd(1692, 2, 29),
                                 Date::from_ymd(1707, 2, 28),
                                 Date::from_ymd(1708, 2, 28)}};
    for (const auto& dates : cases) {
        CAPTURE(dates.groom_birth.days());
        CAPTURE(dates.bride_birth.days());
        Scene scene(Date::from_ymd(1704, 2, 27), dates.groom_birth, dates.bride_birth);
        CHECK(scene.rules.negotiation.wedding_delay_days == 0);
        CHECK(scene.rules.negotiation.wedding_grace_days == 365);
        scene.rules.negotiation.answer_days = 1;
        scene.rules.score.accept_threshold = -1000;
        const auto row = scene.send(scene.groom_house);
        scene.advance_to(row.answer_due);
        const auto response = scene.answer(row.id);
        (void)scene.world.advance_day();
        scene.execute(response);
        REQUIRE(scene.world.family().betrothals.size() == 1);
        const auto& betrothal = scene.world.family().betrothals.front();
        CHECK(betrothal.contracted == Date::from_ymd(1704, 2, 29));
        CHECK(betrothal.wedding_not_before == dates.not_before);
        CHECK(betrothal.wedding_deadline == dates.deadline);
        CHECK(betrothal.wedding_deadline - betrothal.wedding_not_before == 365);
    }
}

TEST_CASE("marriage negotiations: direct marriage structurally clears accepted betrothals") {
    Scene scene;
    scene.rules.score.accept_threshold = -1000;
    const auto row = scene.send(scene.groom_house);
    scene.advance_to(row.answer_due);
    scene.execute(scene.answer(row.id));
    REQUIRE(scene.world.family().betrothals.size() == 1);
    const auto broken_note = [](const ChronicleEntry& entry) {
        return entry.kind == ChronicleKind::Note && entry.param == 205;
    };
    const auto honor_before = scene.world.dynasties().honor;
    const auto opinion_before = scene.world.get_opinion(scene.bride, scene.groom);
    bool fulfilled = true;
    SUBCASE("same pair fulfills its engagement") {
        REQUIRE(scene.world.marry(scene.groom, scene.bride));
    }
    SUBCASE("another pair invalidates its engagement") {
        fulfilled = false;
        const auto other = scene.person(scene.third_house, Sex::Female);
        REQUIRE(scene.world.marry(scene.groom, other));
    }
    CHECK(scene.world.check_invariants().empty());
    CHECK(scene.world.family().betrothals.empty());
    CHECK(scene.world.family_betrothal_of(scene.groom) == nullptr);
    CHECK(scene.world.family_betrothal_of(scene.bride) == nullptr);
    CHECK(scene.world.family().outcomes.size() == 1);
    CHECK(scene.world.dynasties().honor == honor_before);
    CHECK(scene.world.get_opinion(scene.bride, scene.groom) == opinion_before);
    for (const auto house : {scene.groom_house, scene.bride_house}) {
        const auto notes = scene.world.chronicle(house);
        CHECK(std::count_if(notes.begin(), notes.end(), broken_note) == (fulfilled ? 0 : 1));
    }
}
