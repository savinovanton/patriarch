#include "pat/sim/marriage_candidates.hpp"
#include "pat/sim/marriage_dowry.hpp"
#include "pat/sim/marriage_negotiations.hpp"
#include "pat/sim/save.hpp"
#include "pat/sim/simulation.hpp"

#include <algorithm>
#include <utility>

#include "marriage_test_fixture.hpp"

namespace {

using namespace pat::sim;
using economy::AccountRef;
using economy::Kopecks;
using economy::LedgerCode;

struct Scene : pat::test::marriage::Scene {
    MarriageRules rules = MarriageRules::defaults();

    Scene() {
        REQUIRE(world.set_dynasty_head(groom_house, groom));
        REQUIRE(world.set_dynasty_head(bride_house, bride));
    }

    void execute(const Command& command) {
        const CommandContent content{nullptr, nullptr, &rules};
        REQUIRE(validate(world, command, content).has_value());
        apply(world, command, content);
        CHECK(world.check_invariants().empty());
    }

    family::MarriageId wed(CharacterId husband, Kopecks amount = 1000) {
        execute(Command{DynastyId(), cmd::Betroth{husband, bride, amount, bride_house, false}});
        REQUIRE(world.family().betrothals.size() == 1);
        const auto id = world.family().betrothals.front().id;
        execute(Command{DynastyId(), cmd::Wed{id}});
        return world.family().marriages.back().id;
    }

    void settle(CharacterId deceased) {
        REQUIRE(world.die(deceased, CauseOfDeath::Illness));
        execute(Command{DynastyId(), cmd::SettleWidowhood{deceased}});
    }

    void finish_mourning() {
        const auto until = world.today() + rules.eligibility.mourning_days_female;
        while (world.today() < until)
            (void)world.advance_day();
    }

    Kopecks balance(DynastyId house) const { return world.treasury(AccountRef::of(house)); }

    Kopecks total() const { return balance(groom_house) + balance(bride_house) + balance(third_house); }
};

}  // namespace

TEST_CASE("marriage dowry: widow claim uses actual delivery and never transfers at death") {
    Scene scene;
    REQUIRE(scene.world.enable_economy());
    REQUIRE(scene.world.set_initial_treasury(AccountRef::of(scene.bride_house), 1000));
    REQUIRE(
        scene.world.set_initial_treasury(AccountRef::of(scene.groom_house), economy::kMaxTreasuryKop - 400));
    const auto marriage = scene.wed(scene.groom);
    REQUIRE(scene.world.find_family_marriage(marriage)->dowry_kop == 400);
    const auto total = scene.total();
    const auto source = scene.balance(scene.groom_house);
    const auto origin = scene.balance(scene.bride_house);
    bool husband_died = true;
    SUBCASE("husband death creates a claim then widow death cancels it") {}
    SUBCASE("wife death never creates a widower claim") {
        husband_died = false;
    }
    scene.settle(husband_died ? scene.groom : scene.bride);
    const auto* row = scene.world.find_family_marriage(marriage);
    CHECK(row->claim_kop == (husband_died ? 600 : 0));
    CHECK(row->state == (husband_died ? family::MarriageState::Widowed : family::MarriageState::Closed));
    CHECK(scene.total() == total);
    CHECK(scene.balance(scene.groom_house) == source);
    CHECK(scene.balance(scene.bride_house) == origin);
    if (husband_died) {
        scene.settle(scene.bride);
        row = scene.world.find_family_marriage(marriage);
        CHECK(row->claim_kop == 0);
        CHECK(row->state == family::MarriageState::Closed);
        CHECK(row->end_reason == family::MarriageEndReason::BothDead);
        CHECK(scene.total() == total);
        CHECK(scene.balance(scene.groom_house) == source);
    }
}

TEST_CASE("marriage dowry: remarriage quotes the claim and settles an extinct payer once") {
    Scene scene;
    REQUIRE(scene.world.enable_economy());
    REQUIRE(scene.world.set_initial_treasury(AccountRef::of(scene.bride_house), 1000));
    const auto marriage = scene.wed(scene.groom);
    scene.settle(scene.groom);
    REQUIRE(scene.world.dynasty_extinct(scene.groom_house));
    scene.finish_mourning();
    auto loaded = deserialize_save(serialize_save(scene.world));
    REQUIRE(loaded.has_value());
    CHECK(loaded.value().world.compute_hash() == scene.world.compute_hash());
    scene.world = std::move(loaded.value().world);
    const auto replacement = scene.person(scene.third_house, Sex::Male);
    REQUIRE(scene.world.set_dynasty_head(scene.third_house, replacement));
    const auto total = scene.total();
    CandidateQuery query;
    query.member = replacement;
    const auto candidates = find_candidates(scene.world, scene.rules, query);
    const auto candidate = std::find_if(candidates.rows.begin(), candidates.rows.end(), [&](const auto& row) {
        return row.person == scene.bride;
    });
    REQUIRE(candidate != candidates.rows.end());
    CHECK(candidate->dowry_kop == 1500);
    bool negotiated = false;
    SUBCASE("internal betrothal replaces supplied ordinary amount and source") {}
    SUBCASE("accepted envoy uses the same claim") {
        negotiated = true;
    }
    if (negotiated) {
        scene.rules.money.envoy_road_kop = 0;
        scene.rules.money.envoy_gifts_kop = 0;
        scene.rules.score.accept_threshold = -1000;
        scene.execute(Command{scene.third_house, cmd::SendMarriageEnvoy{replacement, scene.bride, 0}});
        REQUIRE(scene.world.family().negotiations.size() == 1);
        const auto negotiation = scene.world.family().negotiations.front();
        CHECK(negotiation.dowry_kop == 1500);
        CHECK(negotiation.dowry_source == scene.groom_house);
        while (scene.world.today() < negotiation.answer_due)
            (void)scene.world.advance_day();
        const auto answer = compute_marriage_response(scene.world, scene.rules, negotiation.id);
        REQUIRE(answer.has_value());
        scene.execute(Command{DynastyId(), *answer});
    } else {
        scene.execute(
            Command{DynastyId(), cmd::Betroth{replacement, scene.bride, 7, scene.bride_house, false}});
    }
    REQUIRE(scene.world.family().betrothals.size() == 1);
    const auto row = scene.world.family().betrothals.front();
    CHECK(row.dowry_kop == 1500);
    CHECK(row.dowry_source == scene.groom_house);
    CHECK(row.claim_marriage == marriage);
    CHECK(scene.balance(scene.groom_house) == 1000);
    while (scene.world.today() < row.wedding_not_before)
        (void)scene.world.advance_day();
    const Command wedding{DynastyId(), cmd::Wed{row.id}};
    scene.execute(wedding);
    CHECK(scene.balance(scene.groom_house) == -500);
    CHECK(scene.balance(scene.third_house) == 1500);
    CHECK(scene.balance(scene.bride_house) == 0);
    CHECK(scene.total() == total);
    CHECK(scene.world.find_family_marriage(marriage)->claim_kop == 0);
    CHECK(scene.world.find_family_marriage(marriage)->state == family::MarriageState::Closed);
    CHECK(scene.world.family().marriages.back().dowry_kop == 1500);
    const auto hash = scene.world.compute_hash();
    CHECK_FALSE(validate(scene.world, wedding, {nullptr, nullptr, &scene.rules}).has_value());
    CHECK(scene.world.compute_hash() == hash);
}

TEST_CASE("marriage dowry: saturated remarriage records delivery and closes the entire claim") {
    Scene scene;
    REQUIRE(scene.world.enable_economy());
    REQUIRE(scene.world.set_initial_treasury(AccountRef::of(scene.bride_house), 1000));
    REQUIRE(
        scene.world.set_initial_treasury(AccountRef::of(scene.third_house), economy::kMaxTreasuryKop - 400));
    const auto marriage = scene.wed(scene.groom);
    scene.settle(scene.groom);
    scene.finish_mourning();
    const auto replacement = scene.person(scene.third_house, Sex::Male);
    const auto total = scene.total();
    (void)scene.wed(replacement);
    CHECK(scene.balance(scene.groom_house) == 600);
    CHECK(scene.balance(scene.third_house) == economy::kMaxTreasuryKop);
    CHECK(scene.world.family().marriages.back().dowry_kop == 400);
    CHECK(scene.world.find_family_marriage(marriage)->claim_kop == 0);
    CHECK(scene.world.find_family_marriage(marriage)->state == family::MarriageState::Closed);
    CHECK(scene.total() == total);
}

TEST_CASE("marriage dowry: remarriage within previous husband's house offsets the claim") {
    Scene scene;
    REQUIRE(scene.world.enable_economy());
    REQUIRE(scene.world.set_initial_treasury(AccountRef::of(scene.bride_house), 1000));
    const auto replacement = scene.person(scene.groom_house, Sex::Male);
    const auto marriage = scene.wed(scene.groom);
    scene.settle(scene.groom);
    scene.finish_mourning();
    const auto before = scene.balance(scene.groom_house);
    (void)scene.wed(replacement);
    CHECK(scene.balance(scene.groom_house) == before);
    CHECK(scene.world.family().marriages.back().dowry_kop == 0);
    CHECK(scene.world.find_family_marriage(marriage)->claim_kop == 0);
    CHECK(scene.world.find_family_marriage(marriage)->state == family::MarriageState::Closed);
}

TEST_CASE("marriage dowry: refund quota counts actual transfers and is cumulative") {
    Scene scene;
    REQUIRE(scene.world.enable_economy());
    REQUIRE(scene.world.set_initial_treasury(AccountRef::of(scene.bride_house), economy::kMaxTreasuryKop));
    const auto marriage = scene.wed(scene.groom);
    // Leave only 200 kopecks of receiving headroom, without minting money.
    REQUIRE(scene.world.transfer(AccountRef::of(scene.groom_house),
                                 AccountRef::of(scene.bride_house),
                                 LedgerCode::DowryOut,
                                 LedgerCode::DowryIn,
                                 800) == 800);
    const auto total = scene.total();
    CHECK(refund_marriage_dowry(scene.world, scene.rules, marriage) == 200);
    CHECK(scene.world.find_family_marriage(marriage)->refunded_kop == 200);
    CHECK(refund_marriage_dowry(scene.world, scene.rules, marriage) == 0);
    REQUIRE(scene.world.transfer(AccountRef::of(scene.bride_house),
                                 AccountRef::of(scene.groom_house),
                                 LedgerCode::DowryOut,
                                 LedgerCode::DowryIn,
                                 1000) == 1000);
    CHECK(refund_marriage_dowry(scene.world, scene.rules, marriage) == 300);
    CHECK(scene.world.find_family_marriage(marriage)->refunded_kop == 500);
    CHECK(refund_marriage_dowry(scene.world, scene.rules, marriage) == 0);
    scene.rules.consequences.tonsure_refund_permille = 1000;
    CHECK(refund_marriage_dowry(scene.world, scene.rules, marriage) == 500);
    CHECK(scene.world.find_family_marriage(marriage)->refunded_kop == 1000);
    CHECK(refund_marriage_dowry(scene.world, scene.rules, marriage) == 0);
    CHECK(scene.total() == total);
    scene.settle(scene.groom);
    CHECK(refund_marriage_dowry(scene.world, scene.rules, marriage) == 0);
    CHECK(refund_marriage_dowry(scene.world, scene.rules, 0) == 0);
    CHECK(scene.world.check_invariants().empty());
}

TEST_CASE("marriage dowry: non-economic worlds never record a monetary refund") {
    Scene scene;
    const auto marriage = scene.wed(scene.groom);
    CHECK(refund_marriage_dowry(scene.world, scene.rules, marriage) == 0);
    CHECK(scene.world.find_family_marriage(marriage)->refunded_kop == 0);
    CHECK(scene.world.check_invariants().empty());
}

TEST_CASE("marriage dowry: fifty years conserve money and deterministic remarriage state") {
    Scene scene;
    REQUIRE(scene.world.enable_economy());
    REQUIRE(scene.world.set_initial_treasury(AccountRef::of(scene.bride_house), 1000));
    const auto initial_total = scene.total();
    const auto horizon = Date::from_ymd(1753, 5, 20);
    (void)scene.wed(scene.groom);
    const auto advance_to = [&](Date date) {
        Simulation sim(scene.world);
        sim.run_days(date - scene.world.today());
        scene.world = sim.world();
        CHECK(scene.total() == initial_total);
    };
    advance_to(Date::from_ymd(1713, 5, 20));
    scene.settle(scene.groom);
    advance_to(Date::from_ymd(1723, 5, 20));
    const auto replacement = scene.person(scene.third_house, Sex::Male);
    auto loaded = deserialize_save(serialize_save(scene.world));
    REQUIRE(loaded.has_value());
    Scene restored_scene = scene;
    restored_scene.world = std::move(loaded.value().world);
    (void)scene.wed(replacement);
    (void)restored_scene.wed(replacement);
    REQUIRE(scene.world.family().marriages.size() == 2);
    REQUIRE(scene.world.family().marriages.back().dowry_kop == 1500);
    Simulation continuous(scene.world);
    Simulation restored(restored_scene.world);
    const auto days = horizon - scene.world.today();
    continuous.run_days(days);
    restored.run_days(days);
    CHECK(continuous.world().today() == horizon);
    CHECK(continuous.world().compute_hash() == restored.world().compute_hash());
    const auto& world = continuous.world();
    CHECK(world.treasury(AccountRef::of(scene.groom_house)) == -500);
    CHECK(world.treasury(AccountRef::of(scene.third_house)) == 1500);
    CHECK(world.treasury(AccountRef::of(scene.groom_house)) +
              world.treasury(AccountRef::of(scene.bride_house)) +
              world.treasury(AccountRef::of(scene.third_house)) ==
          initial_total);
    CHECK(world.check_invariants().empty());
}
