#include "pat/sim/command.hpp"
#include "pat/sim/family/tables.hpp"
#include "pat/sim/marriage_check.hpp"
#include "pat/sim/marriage_rules.hpp"
#include "pat/sim/world.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <string_view>

namespace {

using namespace pat::sim;
using Denial = MarriageDenial;

struct Scene {
    World world;
    DynastyId groom_house;
    DynastyId bride_house;
    DynastyId third_house;
    CharacterId groom;
    CharacterId bride;

    CharacterId person(DynastyId house,
                       Sex sex,
                       Date birth = Date::from_ymd(1670, 1, 1),
                       CharacterId father = {},
                       CharacterId mother = {}) {
        CharacterInit init;
        init.dynasty = house;
        init.sex = sex;
        init.birth_date = birth;
        init.father = father;
        init.mother = mother;
        const auto id = world.create_character(init);
        REQUIRE(id.valid());
        return id;
    }

    explicit Scene(Date today = Date::from_ymd(1703, 5, 20),
                   Date groom_birth = Date::from_ymd(1670, 1, 1),
                   Date bride_birth = Date::from_ymd(1670, 1, 1))
        : world(today, 299) {
        REQUIRE(world.enable_dynasty_mode());
        groom_house = world.create_dynasty({});
        bride_house = world.create_dynasty({});
        third_house = world.create_dynasty({});
        groom = person(groom_house, Sex::Male, groom_birth);
        bride = person(bride_house, Sex::Female, bride_birth);
    }
};

MarriageDenials query(const Scene& scene,
                      CharacterId a,
                      CharacterId b,
                      MarriageCheckOptions options = {},
                      const FaithPolicy* faith = nullptr,
                      const MarriageRules& rules = MarriageRules::defaults()) {
    const WorldHash before = scene.world.compute_hash();
    const auto family_before = scene.world.family();
    const auto result = check_marriage(scene.world, rules, a, b, options, faith);
    CHECK(scene.world.compute_hash() == before);
    CHECK(scene.world.family() == family_before);
    CHECK(std::is_sorted(result.begin(), result.end()));
    CHECK(std::adjacent_find(result.begin(), result.end()) == result.end());
    return result;
}

MarriageDenials query(const Scene& scene,
                      MarriageCheckOptions options = {},
                      const FaithPolicy* faith = nullptr,
                      const MarriageRules& rules = MarriageRules::defaults()) {
    return query(scene, scene.groom, scene.bride, options, faith, rules);
}

bool has(const MarriageDenials& reasons, Denial reason) {
    return std::find(reasons.begin(), reasons.end(), reason) != reasons.end();
}

void expect_reasons(const MarriageDenials& actual, std::initializer_list<Denial> expected) {
    const MarriageDenials wanted(expected);
    CHECK(actual == wanted);
}

void status(Scene& scene, CharacterId person, std::uint8_t flags) {
    family::PersonStatus row;
    row.character = person;
    row.flags = flags;
    if ((flags & family::kPersonTonsured) != 0) {
        row.tonsured_on = scene.world.today();
    }
    if ((flags & family::kPersonDisowned) != 0) {
        row.disowned_on = scene.world.today();
    }
    REQUIRE(scene.world.set_family_status(row));
}

void previous_marriage(Scene& scene, CharacterId person, Sex partner_sex) {
    const auto spouse = scene.person(scene.third_house, partner_sex);
    REQUIRE(scene.world.marry(person, spouse));
    REQUIRE(scene.world.end_marriage(person));
}

class FixedFaith final : public FaithPolicy {
public:
    explicit FixedFaith(bool allow) : allow_(allow) {}

    bool allows(const World& world, CharacterId a, CharacterId b) const override {
        ++calls;
        CHECK(world.exists(a));
        CHECK(world.exists(b));
        return allow_;
    }

    mutable int calls = 0;

private:
    bool allow_;
};

// The side branch has one edge to the founder; the male branch has `degree - 1` edges.
// Every participant is at least 17 and every parent is created before its child.
struct BloodScene : Scene {
    CharacterId founder;

    explicit BloodScene(int degree) {
        founder = person(third_house, Sex::Male, Date::from_ymd(1590, 1, 1));
        CharacterId ancestor = founder;
        for (int step = 1; step < degree; ++step) {
            const auto house = step == degree - 1 ? groom_house : third_house;
            ancestor = person(house, Sex::Male, Date::from_ymd(1590 + 16 * step, 1, 1), ancestor);
        }
        groom = ancestor;
        bride = person(bride_house, Sex::Female, Date::from_ymd(1606, 1, 1), founder);
    }
};

}  // namespace

TEST_CASE("marriage check: unknown ids are guarded and unrelated adults are eligible") {
    Scene scene;
    expect_reasons(query(scene), {});
    FixedFaith policy(true);
    expect_reasons(query(scene, CharacterId(), scene.bride, {}, &policy), {Denial::NotAlive});
    expect_reasons(query(scene, scene.groom, CharacterId(999'999), {}, &policy), {Denial::NotAlive});
    CHECK(policy.calls == 0);
    const auto same = query(scene, scene.groom, scene.groom);
    CHECK(has(same, Denial::SamePerson));
    CHECK(has(same, Denial::SameSex));
    CHECK(has(same, Denial::SameHouse));
    const auto man = scene.person(scene.bride_house, Sex::Male);
    expect_reasons(query(scene, scene.groom, man), {Denial::SameSex});
    const auto member = scene.person(scene.groom_house, Sex::Female);
    expect_reasons(query(scene, scene.groom, member), {Denial::SameHouse});
}

TEST_CASE("marriage check: known archived people retain their independent refusal facts") {
    Scene scene(Date::from_ymd(1703, 5, 20), Date::from_ymd(1688, 5, 21));
    for (int i = 0; i < 3; ++i) {
        previous_marriage(scene, scene.groom, Sex::Female);
    }
    status(scene, scene.groom, family::kPersonTonsured | family::kPersonDisowned);
    REQUIRE(scene.world.die(scene.groom, CauseOfDeath::Illness));
    const auto reasons = query(scene);
    CHECK(has(reasons, Denial::NotAlive));
    CHECK(has(reasons, Denial::GroomTooYoung));
    CHECK(has(reasons, Denial::Tonsured));
    CHECK(has(reasons, Denial::Disowned));
    CHECK(has(reasons, Denial::MarriageLimit));
    CHECK_FALSE(has(reasons, Denial::PreviouslyMarried));
}

TEST_CASE("marriage check: exact age boundaries and sex order use calendar birthdays") {
    const auto today = Date::from_ymd(1703, 5, 20);
    for (int age : {14, 15, 16}) {
        CAPTURE(age);
        Scene groom(today, Date::from_ymd(1703 - age, 5, 20));
        expect_reasons(query(groom),
                       age < 16 ? std::initializer_list<Denial>{Denial::GroomTooYoung}
                                : std::initializer_list<Denial>{});
        Scene bride(today, Date::from_ymd(1670, 1, 1), Date::from_ymd(1703 - age, 5, 20));
        expect_reasons(query(bride),
                       age < 15 ? std::initializer_list<Denial>{Denial::BrideTooYoung}
                                : std::initializer_list<Denial>{});
    }
    Scene before(today, Date::from_ymd(1687, 5, 21), Date::from_ymd(1688, 5, 21));
    expect_reasons(query(before), {Denial::GroomTooYoung, Denial::BrideTooYoung});
    CHECK(query(before) == query(before, before.bride, before.groom));
    Scene exact(today, Date::from_ymd(1687, 5, 20), Date::from_ymd(1688, 5, 20));
    expect_reasons(query(exact), {});
    CHECK(query(exact) == query(exact, exact.bride, exact.groom));

    for (int day : {27, 28}) {
        Scene leap_bride(
            Date::from_ymd(1703, 2, day), Date::from_ymd(1670, 1, 1), Date::from_ymd(1688, 2, 29));
        CHECK(has(query(leap_bride), Denial::BrideTooYoung) == (day == 27));
    }
    for (int day : {28, 29}) {
        Scene leap_groom(Date::from_ymd(1704, 2, day), Date::from_ymd(1688, 2, 29));
        CHECK(has(query(leap_groom), Denial::GroomTooYoung) == (day == 28));
    }
}

TEST_CASE("marriage check: betrothal uses seven years instead of wedding ages") {
    MarriageCheckOptions options;
    options.as_betrothal = true;
    for (const bool groom_is_younger : {false, true}) {
        Scene below(Date::from_ymd(1703, 5, 20),
                    Date::from_ymd(1696, 5, groom_is_younger ? 21 : 20),
                    Date::from_ymd(1696, 5, groom_is_younger ? 20 : 21));
        expect_reasons(query(below, options), {Denial::TooYoungToBetroth});
    }
    Scene exact(Date::from_ymd(1703, 5, 20), Date::from_ymd(1696, 5, 20), Date::from_ymd(1696, 5, 20));
    expect_reasons(query(exact, options), {});
    expect_reasons(query(exact), {Denial::GroomTooYoung, Denial::BrideTooYoung});
}

TEST_CASE("marriage check: eligibility thresholds and pair restrictions come from rules") {
    Scene scene;
    auto rules = MarriageRules::defaults();
    rules.eligibility.min_marriage_age_male = 40;
    rules.eligibility.min_marriage_age_female = 40;
    expect_reasons(query(scene, {}, nullptr, rules), {Denial::GroomTooYoung, Denial::BrideTooYoung});
    const auto man = scene.person(scene.groom_house, Sex::Male);
    rules = MarriageRules::defaults();
    rules.eligibility.require_opposite_sex = false;
    rules.eligibility.forbid_same_house = false;
    expect_reasons(query(scene, scene.groom, man, {}, nullptr, rules), {});
}

TEST_CASE("marriage check: marriage history allows a third union and forbids the fourth") {
    Scene scene;
    for (int marriages = 0; marriages < 3; ++marriages) {
        previous_marriage(scene, scene.groom, Sex::Female);
        CHECK(has(query(scene), Denial::MarriageLimit) == (marriages == 2));
    }
    Scene same_pair;
    REQUIRE(same_pair.world.marry(same_pair.groom, same_pair.bride));
    CHECK(has(query(same_pair), Denial::AlreadyMarried));
    REQUIRE(same_pair.world.end_marriage(same_pair.groom));
    const auto ended = query(same_pair);
    CHECK(has(ended, Denial::PreviouslyMarried));
    CHECK_FALSE(has(ended, Denial::AlreadyMarried));
    CHECK_FALSE(has(ended, Denial::MarriageLimit));
}

TEST_CASE("marriage check: both participant positions expose engagements without duplicate reasons") {
    Scene negotiating;
    family::Negotiation envoy;
    envoy.initiator = negotiating.groom_house;
    envoy.groom = negotiating.groom;
    envoy.bride = negotiating.bride;
    envoy.groom_house = negotiating.groom_house;
    envoy.bride_house = negotiating.bride_house;
    envoy.sent = negotiating.world.today();
    envoy.answer_due = envoy.sent + 30;
    envoy.dowry_source = negotiating.bride_house;
    REQUIRE(negotiating.world.create_family_negotiation(envoy).has_value());
    expect_reasons(query(negotiating), {Denial::InNegotiation});
    CHECK(query(negotiating) == query(negotiating, negotiating.bride, negotiating.groom));

    Scene engaged;
    family::Betrothal contract;
    contract.groom = engaged.groom;
    contract.bride = engaged.bride;
    contract.groom_house = engaged.groom_house;
    contract.bride_house = engaged.bride_house;
    contract.contracted = engaged.world.today();
    contract.wedding_not_before = contract.contracted + 30;
    contract.wedding_deadline = contract.contracted + 120;
    contract.dowry_source = engaged.bride_house;
    contract.origin = family::BetrothalOrigin::Command;
    REQUIRE(engaged.world.create_family_betrothal(contract).has_value());
    expect_reasons(query(engaged), {Denial::Betrothed});
    CHECK(query(engaged) == query(engaged, engaged.bride, engaged.groom));
}

TEST_CASE("marriage check: disownment belongs to the proposing house while tonsure always blocks") {
    Scene disowned;
    status(disowned, disowned.bride, family::kPersonDisowned);
    MarriageCheckOptions options;
    options.actor = disowned.groom_house;
    expect_reasons(query(disowned, options), {});
    options.actor = disowned.bride_house;
    expect_reasons(query(disowned, options), {Denial::Disowned});
    expect_reasons(query(disowned), {Denial::Disowned});
    options.actor = disowned.third_house;
    expect_reasons(query(disowned, options), {Denial::NotYourMember});
    options.actor = DynastyId(999);
    expect_reasons(query(disowned, options), {Denial::NotYourMember});
    Scene tonsured;
    status(tonsured, tonsured.bride, family::kPersonTonsured);
    options.actor = tonsured.groom_house;
    expect_reasons(query(tonsured, options), {Denial::Tonsured});
    options.actor = tonsured.bride_house;
    expect_reasons(query(tonsured, options), {Denial::Tonsured});
    expect_reasons(query(tonsured), {Denial::Tonsured});
}

TEST_CASE("marriage check: female and male mourning expire on their own exact day") {
    for (const bool female : {false, true}) {
        const int duration = female ? 365 : 180;
        for (int since : {0, duration - 1, duration, duration + 1}) {
            CAPTURE(female);
            CAPTURE(since);
            Scene scene;
            family::PersonStatus row;
            row.character = female ? scene.bride : scene.groom;
            row.widowed_on = scene.world.today() - since;
            REQUIRE(scene.world.set_family_status(row));
            CHECK(has(query(scene), Denial::Mourning) == (since < duration));
            CHECK(query(scene) == query(scene, scene.bride, scene.groom));
        }
    }
}

TEST_CASE("marriage check: pregnancy is independent of current spouse and parameter order") {
    Scene scene;
    Pregnancy pregnancy;
    pregnancy.mother = scene.bride;
    pregnancy.father = scene.groom;
    pregnancy.conceived = scene.world.today() - 1;
    pregnancy.due = scene.world.today() + 269;
    REQUIRE(scene.world.begin_pregnancy(pregnancy));
    expect_reasons(query(scene), {Denial::Pregnant});
    CHECK(query(scene) == query(scene, scene.bride, scene.groom));
}

TEST_CASE("marriage check: blood degrees four five six and seven have distinct permission boundaries") {
    MarriageCheckOptions permission;
    permission.dispensation = true;
    for (int degree : {4, 5, 6, 7}) {
        CAPTURE(degree);
        BloodScene scene(degree);
        const auto ordinary = query(scene);
        const auto permitted = query(scene, permission);
        if (degree == 4) {
            expect_reasons(ordinary, {Denial::BloodKinship});
            CHECK(permitted == ordinary);
        } else if (degree <= 6) {
            expect_reasons(ordinary, {Denial::BloodKinshipDispensable});
            expect_reasons(permitted, {});
        } else {
            expect_reasons(ordinary, {});
            CHECK(permitted == ordinary);
        }
    }
}

TEST_CASE("marriage check: dispensation removes only its blood reason from a complete refusal") {
    BloodScene scene(5);
    const auto former =
        scene.person(scene.third_house, Sex::Female, Date::from_ymd(1607, 1, 1), scene.founder);
    REQUIRE(scene.world.marry(scene.groom, former));
    REQUIRE(scene.world.end_marriage(scene.groom));
    REQUIRE(scene.world.set_character_godparents(scene.groom, {CharacterId(), scene.bride}));
    const std::array<std::string_view, 3> confessions{"orthodox", "lutheran", "orthodox"};
    const FaithPolicy faith(confessions);
    const auto ordinary = query(scene, {}, &faith);
    CHECK(has(ordinary, Denial::BloodKinshipDispensable));
    CHECK(has(ordinary, Denial::Affinity));
    CHECK(has(ordinary, Denial::SpiritualKinship));
    CHECK(has(ordinary, Denial::FaithMismatch));
    MarriageCheckOptions options;
    options.dispensation = true;
    auto expected = ordinary;
    std::erase(expected, Denial::BloodKinshipDispensable);
    CHECK(query(scene, options, &faith) == expected);
    CHECK_FALSE(expected.empty());
}

TEST_CASE("marriage check: church permission does not cure absolute or non-blood prohibitions") {
    MarriageCheckOptions permission;
    permission.dispensation = true;
    Scene spiritual;
    REQUIRE(spiritual.world.set_character_godparents(spiritual.groom, {CharacterId(), spiritual.bride}));
    expect_reasons(query(spiritual, permission), {Denial::SpiritualKinship});
    Scene affine;
    const auto father = affine.person(affine.third_house, Sex::Male, Date::from_ymd(1640, 1, 1));
    const auto former = affine.person(affine.third_house, Sex::Female, Date::from_ymd(1670, 1, 1), father);
    const auto sister = affine.person(affine.bride_house, Sex::Female, Date::from_ymd(1672, 1, 1), father);
    REQUIRE(affine.world.marry(affine.groom, former));
    REQUIRE(affine.world.end_marriage(affine.groom));
    expect_reasons(query(affine, affine.groom, sister, permission), {Denial::Affinity});
}

TEST_CASE("marriage check: faith policy resolves equal different empty and missing house entries") {
    Scene scene;
    expect_reasons(query(scene), {});
    for (const auto& confessions : std::array<std::array<std::string_view, 2>, 3>{
             std::array<std::string_view, 2>{"orthodox", "orthodox"},
             std::array<std::string_view, 2>{"catholic", "catholic"},
             std::array<std::string_view, 2>{"", "orthodox"}}) {
        const FaithPolicy faith(confessions);
        expect_reasons(query(scene, {}, &faith), {});
    }
    const std::array<std::string_view, 1> orthodox_only{"orthodox"};
    const FaithPolicy missing_orthodox(orthodox_only);
    expect_reasons(query(scene, {}, &missing_orthodox), {});
    const std::array<std::string_view, 1> lutheran_only{"lutheran"};
    const FaithPolicy missing_other(lutheran_only);
    expect_reasons(query(scene, {}, &missing_other), {Denial::FaithMismatch});
    MarriageCheckOptions permission;
    permission.dispensation = true;
    expect_reasons(query(scene, permission, &missing_other), {Denial::FaithMismatch});
    FixedFaith reject(false);
    expect_reasons(query(scene, {}, &reject), {Denial::FaithMismatch});
    CHECK(reject.calls == 1);
    FixedFaith allow(true);
    expect_reasons(query(scene, {}, &allow), {});
    CHECK(allow.calls == 1);
}

TEST_CASE("marriage check: complete reasons are unique and candidate priority preserves the list") {
    Scene scene(Date::from_ymd(1703, 5, 20), Date::from_ymd(1689, 1, 1), Date::from_ymd(1689, 1, 1));
    status(scene, scene.groom, family::kPersonTonsured | family::kPersonDisowned);
    status(scene, scene.bride, family::kPersonTonsured | family::kPersonDisowned);
    expect_reasons(query(scene),
                   {Denial::GroomTooYoung, Denial::BrideTooYoung, Denial::Tonsured, Denial::Disowned});
    MarriageDenials reasons{Denial::NotAlive,
                            Denial::GroomTooYoung,
                            Denial::AlreadyMarried,
                            Denial::Betrothed,
                            Denial::InNegotiation,
                            Denial::Tonsured,
                            Denial::Mourning,
                            Denial::BloodKinship,
                            Denial::FaithMismatch};
    for (const Denial first : {Denial::Betrothed,
                               Denial::InNegotiation,
                               Denial::AlreadyMarried,
                               Denial::Mourning,
                               Denial::Tonsured,
                               Denial::NotAlive}) {
        const auto before = reasons;
        CHECK(candidate_denial(reasons) == first);
        CHECK(reasons == before);
        std::erase(reasons, first);
    }
    CHECK_FALSE(candidate_denial({}).has_value());
}

TEST_CASE("marriage check: stable reasons have keys and legacy command errors keep a zero code") {
    constexpr std::array<Denial, 23> reasons{Denial::NotAlive,          Denial::SamePerson,
                                             Denial::SameSex,           Denial::SameHouse,
                                             Denial::GroomTooYoung,     Denial::BrideTooYoung,
                                             Denial::TooYoungToBetroth, Denial::AlreadyMarried,
                                             Denial::Betrothed,         Denial::InNegotiation,
                                             Denial::Tonsured,          Denial::Disowned,
                                             Denial::MarriageLimit,     Denial::PreviouslyMarried,
                                             Denial::Mourning,          Denial::Pregnant,
                                             Denial::BloodKinship,      Denial::BloodKinshipDispensable,
                                             Denial::Affinity,          Denial::SpiritualKinship,
                                             Denial::FaithMismatch,     Denial::ChurchConsentRequired,
                                             Denial::NotYourMember};
    for (const auto reason : reasons) {
        CAPTURE(static_cast<std::uint16_t>(reason));
        CHECK_FALSE(marriage_denial_key(reason).empty());
    }
    CHECK(marriage_denial_key(static_cast<Denial>(0)).empty());
    CHECK(marriage_denial_key(static_cast<Denial>(29)).empty());
    CHECK(marriage_denial_key(static_cast<Denial>(65'535)).empty());
    const CommandError empty;
    CHECK(empty.reason == Rejection::None);
    CHECK(empty.code == 0);
    CHECK(std::string_view(empty.detail).empty());
    const CommandError reason_only{Rejection::UnknownEntity};
    CHECK(reason_only.reason == Rejection::UnknownEntity);
    CHECK(reason_only.code == 0);
    CHECK(std::string_view(reason_only.detail).empty());
    const CommandError legacy{Rejection::InvalidState, "detail"};
    CHECK(legacy.code == 0);
    CHECK(legacy.reason == Rejection::InvalidState);
    CHECK(std::string_view(legacy.detail) == "detail");
    const CommandError explicit_code{
        Rejection::InvalidState, "mourning", static_cast<std::uint16_t>(Denial::Mourning)};
    CHECK(explicit_code.code == 27);
}
