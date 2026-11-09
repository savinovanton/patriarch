// Names, patronymics and portraits (E7-T09): the pure functions of pat/sim/names.hpp on pools built in
// code. The shipped data and the loader are tested in names_content_test.cpp.

#include "pat/core/rng.hpp"
#include "pat/sim/names.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

using namespace pat;
using pat::sim::kNoName;
using pat::sim::LifeStage;
using pat::sim::NamePools;
using pat::sim::NamesErrorCode;
using pat::sim::NamingContext;
using pat::sim::Sex;

namespace {

sim::GivenName man(std::string id, std::int32_t weight) {
    sim::GivenName n;
    n.id = id;
    n.sex = Sex::Male;
    n.weight = weight;
    n.name_key = "name." + id;
    n.patronymic_son_key = "pat." + id + ".son";
    n.patronymic_daughter_key = "pat." + id + ".daughter";
    return n;
}

sim::GivenName woman(std::string id, std::int32_t weight) {
    sim::GivenName n;
    n.id = id;
    n.sex = Sex::Female;
    n.weight = weight;
    n.name_key = "name." + id;
    return n;
}

constexpr std::array<LifeStage, 5> kStages{
    LifeStage::Child, LifeStage::Adolescent, LifeStage::Prime, LifeStage::Mature, LifeStage::Elder};

/// Three men, three women, one house and one portrait per sex and stage (ids 11...15 and 21...25).
NamePools make_pools() {
    NamePools pools;
    pools.rules = {.parent_bonus = 40, .grandparent_bonus = 25};
    pools.given_names = {man("ivan", 30),
                         man("petr", 20),
                         man("fedor", 10),
                         woman("anna", 30),
                         woman("maria", 20),
                         woman("fekla", 10)};
    pools.surnames = {{"menshikov", "sur.menshikov.male", "sur.menshikov.female"}};
    for (std::size_t i = 0; i < kStages.size(); ++i) {
        pools.portraits.push_back({static_cast<std::uint16_t>(11 + i), Sex::Male, kStages[i]});
        pools.portraits.push_back({static_cast<std::uint16_t>(21 + i), Sex::Female, kStages[i]});
    }
    return pools;
}

std::string id_of(const NamePools& pools, std::size_t index) {
    REQUIRE(index < pools.given_names.size());
    return pools.given_names[index].id;
}

/// Frequencies of the chosen name ids over `draws` independent draws from one stream.
std::map<std::string, int>
frequencies(const NamePools& pools, const NamingContext& context, int draws, std::uint64_t seed = 1) {
    core::RngStreams streams(seed);
    core::Pcg32& rng = streams.stream("demography.names");
    std::map<std::string, int> counts;
    for (int i = 0; i < draws; ++i) {
        ++counts[id_of(pools, sim::choose_given_name(pools, rng, context))];
    }
    return counts;
}

}  // namespace

// ---- validation ----------------------------------------------------------------------------------

TEST_CASE("names: a complete pool validates") {
    CHECK(sim::validate(make_pools()).has_value());
}

TEST_CASE("names: validation names the problem") {
    const auto code_of = [](const NamePools& pools) {
        const auto result = sim::validate(pools);
        REQUIRE_FALSE(result.has_value());
        return result.error().code;
    };

    {
        NamePools p = make_pools();
        p.rules.parent_bonus = -1;
        CHECK(code_of(p) == NamesErrorCode::Rules);
        p = make_pools();
        p.rules.grandparent_bonus = NamePools::kMaxBonus + 1;
        CHECK(code_of(p) == NamesErrorCode::Rules);
    }
    {
        NamePools p = make_pools();
        p.given_names[0].weight = 0;
        CHECK(code_of(p) == NamesErrorCode::GivenName);
        p = make_pools();
        p.given_names[0].weight = NamePools::kMaxNameWeight + 1;
        CHECK(code_of(p) == NamesErrorCode::GivenName);
        p = make_pools();
        p.given_names[1].id = p.given_names[0].id;  // repeated id
        CHECK(code_of(p) == NamesErrorCode::GivenName);
        p = make_pools();
        p.given_names[0].name_key.clear();
        CHECK(code_of(p) == NamesErrorCode::GivenName);
    }
    {
        NamePools p = make_pools();
        p.given_names[0].patronymic_daughter_key.clear();  // a man needs both forms
        CHECK(code_of(p) == NamesErrorCode::Patronymic);
        p = make_pools();
        p.given_names[3].patronymic_son_key = "pat.anna.son";  // a woman has none
        CHECK(code_of(p) == NamesErrorCode::Patronymic);
    }
    {
        NamePools p = make_pools();
        p.surnames.push_back(p.surnames[0]);  // the house twice
        CHECK(code_of(p) == NamesErrorCode::Surname);
        p = make_pools();
        p.surnames[0].female_key.clear();
        CHECK(code_of(p) == NamesErrorCode::Surname);
    }
    {
        NamePools p = make_pools();
        p.portraits[0].id = 0;
        CHECK(code_of(p) == NamesErrorCode::Portrait);
        p = make_pools();
        p.portraits[1].id = p.portraits[0].id;
        CHECK(code_of(p) == NamesErrorCode::Portrait);
    }
    {
        NamePools p = make_pools();
        p.given_names.resize(3);  // no woman left
        CHECK(code_of(p) == NamesErrorCode::PoolCoverage);
        p = make_pools();
        p.portraits.erase(p.portraits.begin());  // no boy of the first stage
        CHECK(code_of(p) == NamesErrorCode::PoolCoverage);
    }
}

// ---- choosing a given name -----------------------------------------------------------------------

TEST_CASE("names: the same seed gives the same names, a different seed gives different ones") {
    const NamePools pools = make_pools();
    const NamingContext context{.sex = Sex::Male};

    const auto run = [&](std::uint64_t seed) {
        core::RngStreams streams(seed);
        core::Pcg32& rng = streams.stream("demography.names");
        std::vector<std::size_t> out;
        for (int i = 0; i < 64; ++i) {
            out.push_back(sim::choose_given_name(pools, rng, context));
        }
        return out;
    };
    CHECK(run(7) == run(7));
    CHECK(run(7) != run(8));
}

TEST_CASE("names: a name always matches the sex") {
    const NamePools pools = make_pools();
    core::RngStreams streams(3);
    core::Pcg32& rng = streams.stream("demography.names");
    for (int i = 0; i < 500; ++i) {
        const auto boy = sim::choose_given_name(pools, rng, NamingContext{.sex = Sex::Male});
        const auto girl = sim::choose_given_name(pools, rng, NamingContext{.sex = Sex::Female});
        REQUIRE(boy != kNoName);
        REQUIRE(girl != kNoName);
        CHECK(pools.given_names[boy].sex == Sex::Male);
        CHECK(pools.given_names[girl].sex == Sex::Female);
    }
}

TEST_CASE("names: frequencies follow the weights") {
    NamePools pools = make_pools();
    pools.rules = {};  // no family effect
    constexpr int kDraws = 60000;
    const auto counts = frequencies(pools, NamingContext{.sex = Sex::Male}, kDraws);
    // weights 30 : 20 : 10 -> 50 % : 33.3 % : 16.7 %
    CHECK(counts.at("ivan") == doctest::Approx(kDraws * 0.5).epsilon(0.03));
    CHECK(counts.at("petr") == doctest::Approx(kDraws / 3.0).epsilon(0.03));
    CHECK(counts.at("fedor") == doctest::Approx(kDraws / 6.0).epsilon(0.05));
}

TEST_CASE("names: living brothers are never repeated") {
    const NamePools pools = make_pools();
    const std::array<std::string_view, 2> brothers{"ivan", "petr"};
    NamingContext context{.sex = Sex::Male};
    context.living_siblings = brothers;
    // The father is called Ivan: the bonus must not bring his name back while a brother has it.
    context.parent_name = "ivan";
    const auto counts = frequencies(pools, context, 2000);
    REQUIRE(counts.size() == 1);
    CHECK(counts.contains("fedor"));
}

TEST_CASE("names: when every name is taken the exclusion is dropped") {
    const NamePools pools = make_pools();
    const std::array<std::string_view, 3> brothers{"ivan", "petr", "fedor"};
    NamingContext context{.sex = Sex::Male};
    context.living_siblings = brothers;
    const auto counts = frequencies(pools, context, 600);
    CHECK(counts.size() == 3);  // still a man's name, repeats allowed
    // Sisters do not take names away from a boy: only the listed ids count, and they are women's.
    const std::array<std::string_view, 1> sisters{"anna"};
    NamingContext boy{.sex = Sex::Male};
    boy.living_siblings = sisters;
    CHECK(frequencies(pools, boy, 600).size() == 3);
}

TEST_CASE("names: the father's and the grandfather's names are favoured") {
    NamePools pools = make_pools();
    constexpr int kDraws = 40000;

    NamingContext plain{.sex = Sex::Male};
    const auto without = frequencies(pools, plain, kDraws);

    NamingContext with_father = plain;
    with_father.parent_name = "fedor";  // the rarest name
    const auto father = frequencies(pools, with_father, kDraws);
    // fedor: weight 10 + 40 = 50 of 30 + 20 + 50 = 100; the others lose share
    CHECK(father.at("fedor") > without.at("fedor") * 2);
    CHECK(father.at("fedor") == doctest::Approx(kDraws * 50.0 / 100.0).epsilon(0.04));

    NamingContext with_both = plain;
    with_both.parent_name = "fedor";
    with_both.grandparent_name = "petr";
    const auto both = frequencies(pools, with_both, kDraws);
    // totals: ivan 30, petr 20 + 25, fedor 10 + 40 -> 30 : 45 : 50 of 125
    CHECK(both.at("petr") == doctest::Approx(kDraws * 45.0 / 125.0).epsilon(0.04));
    CHECK(both.at("fedor") == doctest::Approx(kDraws * 50.0 / 125.0).epsilon(0.04));

    // A parent name of the other sex or outside the pool changes nothing.
    NamingContext stray = plain;
    stray.parent_name = "anna";
    stray.grandparent_name = "no_such_name";
    CHECK(frequencies(pools, stray, kDraws) == without);

    // Bonuses of 0 turn the effect off.
    pools.rules = {};
    CHECK(frequencies(pools, with_both, kDraws) == without);
}

TEST_CASE("names: a girl takes the names of her mother's line the same way") {
    const NamePools pools = make_pools();
    NamingContext context{.sex = Sex::Female};
    context.parent_name = "fekla";
    context.grandparent_name = "maria";
    constexpr int kDraws = 40000;
    const auto counts = frequencies(pools, context, kDraws);
    // anna 30, maria 20 + 25, fekla 10 + 40 -> 30 : 45 : 50 of 125
    CHECK(counts.at("fekla") == doctest::Approx(kDraws * 50.0 / 125.0).epsilon(0.04));
}

TEST_CASE("names: exactly one draw is used in every branch") {
    const NamePools pools = make_pools();
    NamePools empty;

    const auto next_after = [](auto&& call) {
        core::RngStreams streams(42);
        core::Pcg32& rng = streams.stream("demography.names");
        call(rng);
        return rng.next_u32();
    };
    const auto after_one_draw = next_after([](core::Pcg32& rng) { (void)rng.next_u32(); });

    const std::array<std::string_view, 3> all_taken{"ivan", "petr", "fedor"};
    NamingContext taken{.sex = Sex::Male};
    taken.living_siblings = all_taken;
    NamingContext favoured{.sex = Sex::Male};
    favoured.parent_name = "ivan";

    CHECK(next_after([&](core::Pcg32& rng) {
              (void)sim::choose_given_name(pools, rng, {.sex = Sex::Male});
          }) == after_one_draw);
    CHECK(next_after([&](core::Pcg32& rng) { (void)sim::choose_given_name(pools, rng, taken); }) ==
          after_one_draw);
    CHECK(next_after([&](core::Pcg32& rng) { (void)sim::choose_given_name(pools, rng, favoured); }) ==
          after_one_draw);
    CHECK(next_after([&](core::Pcg32& rng) {
              (void)sim::choose_given_name(empty, rng, {.sex = Sex::Male});
          }) == after_one_draw);

    for (const Sex sex : {Sex::Male, Sex::Female}) {
        for (const LifeStage stage : kStages) {
            CHECK(next_after([&](core::Pcg32& rng) { (void)sim::choose_portrait(pools, rng, sex, stage); }) ==
                  after_one_draw);
            CHECK(next_after([&](core::Pcg32& rng) { (void)sim::choose_portrait(empty, rng, sex, stage); }) ==
                  after_one_draw);
        }
    }
}

TEST_CASE("names: an empty pool gives no name and no portrait") {
    const NamePools empty;
    core::RngStreams streams(1);
    core::Pcg32& rng = streams.stream("demography.names");
    CHECK(sim::choose_given_name(empty, rng, {.sex = Sex::Male}) == kNoName);
    CHECK(sim::choose_portrait(empty, rng, Sex::Female, LifeStage::Prime) == 0);

    NamePools only_men = make_pools();
    only_men.given_names.resize(3);
    CHECK(sim::choose_given_name(only_men, rng, {.sex = Sex::Female}) == kNoName);
    CHECK(sim::choose_given_name(only_men, rng, {.sex = Sex::Male}) != kNoName);
}

// ---- choosing a portrait -------------------------------------------------------------------------

TEST_CASE("names: a portrait comes from the pool of the sex and the life stage") {
    NamePools pools = make_pools();
    // Two more adult men and one more adult woman.
    pools.portraits.push_back({31, Sex::Male, LifeStage::Prime});
    pools.portraits.push_back({32, Sex::Male, LifeStage::Prime});
    pools.portraits.push_back({41, Sex::Female, LifeStage::Prime});

    core::RngStreams streams(5);
    core::Pcg32& rng = streams.stream("demography.names");
    std::map<std::uint16_t, int> men;
    std::map<std::uint16_t, int> women;
    for (int i = 0; i < 6000; ++i) {
        ++men[sim::choose_portrait(pools, rng, Sex::Male, LifeStage::Prime)];
        ++women[sim::choose_portrait(pools, rng, Sex::Female, LifeStage::Prime)];
        CHECK(sim::choose_portrait(pools, rng, Sex::Male, LifeStage::Child) == 11);
        CHECK(sim::choose_portrait(pools, rng, Sex::Female, LifeStage::Elder) == 25);
    }
    // Uniform over the portraits of that sex and stage, and only those.
    REQUIRE(men.size() == 3);
    for (const std::uint16_t id : std::array<std::uint16_t, 3>{13, 31, 32}) {
        CHECK(men.at(id) == doctest::Approx(2000).epsilon(0.08));
    }
    REQUIRE(women.size() == 2);
    for (const std::uint16_t id : std::array<std::uint16_t, 2>{23, 41}) {
        CHECK(women.at(id) == doctest::Approx(3000).epsilon(0.08));
    }
}

TEST_CASE("names: a missing stage falls back to the nearest one of the same sex") {
    NamePools pools;
    pools.portraits = {{1, Sex::Male, LifeStage::Child},
                       {2, Sex::Male, LifeStage::Prime},
                       {3, Sex::Male, LifeStage::Elder},
                       {9, Sex::Female, LifeStage::Mature}};
    core::RngStreams streams(1);
    core::Pcg32& rng = streams.stream("demography.names");

    // Child and Prime are equally near to Adolescent: the younger wins.
    CHECK(sim::choose_portrait(pools, rng, Sex::Male, LifeStage::Adolescent) == 1);
    // Prime and Elder are equally near to Mature: the younger wins.
    CHECK(sim::choose_portrait(pools, rng, Sex::Male, LifeStage::Mature) == 2);
    CHECK(sim::choose_portrait(pools, rng, Sex::Male, LifeStage::Elder) == 3);
    CHECK(sim::choose_portrait(pools, rng, Sex::Female, LifeStage::Child) == 9);  // the only one of her sex
}

// ---- composing a name ----------------------------------------------------------------------------

TEST_CASE("names: the full name is built from the person, the father and the house") {
    const NamePools pools = make_pools();

    const auto son = sim::person_name_keys(pools, "ivan", "petr", "menshikov", Sex::Male);
    CHECK(son.given == "name.ivan");
    CHECK(son.patronymic == "pat.petr.son");
    CHECK(son.surname == "sur.menshikov.male");

    const auto daughter = sim::person_name_keys(pools, "anna", "petr", "menshikov", Sex::Female);
    CHECK(daughter.given == "name.anna");
    CHECK(daughter.patronymic == "pat.petr.daughter");
    CHECK(daughter.surname == "sur.menshikov.female");
}

TEST_CASE("names: unknown parts stay empty") {
    const NamePools pools = make_pools();
    const auto orphan = sim::person_name_keys(pools, "ivan", "", "menshikov", Sex::Male);
    CHECK(orphan.given == "name.ivan");
    CHECK(orphan.patronymic.empty());
    CHECK(orphan.surname == "sur.menshikov.male");

    // A woman's name is not a father's name.
    CHECK(sim::person_name_keys(pools, "ivan", "anna", "menshikov", Sex::Male).patronymic.empty());
    // Names and houses outside the pool.
    const auto stranger = sim::person_name_keys(pools, "no_such", "no_such", "no_such", Sex::Female);
    CHECK(stranger.given.empty());
    CHECK(stranger.patronymic.empty());
    CHECK(stranger.surname.empty());
    const auto nobody = sim::person_name_keys(pools, "", "", "", Sex::Male);
    CHECK(nobody.given.empty());
    CHECK(nobody.patronymic.empty());
    CHECK(nobody.surname.empty());
}

TEST_CASE("names: lookups find entries by id") {
    const NamePools pools = make_pools();
    REQUIRE(sim::find_given_name(pools, "petr") != nullptr);
    CHECK(sim::find_given_name(pools, "petr")->weight == 20);
    CHECK(sim::find_given_name(pools, "nobody") == nullptr);
    CHECK(sim::find_given_name(pools, "") == nullptr);
    REQUIRE(sim::find_surname(pools, "menshikov") != nullptr);
    CHECK(sim::find_surname(pools, "nobody") == nullptr);
}
