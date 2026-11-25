#include "pat/sim/dynasty_relation.hpp"
#include "pat/sim/marriage_candidates.hpp"
#include "pat/sim/save.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <string_view>
#include <vector>

namespace {

using namespace pat::sim;

struct Scene {
    World world{Date::from_ymd(1703, 1, 1), 301};
    pat::core::StringInterner names;
    MarriageRules rules = MarriageRules::defaults();
    DynastyId own;
    DynastyId other;
    DynastyId third;
    CharacterId member;

    CharacterId person(DynastyId house,
                       Sex sex,
                       int age = 20,
                       std::string_view name = {},
                       CharacterId father = {},
                       CharacterId mother = {}) {
        CharacterInit init;
        init.dynasty = house;
        init.sex = sex;
        init.birth_date = Date::from_ymd(1703 - age, 1, 1);
        init.name = name.empty() ? pat::core::StringId{} : names.intern(name);
        init.father = father;
        init.mother = mother;
        init.health = pat::core::Fixed32::from_int(50);
        init.fertility = pat::core::Fixed32::from_int(50);
        init.skills.fill(pat::core::Fixed32::from_int(8));
        const auto id = world.create_character(init);
        REQUIRE(id.valid());
        return id;
    }

    Scene() {
        REQUIRE(world.enable_dynasty_mode());
        own = world.create_dynasty(names.intern("Volkov"));
        other = world.create_dynasty(names.intern("Долгоруков"));
        third = world.create_dynasty(names.intern("Orlov"));
        member = person(own, Sex::Male, 25, "Ivan");
        REQUIRE(world.set_dynasty_head(own, member));
    }

    CandidateQuery query() const {
        CandidateQuery q;
        q.member = member;
        q.names = &names;
        return q;
    }

    void status(CharacterId id, std::uint8_t flags) {
        family::PersonStatus row;
        row.character = id;
        row.flags = flags;
        if ((flags & family::kPersonTonsured) != 0)
            row.tonsured_on = world.today();
        if ((flags & family::kPersonDisowned) != 0)
            row.disowned_on = world.today();
        REQUIRE(world.set_family_status(row));
    }
};

const MarriageCandidate& row_for(const CandidateList& list, CharacterId id) {
    const auto it =
        std::find_if(list.rows.begin(), list.rows.end(), [id](const auto& row) { return row.person == id; });
    REQUIRE(it != list.rows.end());
    return *it;
}

bool has(const MarriageDenials& denials, MarriageDenial reason) {
    return std::find(denials.begin(), denials.end(), reason) != denials.end();
}

void expect_people(const CandidateList& list, std::initializer_list<CharacterId> expected) {
    std::vector<CharacterId> actual;
    for (const auto& row : list.rows)
        actual.push_back(row.person);
    auto wanted = std::vector<CharacterId>(expected);
    std::sort(actual.begin(), actual.end());
    std::sort(wanted.begin(), wanted.end());
    CHECK(actual == wanted);
}

class OtherFaith final : public FaithPolicy {
public:
    explicit OtherFaith(DynastyId house) : house_(house) {}
    std::string_view faith_of(DynastyId house) const noexcept override {
        return house == house_ ? "catholic" : "orthodox";
    }

private:
    DynastyId house_;
};

}  // namespace

TEST_CASE("marriage candidates: base pool and invalid queries") {
    Scene s;
    CHECK(find_candidates(s.world, s.rules, s.query()) == CandidateList{});
    const auto woman = s.person(s.other, Sex::Female);
    const auto same_house = s.person(s.own, Sex::Female);
    const auto man = s.person(s.other, Sex::Male);
    const auto dead = s.person(s.other, Sex::Female);
    REQUIRE(s.world.die(dead, CauseOfDeath::Illness));
    auto q = s.query();
    const auto list = find_candidates(s.world, s.rules, q);
    expect_people(list, {woman, same_house});
    CHECK(has(row_for(list, same_house).denials, MarriageDenial::SameHouse));
    q.sex = Sex::Male;
    const auto males = find_candidates(s.world, s.rules, q);
    expect_people(males, {man});
    CHECK(has(row_for(males, man).denials, MarriageDenial::SameSex));
    for (const auto member : {CharacterId{}, CharacterId(999999), dead}) {
        q.member = member;
        CHECK(find_candidates(s.world, s.rules, q) == CandidateList{});
    }
    World legacy(Date::from_ymd(1703, 1, 1), 301);
    q.member = legacy.create_character({});
    CHECK(find_candidates(legacy, s.rules, q) == CandidateList{});
    World empty(Date::from_ymd(1703, 1, 1), 301);
    REQUIRE(empty.enable_dynasty_mode());
    q.member = empty.create_character({});
    CHECK(find_candidates(empty, s.rules, q) == CandidateList{});
}

TEST_CASE("marriage candidates: name OR house search folds ASCII and Russian letters") {
    Scene s;
    const auto anna = s.person(s.other, Sex::Female, 20, "Anna");
    const auto elena = s.person(s.third, Sex::Female, 20, "ЕлЁна");
    const auto unnamed = s.person(s.third, Sex::Female);
    auto q = s.query();
    for (const auto text : {"aNn", "ДОЛГОРУК"}) {
        q.text = text;
        expect_people(find_candidates(s.world, s.rules, q), {anna});
    }
    q.text = "еЛё";
    expect_people(find_candidates(s.world, s.rules, q), {elena});
    q.text = "ORLOV";
    expect_people(find_candidates(s.world, s.rules, q), {elena, unnamed});
    q.names = nullptr;
    CHECK(find_candidates(s.world, s.rules, q).rows.empty());
    q.text = {};
    CHECK(find_candidates(s.world, s.rules, q).total == 3);
    q.names = &s.names;
    q.text = "nobody";
    CHECK(find_candidates(s.world, s.rules, q).rows.empty());
    CharacterInit bad_name;
    bad_name.dynasty = s.other;
    bad_name.sex = Sex::Female;
    bad_name.birth_date = Date::from_ymd(1680, 1, 1);
    bad_name.name = pat::core::StringId(999999);
    const auto invalid_name = s.world.create_character(bad_name);
    REQUIRE(invalid_name.valid());
    CHECK(find_candidates(s.world, s.rules, q).rows.empty());
    q.text = "долгоруков";
    expect_people(find_candidates(s.world, s.rules, q), {anna, invalid_name});
}

TEST_CASE("marriage candidates: inclusive age rank house and faith filters compose") {
    Scene s;
    s.world.add_honor(s.other, 100);
    s.world.add_honor(s.third, 500);
    const auto low = s.person(s.other, Sex::Female, 18);
    const auto high = s.person(s.third, Sex::Female, 30);
    s.person(s.other, Sex::Female, 17);
    s.person(s.third, Sex::Female, 31);
    auto q = s.query();
    q.min_age = 18;
    q.max_age = 30;
    q.min_rank = 2;
    q.max_rank = 3;
    expect_people(find_candidates(s.world, s.rules, q), {low, high});
    q.house = s.other;
    q.min_rank = q.max_rank = 2;
    expect_people(find_candidates(s.world, s.rules, q), {low});
    q.house = DynastyId(999999);
    CHECK(find_candidates(s.world, s.rules, q).rows.empty());
    q.house = {};
    q.min_rank = 3;
    q.max_rank = 2;
    CHECK(find_candidates(s.world, s.rules, q).rows.empty());
    q.min_rank.reset();
    q.max_rank.reset();
    q.min_age = 31;
    q.max_age = 18;
    CHECK(find_candidates(s.world, s.rules, q).rows.empty());
    q.min_age = 18;
    q.max_age = 30;
    OtherFaith policy(s.third);
    q.faith_policy = &policy;
    q.faith = "catholic";
    const auto catholic = find_candidates(s.world, s.rules, q);
    expect_people(catholic, {high});
    CHECK(row_for(catholic, high).faith == "catholic");
    CHECK(has(row_for(catholic, high).denials, MarriageDenial::FaithMismatch));
    q.faith_policy = nullptr;
    CHECK(find_candidates(s.world, s.rules, q).rows.empty());
    q.faith = "orthodox";
    expect_people(find_candidates(s.world, s.rules, q), {low, high});
}

TEST_CASE("marriage candidates: full denials badge and counts precede availability") {
    Scene s;
    const auto free = s.person(s.other, Sex::Female);
    const auto foreign_disowned = s.person(s.third, Sex::Female);
    s.status(foreign_disowned, family::kPersonDisowned);
    const auto blocked = s.person(s.other, Sex::Female, 12);
    const auto husband = s.person(s.third, Sex::Male);
    REQUIRE(s.world.marry(husband, blocked));
    s.status(blocked, family::kPersonTonsured | family::kPersonDisowned);
    auto q = s.query();
    const auto all = find_candidates(s.world, s.rules, q);
    CHECK(all.total == 3);
    CHECK(all.available_count == 2);
    CHECK(row_for(all, foreign_disowned).available());
    CHECK(row_for(all, blocked).denial == MarriageDenial::AlreadyMarried);
    CHECK(row_for(all, blocked).denials.size() >= 3);
    for (const auto& row : all.rows) {
        MarriageCheckOptions options;
        options.actor = s.own;
        CHECK(row.denials == check_marriage(s.world, s.rules, s.member, row.person, options));
        CHECK(row.denial == candidate_denial(row.denials));
    }
    q.availability = CandidateAvailability::Available;
    const auto available = find_candidates(s.world, s.rules, q);
    expect_people(available, {free, foreign_disowned});
    CHECK(available.total == all.total);
    CHECK(available.available_count == all.available_count);
    q.availability = CandidateAvailability::Unavailable;
    const auto unavailable = find_candidates(s.world, s.rules, q);
    expect_people(unavailable, {blocked});
    CHECK(unavailable.total == all.total);
    CHECK(unavailable.available_count == all.available_count);
    s.status(s.member, family::kPersonDisowned);
    q.availability = CandidateAvailability::All;
    for (const auto& row : find_candidates(s.world, s.rules, q).rows)
        CHECK(has(row.denials, MarriageDenial::Disowned));
}

TEST_CASE("marriage candidates: engagement mourning pregnancy and policy retain complete checks") {
    Scene s;
    const auto bride = s.person(s.other, Sex::Female);
    const auto partner = s.person(s.third, Sex::Male);
    family::Betrothal contract;
    contract.groom = partner;
    contract.bride = bride;
    contract.groom_house = s.third;
    contract.bride_house = s.other;
    contract.contracted = s.world.today();
    contract.wedding_not_before = contract.contracted + 30;
    contract.wedding_deadline = contract.contracted + 120;
    contract.dowry_source = s.other;
    contract.origin = family::BetrothalOrigin::Command;
    REQUIRE(s.world.create_family_betrothal(contract).has_value());
    family::PersonStatus status;
    status.character = bride;
    status.widowed_on = s.world.today();
    REQUIRE(s.world.set_family_status(status));
    Pregnancy pregnancy;
    pregnancy.mother = bride;
    pregnancy.father = partner;
    pregnancy.conceived = s.world.today() - 1;
    pregnancy.due = s.world.today() + 269;
    REQUIRE(s.world.begin_pregnancy(pregnancy));
    OtherFaith policy(s.other);
    auto q = s.query();
    q.faith_policy = &policy;
    auto list = find_candidates(s.world, s.rules, q);
    const auto& row = row_for(list, bride);
    MarriageCheckOptions options;
    options.actor = s.own;
    CHECK(row.denials == check_marriage(s.world, s.rules, s.member, bride, options, &policy));
    CHECK(row.denial == MarriageDenial::Betrothed);
    CHECK(has(row.denials, MarriageDenial::Mourning));
    CHECK(has(row.denials, MarriageDenial::Pregnant));
    CHECK(has(row.denials, MarriageDenial::FaithMismatch));

    const auto negotiating = s.person(s.third, Sex::Female);
    const auto envoy_groom = s.person(s.other, Sex::Male);
    family::Negotiation envoy;
    envoy.initiator = s.other;
    envoy.groom = envoy_groom;
    envoy.bride = negotiating;
    envoy.groom_house = s.other;
    envoy.bride_house = s.third;
    envoy.sent = s.world.today();
    envoy.answer_due = envoy.sent + 30;
    envoy.dowry_source = s.third;
    REQUIRE(s.world.create_family_negotiation(envoy).has_value());
    list = find_candidates(s.world, s.rules, q);
    const auto& pending = row_for(list, negotiating);
    CHECK(pending.denials == check_marriage(s.world, s.rules, s.member, negotiating, options, &policy));
    CHECK(pending.denial == MarriageDenial::InNegotiation);
}

TEST_CASE("marriage candidates: kinship filter detects blood affinity and spiritual relations") {
    Scene s;
    const auto father = s.person(s.third, Sex::Male, 60);
    s.member = s.person(s.own, Sex::Male, 25, {}, father);
    const auto sister = s.person(s.other, Sex::Female, 20, {}, father);
    const auto goddaughter = s.person(s.other, Sex::Female);
    REQUIRE(s.world.set_character_godparents(goddaughter, {s.member, {}}));
    const auto spouse = s.person(s.third, Sex::Female, 45);
    const auto stepdaughter = s.person(s.other, Sex::Female, 18, {}, {}, spouse);
    const auto free = s.person(s.other, Sex::Female);
    REQUIRE(s.world.marry(s.member, spouse));
    auto q = s.query();
    const auto all = find_candidates(s.world, s.rules, q);
    CHECK(row_for(all, sister).kinship.blood_degree == 2);
    CHECK(row_for(all, goddaughter).kinship.spiritual);
    CHECK(row_for(all, stepdaughter).kinship.affinity_degree == 1);
    q.unrelated_only = true;
    const auto list = find_candidates(s.world, s.rules, q);
    CHECK(std::none_of(list.rows.begin(), list.rows.end(), [&](const auto& row) {
        return row.person == sister || row.person == goddaughter || row.person == stepdaughter;
    }));
    CHECK(row_for(list, free).kinship == KinResult{});
    for (const auto& row : list.rows)
        CHECK(row.kinship == KinResult{});
}

TEST_CASE("marriage candidates: betrothal and dispensation options reach the full check") {
    Scene s;
    const auto young = s.person(s.other, Sex::Female, 10);
    auto q = s.query();
    CHECK(has(row_for(find_candidates(s.world, s.rules, q), young).denials, MarriageDenial::BrideTooYoung));
    q.as_betrothal = true;
    CHECK(row_for(find_candidates(s.world, s.rules, q), young).available());
    const auto founder = s.person(s.third, Sex::Male, 100);
    auto ancestor = founder;
    for (int step = 1; step < 5; ++step)
        ancestor = s.person(step == 4 ? s.own : s.third, Sex::Male, 100 - 16 * step, {}, ancestor);
    s.member = ancestor;
    const auto cousin = s.person(s.other, Sex::Female, 80, {}, founder);
    q = s.query();
    const auto ordinary = find_candidates(s.world, s.rules, q);
    CHECK(row_for(ordinary, cousin).dispensable);
    CHECK(has(row_for(ordinary, cousin).denials, MarriageDenial::BloodKinshipDispensable));
    q.dispensation = true;
    const auto permitted = find_candidates(s.world, s.rules, q);
    CHECK(row_for(permitted, cousin).dispensable);
    CHECK(row_for(permitted, cousin).available());
    MarriageCheckOptions options;
    options.actor = s.own;
    options.dispensation = true;
    CHECK(row_for(permitted, cousin).denials == check_marriage(s.world, s.rules, s.member, cousin, options));
}

TEST_CASE("marriage candidates: score forecast sorting and purity compose with T05") {
    Scene s;
    s.world.add_honor(s.other, 340);
    s.world.add_honor(s.third, 1000);
    const auto first = s.person(s.other, Sex::Female, 20);
    const auto tie = s.person(s.other, Sex::Female, 20);
    const auto different = s.person(s.third, Sex::Female, 30);
    const auto observer = s.person(s.own, Sex::Male, 30);
    REQUIRE(s.world.set_dynasty_head(s.other, first));
    REQUIRE(s.world.set_dynasty_head(s.third, different));
    REQUIRE(s.world.add_opinion(first, s.member, pat::core::Fixed32::from_int(40), 1));
    REQUIRE(s.world.add_opinion(observer, different, pat::core::Fixed32::from_int(-40), 1));
    auto q = s.query();
    static_cast<void>(s.world.rng().stream("marriage.purity").next_u32());
    const auto rng = s.world.rng().save();
    const auto bytes = serialize_save(s.world);
    const auto hash = s.world.compute_hash();
    const auto names_size = s.names.size();
    const auto all = find_candidates(s.world, s.rules, q);
    CHECK(all == find_candidates(s.world, s.rules, q));
    for (const auto& row : all.rows) {
        MatchContext context;
        context.initiator = s.own;
        context.relation = dynasty_relation(s.world, s.own, row.house, s.rules);
        CHECK(row.relation == context.relation);
        const auto dowry = standard_dowry(s.world, s.rules, row.house);
        const auto evaluation = evaluate_match(s.world, s.rules, s.member, row.person, dowry, context);
        REQUIRE(evaluation.has_value());
        CHECK(row.dowry_kop == dowry);
        CHECK(row.score == accept_score(*evaluation, MatchSide::Groom));
        CHECK(row.match_value == row.score.score);
        CHECK(row.forecast == forecast_of(s.rules, accept_score(*evaluation, MatchSide::Bride)));
        economy::Breakdown explanation;
        explain_accept_score(row.score, explanation);
        CHECK(explanation.is_consistent());
    }
    CHECK(row_for(all, different).match_value > row_for(all, first).match_value);
    CHECK(row_for(all, first).match_value == row_for(all, tie).match_value);
    CHECK(row_for(all, first).relation > row_for(all, different).relation);
    struct ExpectedOrder {
        CandidateSort sort;
        bool ascending;
        std::array<CharacterId, 3> people;
    };
    const std::array orders{
        ExpectedOrder{CandidateSort::MatchValue, false, {different, first, tie}},
        ExpectedOrder{CandidateSort::MatchValue, true, {first, tie, different}},
        ExpectedOrder{CandidateSort::Relation, false, {first, tie, different}},
        ExpectedOrder{CandidateSort::Relation, true, {different, first, tie}},
        ExpectedOrder{CandidateSort::Age, false, {different, first, tie}},
        ExpectedOrder{CandidateSort::Age, true, {first, tie, different}},
    };
    for (const auto& order : orders) {
        q.sort = order.sort;
        q.ascending = order.ascending;
        std::vector<CharacterId> actual;
        for (const auto& row : find_candidates(s.world, s.rules, q).rows) {
            actual.push_back(row.person);
        }
        CHECK(actual == std::vector<CharacterId>(order.people.begin(), order.people.end()));
    }
    CHECK(s.world.compute_hash() == hash);
    CHECK(s.world.rng().save() == rng);
    CHECK(serialize_save(s.world) == bytes);  // Includes every named RNG stream and its draw state.
    CHECK(s.names.size() == names_size);
    auto loaded = deserialize_save(bytes);
    REQUIRE(loaded.has_value());
    q = s.query();
    CHECK(find_candidates(loaded.value().world, s.rules, q) == all);
    q.member = first;  // A proposing woman sees the bride's score and the groom's forecast.
    const auto female_list = find_candidates(s.world, s.rules, q);
    const auto& reverse = row_for(female_list, s.member);
    MatchContext reverse_context;
    reverse_context.initiator = s.other;
    reverse_context.relation = dynasty_relation(s.world, s.other, s.own, s.rules);
    const auto reverse_dowry = standard_dowry(s.world, s.rules, s.other);
    const auto reverse_evaluation =
        evaluate_match(s.world, s.rules, s.member, first, reverse_dowry, reverse_context);
    REQUIRE(reverse_evaluation.has_value());
    CHECK(reverse.dowry_kop == reverse_dowry);
    CHECK(reverse.score == accept_score(*reverse_evaluation, MatchSide::Bride));
    CHECK(reverse.match_value == reverse.score.score);
    CHECK(reverse.forecast == forecast_of(s.rules, accept_score(*reverse_evaluation, MatchSide::Groom)));
}

TEST_CASE("marriage candidates: batched kinship and denials match standalone retained-family queries") {
    Scene s;
    // The founder is reached through both parents, at different depths. The shortest
    // member-to-cousin path is deliberately not the first paternal path.
    const auto founder = s.person(s.third, Sex::Male, 120);
    const auto long_branch = s.person(s.third, Sex::Male, 95, {}, founder);
    const auto father = s.person(s.own, Sex::Male, 70, {}, long_branch);
    const auto mother = s.person(s.third, Sex::Female, 65, {}, founder);
    const auto member = s.person(s.own, Sex::Male, 40, {}, father, mother);
    const auto cousin = s.person(s.other, Sex::Female, 45, {}, founder);

    const auto spouse_father = s.person(s.other, Sex::Male, 80);
    const auto spouse = s.person(s.other, Sex::Female, 38, {}, spouse_father);
    const auto sister_in_law = s.person(s.other, Sex::Female, 42, {}, spouse_father);
    const auto former_father = s.person(s.third, Sex::Male, 85);
    const auto former_spouse = s.person(s.third, Sex::Female, 43, {}, former_father);
    const auto former_sister = s.person(s.other, Sex::Female, 46, {}, former_father);
    REQUIRE(s.world.marry(member, former_spouse));
    REQUIRE(s.world.end_marriage(member));
    REQUIRE(s.world.marry(member, spouse));

    // Affinity also arrives from the candidate's current or archived former spouse.
    const auto current_husband = s.person(s.third, Sex::Male, 42, {}, father);
    const auto current_wife = s.person(s.other, Sex::Female, 40);
    REQUIRE(s.world.marry(current_husband, current_wife));
    const auto former_husband = s.person(s.third, Sex::Male, 44, {}, father);
    const auto former_wife = s.person(s.other, Sex::Female, 41);
    REQUIRE(s.world.marry(former_husband, former_wife));
    REQUIRE(s.world.end_marriage(former_husband));

    const auto old_godparent = s.person(s.third, Sex::Male, 90);
    const auto spiritual_peer = s.person(s.other, Sex::Female, 39);
    REQUIRE(s.world.set_character_godparents(member, {old_godparent, {}}));
    REQUIRE(s.world.set_character_godparents(spiritual_peer, {old_godparent, {}}));
    const auto godmother = s.person(s.other, Sex::Female, 60);
    const auto member_child = s.person(s.own, Sex::Male, 18, {}, member);
    REQUIRE(s.world.set_character_godparents(member_child, {godmother, {}}));
    const auto godchild_mother = s.person(s.other, Sex::Female, 40);
    const auto godchild = s.person(s.third, Sex::Male, 16, {}, {}, godchild_mother);
    REQUIRE(s.world.set_character_godparents(godchild, {member, {}}));
    for (const auto archived : {founder,
                                long_branch,
                                former_father,
                                former_spouse,
                                former_husband,
                                old_godparent,
                                member_child,
                                godchild}) {
        REQUIRE(s.world.die(archived, CauseOfDeath::Illness));
    }
    s.status(spiritual_peer, family::kPersonTonsured | family::kPersonDisowned);

    // Explicit expectations ensure the fixture exercises retained facts, rather than merely
    // comparing two implementations on a collection of unrelated people.
    CHECK(kinship_for_marriage(s.world, member, cousin).blood_degree == 3);
    for (const auto related : {sister_in_law, former_sister, current_wife, former_wife})
        CHECK(kinship_for_marriage(s.world, member, related).affinity_degree == 2);
    for (const auto related : {spiritual_peer, godmother, godchild_mother})
        CHECK(kinship_for_marriage(s.world, member, related).spiritual);

    OtherFaith policy(s.other);
    for (const auto proposer : {member, cousin, current_wife, spiritual_peer}) {
        for (const bool as_betrothal : {false, true}) {
            for (const bool dispensation : {false, true}) {
                for (const auto sex : {Sex::Male, Sex::Female}) {
                    auto q = s.query();
                    q.member = proposer;
                    q.sex = sex;
                    q.as_betrothal = as_betrothal;
                    q.dispensation = dispensation;
                    q.faith_policy = &policy;
                    const auto list = find_candidates(s.world, s.rules, q);
                    REQUIRE_FALSE(list.rows.empty());
                    MarriageCheckOptions options;
                    options.actor = s.world.characters().dynasty[proposer.index()];
                    options.as_betrothal = as_betrothal;
                    options.dispensation = dispensation;
                    for (const auto& row : list.rows) {
                        CAPTURE(proposer.index());
                        CAPTURE(row.person.index());
                        CHECK(row.kinship == kinship_for_marriage(s.world, proposer, row.person));
                        CHECK(row.denials ==
                              check_marriage(s.world, s.rules, proposer, row.person, options, &policy));
                    }
                }
            }
        }
    }
}

TEST_CASE("marriage candidates: same-house reuse preserves each person's quality age and decision") {
    for (const bool female_proposer : {false, true}) {
        Scene s;
        const auto proposer = female_proposer ? s.person(s.own, Sex::Female, 25) : s.member;
        const auto candidate_sex = female_proposer ? Sex::Male : Sex::Female;
        const auto candidate = [&](int age, int quality, int skill) {
            CharacterInit init;
            init.dynasty = s.other;
            init.sex = candidate_sex;
            init.birth_date = Date::from_ymd(1703 - age, 1, 1);
            init.health = pat::core::Fixed32::from_int(quality);
            init.fertility = pat::core::Fixed32::from_int(quality);
            init.skills.fill(pat::core::Fixed32::from_int(skill));
            const auto id = s.world.create_character(init);
            REQUIRE(id.valid());
            return id;
        };
        // Living-id traversal encounters a refusal before an acceptance in the same house.
        const auto old = candidate(75, 0, 0);
        const auto young = candidate(25, 100, 20);
        const auto middle = candidate(40, 50, 8);
        REQUIRE(old < young);
        REQUIRE(young < middle);
        auto q = s.query();
        q.member = proposer;
        q.house = s.other;
        const auto list = find_candidates(s.world, s.rules, q);
        REQUIRE(list.rows.size() == 3);
        const auto own_side = female_proposer ? MatchSide::Bride : MatchSide::Groom;
        const auto answering_side = female_proposer ? MatchSide::Groom : MatchSide::Bride;
        for (const auto& row : list.rows) {
            const auto groom = female_proposer ? row.person : proposer;
            const auto bride = female_proposer ? proposer : row.person;
            const auto bride_house = female_proposer ? s.own : s.other;
            const auto dowry = standard_dowry(s.world, s.rules, bride_house);
            MatchContext context;
            context.initiator = s.own;
            context.relation = dynasty_relation(s.world, s.own, s.other, s.rules);
            const auto full = evaluate_match(s.world, s.rules, groom, bride, dowry, context);
            REQUIRE(full.has_value());
            CHECK(row.score == accept_score(*full, own_side));
            CHECK(row.match_value == row.score.score);
            CHECK(row.forecast == forecast_of(s.rules, accept_score(*full, answering_side)));
        }
        const auto& refused = row_for(list, old);
        const auto& accepted = row_for(list, young);
        const auto& intermediate = row_for(list, middle);
        CHECK_FALSE(refused.score.accepted);
        CHECK(refused.score.refusal_code != 0);
        CHECK(accepted.score.accepted);
        CHECK(accepted.score.refusal_code == 0);
        CHECK(refused.match_value < intermediate.match_value);
        CHECK(intermediate.match_value < accepted.match_value);
        CHECK(refused.score.health_part != accepted.score.health_part);
        CHECK(refused.score.skills_part != accepted.score.skills_part);
        CHECK(refused.score.terms[static_cast<std::size_t>(ScoreTerm::AgeGap)] <
              accepted.score.terms[static_cast<std::size_t>(ScoreTerm::AgeGap)]);
        CHECK(refused.forecast != accepted.forecast);
    }
}
