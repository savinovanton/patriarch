#include "pat/core/fixed.hpp"
#include "pat/sim/demo_scenario.hpp"
#include "pat/sim/family/tables.hpp"
#include "pat/sim/world.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>

using namespace pat::sim;
using pat::core::Fixed32;
using pat::core::StringId;

namespace {

Date today() {
    return Date::from_ymd(1703, 5, 20);
}

World make_family_world() {
    World world(today(), 297);
    REQUIRE(world.enable_dynasty_mode());
    for (std::uint32_t i = 0; i < 3; ++i) {
        REQUIRE(world.create_dynasty(StringId(100 + i)) == DynastyId(i));
        REQUIRE(world.create_province(StringId(200 + i), DynastyId(i), Fixed32::from_int(100)) ==
                ProvinceId(i));
    }
    return world;
}

CharacterId person(World& world,
                   DynastyId dynasty,
                   Sex sex = Sex::Male,
                   int birth_year = 1670,
                   CharacterId father = {},
                   CharacterId mother = {}) {
    CharacterInit init;
    init.dynasty = dynasty;
    init.sex = sex;
    init.birth_date = Date::from_ymd(birth_year, 1, 1);
    init.father = father;
    init.mother = mother;
    const CharacterId id = world.create_character(init);
    REQUIRE(id.valid());
    return id;
}

struct Pair {
    CharacterId groom;
    CharacterId bride;
    DynastyId groom_house;
    DynastyId bride_house;
};

Pair pair(World& world, std::uint32_t house = 0, int birth_year = 1670) {
    const DynastyId groom_house(house);
    const DynastyId bride_house((house + 1) % 3);
    return {person(world, groom_house, Sex::Male, birth_year),
            person(world, bride_house, Sex::Female, birth_year),
            groom_house,
            bride_house};
}

family::Negotiation negotiation(const Pair& participants) {
    family::Negotiation row;
    row.initiator = participants.groom_house;
    row.groom = participants.groom;
    row.bride = participants.bride;
    row.groom_house = participants.groom_house;
    row.bride_house = participants.bride_house;
    row.sent = today() - 1;
    row.answer_due = today() + 29;
    row.cost_kop = 12'000;
    row.dowry_kop = 50'000;
    row.dowry_source = participants.bride_house;
    return row;
}

family::Betrothal betrothal(const Pair& participants) {
    family::Betrothal row;
    row.groom = participants.groom;
    row.bride = participants.bride;
    row.groom_house = participants.groom_house;
    row.bride_house = participants.bride_house;
    row.contracted = today();
    row.wedding_not_before = today() + 30;
    row.wedding_deadline = today() + 120;
    row.dowry_kop = 50'000;
    row.dowry_source = participants.bride_house;
    row.origin = family::BetrothalOrigin::Command;
    return row;
}

family::Marriage marriage(const Pair& participants) {
    family::Marriage row;
    row.groom = participants.groom;
    row.bride = participants.bride;
    row.groom_house = participants.groom_house;
    row.bride_house = participants.bride_house;
    row.wedding = today() - 30;
    row.dowry_kop = 100'000;
    return row;
}

family::NegotiationOutcome outcome(const Pair& participants, family::NegotiationId id) {
    family::NegotiationOutcome row;
    row.negotiation = id;
    row.initiator = participants.groom_house;
    row.responder = participants.bride_house;
    row.groom = participants.groom;
    row.bride = participants.bride;
    row.sent = today() - 1;
    row.answered = today();
    row.result = family::NegotiationResult::Refused;
    row.score = -12;
    row.threshold = 10;
    row.terms[0] = 8;
    row.terms[1] = -20;
    row.reason = 1;
    return row;
}

void require_valid(const World& world) {
    const auto problems = world.check_invariants();
    for (const auto& problem : problems) {
        MESSAGE(problem);
    }
    REQUIRE(problems.empty());
}

template <typename Mutation>
void reject_atomically(World& world, Mutation mutate) {
    const family::State before = world.family();
    const WorldHash hash = world.compute_hash();
    CHECK_FALSE(static_cast<bool>(mutate()));
    CHECK(world.family() == before);
    CHECK(world.compute_hash() == hash);
}

family::State& mutable_family(World& world) {
    return const_cast<family::State&>(world.family());
}

template <typename Damage>
void detects_corruption(const World& source, Damage damage) {
    World copy = source;
    damage(mutable_family(copy));
    CHECK_FALSE(copy.check_invariants().empty());
    require_valid(source);
}

}  // namespace

TEST_CASE("family state: generated disjoint contracts keep participant indexes consistent") {
    World world = make_family_world();
    for (std::uint32_t i = 0; i < 18; ++i) {
        const Pair participants = pair(world, i % 3, 1660 + static_cast<int>(i));
        if (i % 3 == 0) {
            auto row = negotiation(participants);
            row.dowry_kop = static_cast<std::int64_t>(i) * 1'001;
            const auto id = world.create_family_negotiation(row);
            REQUIRE(id.has_value());
            CHECK(world.engagement_of(participants.groom) == 1);
            CHECK(world.engagement_of(participants.bride) == 1);
            REQUIRE(world.family_negotiation_of(participants.bride) != nullptr);
            CHECK(world.family_negotiation_of(participants.bride)->id == *id);
        } else if (i % 3 == 1) {
            const auto id = world.create_family_betrothal(betrothal(participants));
            REQUIRE(id.has_value());
            CHECK(world.engagement_of(participants.groom) == 2);
            CHECK(world.engagement_of(participants.bride) == 2);
            REQUIRE(world.family_betrothal_of(participants.groom) != nullptr);
            CHECK(world.family_betrothal_of(participants.groom)->id == *id);
        } else {
            REQUIRE(world.marry(participants.groom, participants.bride));
            const auto id = world.create_family_marriage(marriage(participants));
            REQUIRE(id.has_value());
            REQUIRE(world.family_marriage_of(participants.bride) != nullptr);
            CHECK(world.family_marriage_of(participants.bride)->id == *id);
            CHECK(world.engagement_of(participants.groom) == 0);
        }
        require_valid(world);
    }
    CHECK(world.family().negotiations.size() == 6);
    CHECK(world.family().betrothals.size() == 6);
    CHECK(world.family().marriages.size() == 6);
}

TEST_CASE("family state: both participant columns share one engagement across both tables") {
    World world = make_family_world();
    const Pair first = pair(world);
    const Pair second = pair(world, 1);
    const auto first_id = world.create_family_negotiation(negotiation(first));
    const auto second_id = world.create_family_betrothal(betrothal(second));
    REQUIRE(first_id.has_value());
    REQUIRE(second_id.has_value());

    auto duplicate = negotiation(first);
    reject_atomically(world, [&] { return world.create_family_negotiation(duplicate); });
    auto cross_table = betrothal(first);
    reject_atomically(world, [&] { return world.create_family_betrothal(cross_table); });
    auto occupied_groom = negotiation(pair(world, 2));
    occupied_groom.groom = second.groom;
    occupied_groom.groom_house = second.groom_house;
    occupied_groom.initiator = second.groom_house;
    reject_atomically(world, [&] { return world.create_family_negotiation(occupied_groom); });
    auto occupied_bride = betrothal(pair(world, 2));
    occupied_bride.bride = first.bride;
    occupied_bride.bride_house = first.bride_house;
    occupied_bride.dowry_source = first.bride_house;
    reject_atomically(world, [&] { return world.create_family_betrothal(occupied_bride); });

    REQUIRE(world.close_family_negotiation(*first_id));
    CHECK(world.engagement_of(first.groom) == 0);
    CHECK(world.engagement_of(first.bride) == 0);
    REQUIRE(world.create_family_betrothal(betrothal(first)).has_value());
    CHECK(world.engagement_of(first.groom) == 2);
    require_valid(world);
}

TEST_CASE("family state: invalid negotiation fields reject without spending an id") {
    World world = make_family_world();
    const auto valid = negotiation(pair(world));
    for (int problem = 0; problem < 12; ++problem) {
        INFO("invalid field " << problem);
        auto row = valid;
        switch (problem) {
            case 0:
                row.cost_kop = -1;
                break;
            case 1:
                row.dowry_kop = -1;
                break;
            case 2:
                row.answer_due = row.sent - 1;
                break;
            case 3:
                row.sent = kNoDate;
                break;
            case 4:
                row.bride = CharacterId(999'999);
                break;
            case 5:
                row.groom = row.bride;
                break;
            case 6:
                row.dowry_source = DynastyId(999);
                break;
            case 7:
                row.dowry_tier = 1;
                break;
            case 8:
                row.dispensation = 2;
                break;
            case 9:
                row.id = 77;
                break;
            case 10:
                row.bride_house = DynastyId(2);
                break;
            case 11:
                row.groom_house = DynastyId(2);
                row.initiator = row.groom_house;
                break;
        }
        reject_atomically(world, [&] { return world.create_family_negotiation(row); });
    }
    const auto id = world.create_family_negotiation(valid);
    REQUIRE(id.has_value());
    CHECK(*id == 1);
    auto changed = *world.find_family_negotiation(*id);
    changed.answer_due = changed.sent - 1;
    reject_atomically(world, [&] { return world.update_family_negotiation(*id, changed); });
    changed = *world.find_family_negotiation(*id);
    changed.bride_house = DynastyId(2);
    reject_atomically(world, [&] { return world.update_family_negotiation(*id, changed); });
    require_valid(world);
}

TEST_CASE("family state: betrothal dates and widow claim references are checked atomically") {
    World world = make_family_world();
    const auto valid = betrothal(pair(world));
    for (int problem = 0; problem < 7; ++problem) {
        INFO("invalid betrothal field " << problem);
        auto row = valid;
        switch (problem) {
            case 0:
                row.wedding_not_before = row.contracted - 1;
                break;
            case 1:
                row.wedding_deadline = row.wedding_not_before - 1;
                break;
            case 2:
                row.dowry_kop = -1;
                break;
            case 3:
                row.claim_marriage = 999;
                break;
            case 4:
                row.origin = static_cast<family::BetrothalOrigin>(3);
                break;
            case 5:
                row.bride_house = DynastyId(2);
                break;
            case 6:
                row.groom_house = DynastyId(2);
                break;
        }
        reject_atomically(world, [&] { return world.create_family_betrothal(row); });
    }
    const auto id = world.create_family_betrothal(valid);
    REQUIRE(id.has_value());
    auto changed = *world.find_family_betrothal(*id);
    changed.bride_house = DynastyId(2);
    reject_atomically(world, [&] { return world.update_family_betrothal(*id, changed); });
    require_valid(world);
}

TEST_CASE("family state: recording a marriage requires the existing E7 union") {
    World world = make_family_world();
    const Pair participants = pair(world);
    const auto row = marriage(participants);
    reject_atomically(world, [&] { return world.create_family_marriage(row); });
    CHECK_FALSE(world.characters().spouse[participants.groom.index()].valid());
    REQUIRE(world.marry(participants.groom, participants.bride));
    const auto id = world.create_family_marriage(row);
    REQUIRE(id.has_value());
    reject_atomically(world, [&] { return world.create_family_marriage(row); });
    for (int problem = 0; problem < 4; ++problem) {
        auto changed = *world.find_family_marriage(*id);
        switch (problem) {
            case 0:
                changed.refunded_kop = changed.dowry_kop + 1;
                break;
            case 1:
                changed.claim_kop = 1;
                break;
            case 2:
                changed.ended = today();
                break;
            case 3:
                changed.dowry_kop = -1;
                break;
        }
        reject_atomically(world, [&] { return world.update_family_marriage(*id, changed); });
    }
    require_valid(world);
}

TEST_CASE("family state: a foreign mother can govern without joining the child's dynasty") {
    World world = make_family_world();
    const DynastyId house(0);
    const DynastyId foreign_house(1);
    const CharacterId father = person(world, house);
    const CharacterId mother = person(world, foreign_house, Sex::Female);
    const CharacterId child = person(world, house, Sex::Male, 1695, father, mother);
    const CharacterId stranger = person(world, foreign_house, Sex::Female);
    REQUIRE(world.marry(father, mother));
    REQUIRE(world.die(father, CauseOfDeath::Illness));
    REQUIRE(world.set_dynasty_head(house, child));
    family::Regency row;
    row.dynasty = house;
    row.ward = child;
    row.regent = mother;
    row.kind = family::RegencyKind::Mother;
    row.started = today();
    row.last_risk_permille = 1000;
    REQUIRE(world.set_family_regency(row));
    CHECK(world.characters().dynasty[mother.index()] == foreign_house);
    CHECK(world.dynasties().head[house.index()] == child);
    REQUIRE(world.family_regency(house) != nullptr);
    CHECK(world.family_regency(house)->regent == mother);
    require_valid(world);

    family::PersonStatus disowned;
    disowned.character = mother;
    disowned.flags = family::kPersonDisowned;
    disowned.disowned_on = today();
    reject_atomically(world, [&] { return world.set_family_status(disowned); });

    auto invalid = row;
    invalid.regent = stranger;
    reject_atomically(world, [&] { return world.set_family_regency(invalid); });
    invalid = row;
    invalid.kind = family::RegencyKind::Guardian;
    reject_atomically(world, [&] { return world.set_family_regency(invalid); });
    invalid = row;
    invalid.excluded = mother;
    reject_atomically(world, [&] { return world.set_family_regency(invalid); });
    invalid = row;
    invalid.last_risk_permille = 1001;
    reject_atomically(world, [&] { return world.set_family_regency(invalid); });

    REQUIRE(world.clear_family_regency(house));
    row.kind = family::RegencyKind::Imperial;
    row.regent = {};
    REQUIRE(world.set_family_regency(row));
    require_valid(world);
}

TEST_CASE("family state: dispute deaths preserve the saved order and advance temporary authority") {
    World world = make_family_world();
    const DynastyId house(0);
    const CharacterId predecessor = person(world, house, Sex::Male, 1640);
    const CharacterId first = person(world, house, Sex::Male, 1670);
    const CharacterId second = person(world, house, Sex::Male, 1671);
    const CharacterId third = person(world, house, Sex::Male, 1672);
    REQUIRE(world.die(predecessor, CauseOfDeath::OldAge));
    REQUIRE(world.set_dynasty_head(house, {}));
    family::Dispute row;
    row.dynasty = house;
    row.predecessor = predecessor;
    row.opened = today();
    row.resolve_by = today() + 90;
    row.claimants_n = 3;
    row.claimants[0] = {first, 30};
    row.claimants[1] = {second, 20};
    row.claimants[2] = {third, 10};
    REQUIRE(world.set_family_dispute(row));
    CHECK(world.family_caretaker(house) == first);
    CHECK_FALSE(world.dynasties().head[house.index()].valid());
    require_valid(world);

    REQUIRE(world.die(first, CauseOfDeath::Illness));
    REQUIRE(world.family_dispute(house) != nullptr);
    CHECK(world.family_dispute(house)->claimants == row.claimants);
    CHECK(world.family_dispute(house)->claimants_n == 3);
    CHECK(world.family_caretaker(house) == second);
    CHECK(world.is_archived(first));
    CHECK_FALSE(world.dynasties().head[house.index()].valid());
    require_valid(world);
    REQUIRE(world.die(second, CauseOfDeath::Illness));
    CHECK(world.family_caretaker(house) == third);
    CHECK(world.family_dispute(house)->claimants == row.claimants);
    require_valid(world);
}

TEST_CASE("family state: adult dispute authority precedes minority regency") {
    World world = make_family_world();
    const DynastyId house(0);
    const DynastyId foreign_house(1);
    const CharacterId predecessor = person(world, house);
    const CharacterId mother = person(world, foreign_house, Sex::Female);
    const CharacterId first = person(world, house, Sex::Male, 1695, predecessor, mother);
    const CharacterId second = person(world, house, Sex::Male, 1698, predecessor, mother);
    const CharacterId adult = person(world, house, Sex::Male, 1670);
    REQUIRE(world.marry(predecessor, mother));
    REQUIRE(world.die(predecessor, CauseOfDeath::Illness));
    REQUIRE(world.set_dynasty_head(house, {}));
    family::Dispute dispute;
    dispute.dynasty = house;
    dispute.predecessor = predecessor;
    dispute.opened = today();
    dispute.resolve_by = today() + 90;
    dispute.claimants_n = 3;
    dispute.claimants[0] = {first, 20};
    dispute.claimants[1] = {second, 10};
    dispute.claimants[2] = {adult, 5};
    REQUIRE(world.set_family_dispute(dispute));
    CHECK(world.family_caretaker(house) == adult);
    family::Regency regency;
    regency.dynasty = house;
    regency.ward = first;
    regency.regent = mother;
    regency.kind = family::RegencyKind::Mother;
    regency.started = today();
    reject_atomically(world, [&] { return world.set_family_regency(regency); });
    REQUIRE(world.die(adult, CauseOfDeath::Illness));
    CHECK(world.family_caretaker(house) == first);
    REQUIRE(world.set_family_regency(regency));
    CHECK_FALSE(world.dynasties().head[house.index()].valid());
    CHECK(world.characters().dynasty[mother.index()] == foreign_house);
    REQUIRE(world.family_regency(house) != nullptr);
    CHECK(world.family_regency(house)->ward == first);
    CHECK(world.family_regency(house)->regent == mother);
    reject_atomically(world, [&] { return world.clear_family_dispute(house); });
    auto changed_dispute = dispute;
    changed_dispute.claimants[2].character = person(world, house, Sex::Male, 1670);
    reject_atomically(world, [&] { return world.set_family_dispute(changed_dispute); });
    require_valid(world);
}

TEST_CASE("family state: succession references accept archives and reject unknown people") {
    World world = make_family_world();
    const DynastyId house(0);
    const CharacterId heir = person(world, house);
    const CharacterId guardian = person(world, house);
    REQUIRE(world.die(guardian, CauseOfDeath::Illness));
    family::SuccessionState row;
    row.dynasty = house;
    row.designated_heir = heir;
    row.guardian = guardian;
    row.law_changed_on = today();
    REQUIRE(world.set_family_succession(row));
    require_valid(world);
    auto invalid = row;
    invalid.designated_heir = CharacterId(999'999);
    reject_atomically(world, [&] { return world.set_family_succession(invalid); });
    invalid = row;
    invalid.law_changed_on = today() + 1;
    reject_atomically(world, [&] { return world.set_family_succession(invalid); });

    family::PersonStatus status;
    status.character = heir;
    status.flags = family::kPersonTonsured;
    status.tonsured_on = today();
    reject_atomically(world, [&] { return world.set_family_status(status); });
    REQUIRE(world.clear_family_succession(house));
    REQUIRE(world.set_family_status(status));
    reject_atomically(world, [&] { return world.set_family_succession(row); });
    require_valid(world);
}

TEST_CASE("family state: land grants require the owner's adult and the existing administrator") {
    World world = make_family_world();
    const CharacterId holder = person(world, DynastyId(0));
    const CharacterId stranger = person(world, DynastyId(1));
    const CharacterId child = person(world, DynastyId(0), Sex::Male, 1695);
    REQUIRE(world.enable_economy());
    family::LandGrant row;
    row.province = ProvinceId(0);
    row.holder = holder;
    row.granted_on = today();
    reject_atomically(world, [&] { return world.set_family_land_grant(row); });
    REQUIRE(world.set_province_administrator(row.province, holder));
    REQUIRE(world.set_family_land_grant(row));
    for (const auto flags : {family::kPersonTonsured, family::kPersonDisowned}) {
        family::PersonStatus status;
        status.character = holder;
        status.flags = flags;
        status.tonsured_on = flags == family::kPersonTonsured ? today() : kNoDate;
        status.disowned_on = flags == family::kPersonDisowned ? today() : kNoDate;
        reject_atomically(world, [&] { return world.set_family_status(status); });
    }
    auto invalid = row;
    invalid.holder = stranger;
    reject_atomically(world, [&] { return world.set_family_land_grant(invalid); });
    invalid.holder = child;
    reject_atomically(world, [&] { return world.set_family_land_grant(invalid); });
    invalid = row;
    invalid.province = ProvinceId(999);
    reject_atomically(world, [&] { return world.set_family_land_grant(invalid); });
    CHECK(world.provinces().owner[0] == DynastyId(0));
    CHECK(world.economy().provinces.administrator[0] == holder);
    require_valid(world);
}

TEST_CASE("family state: death frees engagements but preserves irreversible status facts") {
    World world = make_family_world();
    const Pair negotiating = pair(world);
    const Pair betrothed = pair(world, 1);
    const auto negotiation_id = world.create_family_negotiation(negotiation(negotiating));
    const auto betrothal_id = world.create_family_betrothal(betrothal(betrothed));
    REQUIRE(negotiation_id.has_value());
    REQUIRE(betrothal_id.has_value());
    const CharacterId remembered = person(world, DynastyId(2), Sex::Female);
    family::PersonStatus status;
    status.character = remembered;
    status.flags = family::kPersonTonsured | family::kPersonDisowned;
    status.tonsured_on = today() - 20;
    status.disowned_on = today() - 10;
    status.widowed_on = today() - 30;
    REQUIRE(world.set_family_status(status));
    REQUIRE(world.die(negotiating.bride, CauseOfDeath::Illness));
    REQUIRE(world.die(betrothed.groom, CauseOfDeath::Illness));
    REQUIRE(world.die(remembered, CauseOfDeath::Illness));
    CHECK(world.find_family_negotiation(*negotiation_id) == nullptr);
    CHECK(world.find_family_betrothal(*betrothal_id) == nullptr);
    CHECK(world.engagement_of(negotiating.groom) == 0);
    CHECK(world.engagement_of(betrothed.bride) == 0);
    REQUIRE(world.family_status(remembered) != nullptr);
    CHECK(world.family_status(remembered)->flags == status.flags);
    CHECK(world.family_status(remembered)->tonsured_on == status.tonsured_on);
    CHECK(world.family_status(remembered)->disowned_on == status.disowned_on);
    CHECK(world.family_status(remembered)->widowed_on == kNoDate);
    require_valid(world);
}

TEST_CASE("family state: the outcome ring retains original explanations and bounded history") {
    World world = make_family_world();
    const Pair participants = pair(world);
    constexpr auto count = family::kNegotiationOutcomeRingSize + 5;
    for (std::size_t i = 0; i < count; ++i) {
        const auto id = world.create_family_negotiation(negotiation(participants));
        REQUIRE(id.has_value());
        REQUIRE(world.close_family_negotiation(*id));
        const auto seq = world.append_family_outcome(outcome(participants, *id));
        REQUIRE(seq.has_value());
        CHECK(*seq == static_cast<std::uint32_t>(i + 1));
    }
    REQUIRE(world.family().outcomes.size() == family::kNegotiationOutcomeRingSize);
    CHECK(world.family().outcomes.front().seq == 6);
    CHECK(world.family().outcomes.back().seq == count);
    CHECK(world.find_family_outcome(5) == nullptr);
    const auto* remembered = world.find_family_outcome(6);
    REQUIRE(remembered != nullptr);
    CHECK(remembered->score == -12);
    CHECK(remembered->terms[0] == 8);
    CHECK(remembered->terms[1] == -20);
    CHECK(world.family().next_negotiation_id == count + 1);
    CHECK(world.family().next_outcome_seq == count + 1);
    auto invalid = outcome(participants, 1);
    invalid.score = 999;
    reject_atomically(world, [&] { return world.append_family_outcome(invalid); });
    require_valid(world);
}

TEST_CASE("family state: monotone ids are not reused and exhaustion rejects atomically") {
    World world = make_family_world();
    const auto row = negotiation(pair(world));
    const auto first = world.create_family_negotiation(row);
    REQUIRE(first.has_value());
    REQUIRE(world.close_family_negotiation(*first));
    const auto second = world.create_family_negotiation(row);
    REQUIRE(second.has_value());
    CHECK(*second > *first);
    REQUIRE(world.close_family_negotiation(*second));
    mutable_family(world).next_negotiation_id = std::numeric_limits<std::uint32_t>::max();
    reject_atomically(world, [&] { return world.create_family_negotiation(row); });
    require_valid(world);
}

TEST_CASE("family state: marriage history keeps referenced records when full") {
    World world = make_family_world();
    const Pair historical = pair(world);
    auto record = marriage(historical);
    record.state = family::MarriageState::Closed;
    record.ended = today();
    record.end_reason = family::MarriageEndReason::Annulment;
    auto& state = mutable_family(world);
    state.marriages.assign(family::kMaxMarriageRecords, record);
    for (std::size_t i = 0; i < state.marriages.size(); ++i) {
        state.marriages[i].id = static_cast<family::MarriageId>(i + 1);
    }
    state.next_marriage_id = static_cast<family::MarriageId>(state.marriages.size() + 1);
    auto contract = betrothal(historical);
    contract.claim_marriage = 1;
    REQUIRE(world.create_family_betrothal(contract).has_value());
    require_valid(world);

    const Pair newlyweds = pair(world, 1);
    REQUIRE(world.marry(newlyweds.groom, newlyweds.bride));
    const auto id = world.create_family_marriage(marriage(newlyweds));
    REQUIRE(id.has_value());
    CHECK(world.family().marriages.size() == family::kMaxMarriageRecords);
    CHECK(world.find_family_marriage(1) != nullptr);
    CHECK(world.find_family_marriage(2) == nullptr);
    require_valid(world);
}

TEST_CASE("family state: snapshots and copies own the sparse tables and counters") {
    World world = make_family_world();
    const Pair first = pair(world);
    const Pair second = pair(world, 1);
    const auto id = world.create_family_negotiation(negotiation(first));
    REQUIRE(id.has_value());
    REQUIRE(world.create_family_betrothal(betrothal(second)).has_value());
    const auto snapshot = world.snapshot();
    const World copy = world;
    CHECK(snapshot->family() == world.family());
    CHECK(copy.family() == world.family());
    REQUIRE(world.close_family_negotiation(*id));
    CHECK_FALSE(snapshot->family() == world.family());
    REQUIRE(snapshot->find_family_negotiation(*id) != nullptr);
    CHECK(snapshot->engagement_of(first.groom) == 1);
    CHECK(copy.engagement_of(second.bride) == 2);
    require_valid(*snapshot);
    require_valid(copy);
    require_valid(world);
}

TEST_CASE("family invariants: malformed persistent rows cannot hide behind derived indexes") {
    World world = make_family_world();
    const Pair first = pair(world);
    const Pair second = pair(world, 1);
    REQUIRE(world.create_family_negotiation(negotiation(first)).has_value());
    REQUIRE(world.create_family_negotiation(negotiation(second)).has_value());
    require_valid(world);
    detects_corruption(world,
                       [](family::State& state) { state.negotiations[1].id = state.negotiations[0].id; });
    detects_corruption(
        world, [](family::State& state) { state.negotiations[1].bride = state.negotiations[0].bride; });
    detects_corruption(world, [](family::State& state) { state.negotiations[0].cost_kop = -1; });
    detects_corruption(world, [](family::State& state) {
        state.negotiations[0].answer_due = state.negotiations[0].sent - 1;
    });
    detects_corruption(world, [](family::State& state) { state.next_negotiation_id = 1; });
}

TEST_CASE("family state: empty family APIs preserve plain dynasty and demo hashes") {
    const std::array<bool, 2> modes{false, true};
    for (const bool dynasty_mode : modes) {
        World world(today(), 297);
        if (dynasty_mode) {
            REQUIRE(world.enable_dynasty_mode());
        }
        REQUIRE(world.family() == family::State{});
        const WorldHash before = world.compute_hash();
        reject_atomically(world, [&] { return world.close_family_negotiation(1); });
        reject_atomically(world, [&] { return world.close_family_betrothal(1); });
        CHECK(world.compute_hash() == before);
        for (std::size_t part = 8; part < 16; ++part) {
            CHECK(before.ext[part] == 0);
        }
        require_valid(world);
    }
    demo::Config config;
    config.seed = 297;
    config.dynasties = 2;
    config.provinces = 3;
    config.characters_per_dynasty = 4;
    const World demo = demo::make_world(config);
    CHECK(demo.family() == family::State{});
    for (std::size_t part = 8; part < 16; ++part) {
        CHECK(demo.compute_hash().ext[part] == 0);
    }
    require_valid(demo);
}
