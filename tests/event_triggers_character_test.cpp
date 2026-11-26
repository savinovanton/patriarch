// E11-T07: the triggers and values of the scopes character and dynasty. The catalog table (names, codes,
// signatures, tooltip keys, examples), the Russian tooltip templates, and the evaluators on a hand-made
// World, boundary by boundary; one compiled event runs through the machine with a host over the World.

#include "pat/core/rng.hpp"
#include "pat/data/content_source.hpp"
#include "pat/loc/localizer.hpp"
#include "pat/sim/economy/tables.hpp"
#include "pat/sim/events/catalog.hpp"
#include "pat/sim/events/character_triggers.hpp"
#include "pat/sim/events/vm.hpp"
#include "pat/sim/family/tables.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "court_fixture.hpp"
#include "event_compile_fixture.hpp"
#include "world_fixture.hpp"

using namespace pat::sim;
using namespace pat::sim::events;
using pat::core::Fixed32;
using pat::core::Fixed64;
using pat::core::StringId;

namespace {

Fixed32 fx32(int v) {
    return Fixed32::from_int(v);
}

Word whole(int v) {
    return word_from_int(v);
}

Date day(int y, int m, int d) {
    return Date::from_ymd(y, m, d);
}

// The trait slots of the scenario: slot 5 raises the martial skill and the stress, slot 9 does nothing.
constexpr std::uint16_t kSlotWarlike = 5;
constexpr std::uint16_t kSlotPlain = 9;

TraitCatalog trait_catalog() {
    TraitCatalog catalog;
    TraitDef warlike;
    warlike.id = kSlotWarlike;
    warlike.key = "warlike";
    warlike.name_key = "trait.warlike.name";
    warlike.desc_key = "trait.warlike.desc";
    warlike.skill_modifiers[static_cast<std::size_t>(Skill::Martial)] = fx32(2);
    warlike.attribute_modifiers[static_cast<std::size_t>(CharacterAttribute::Stress)] = fx32(10);
    catalog.definitions.push_back(warlike);
    TraitDef plain;
    plain.id = kSlotPlain;
    plain.key = "plain";
    plain.name_key = "trait.plain.name";
    plain.desc_key = "trait.plain.desc";
    catalog.definitions.push_back(plain);
    return catalog;
}

/// Two houses; the head of house A with a wife from house B, a son, a brother, and a few special people.
struct Scenario {
    World world{day(1703, 1, 1), 5};
    DynastyId house_a;
    DynastyId house_b;
    CharacterId father;    // A, born 1640
    CharacterId head;      // A, born 1670-01-01, head of A
    CharacterId brother;   // A, born 1672, tonsured
    CharacterId wife;      // B, born 1675, married to the head, pregnant
    CharacterId son;       // A, born 1700, heir of A
    CharacterId stranger;  // B, born 1668
    CharacterId adult16;   // A, born 1687-01-01: exactly 16 today
    CharacterId minor15;   // A, born 1687-01-02: 15 today, disowned
    CharacterId bride;     // B, betrothed to adult16
    CharacterId widow;     // A, her husband died
    CharacterId late;      // B, died
    DynastyId house_c;     // a child head under the regency of his uncle
    CharacterId kid;       // C, born 1695, head of C
    CharacterId uncle;     // C, born 1670, regent of C
    std::vector<std::uint16_t> trait_slots{0, kSlotWarlike, kSlotPlain};  // registry index -> slot
    TraitCatalog traits = trait_catalog();

    CharacterId
    add(DynastyId house, Sex sex, Date birth, CharacterId father_id = {}, CharacterId mother_id = {}) {
        CharacterInit init;
        init.dynasty = house;
        init.birth_date = birth;
        init.sex = sex;
        init.father = father_id;
        init.mother = mother_id;
        const CharacterId id = world.create_character(init);
        REQUIRE(id.valid());
        return id;
    }

    Scenario() {
        REQUIRE(world.enable_dynasty_mode());
        house_a = world.create_dynasty(StringId(0));
        house_b = world.create_dynasty(StringId(1));
        house_c = world.create_dynasty(StringId(2));
        father = add(house_a, Sex::Male, day(1640, 1, 1));
        head = add(house_a, Sex::Male, day(1670, 1, 1), father);
        brother = add(house_a, Sex::Male, day(1672, 6, 1), father);
        wife = add(house_b, Sex::Female, day(1675, 1, 1));
        stranger = add(house_b, Sex::Male, day(1668, 1, 1));
        REQUIRE(world.marry(head, wife));
        son = add(house_a, Sex::Male, day(1700, 3, 3), head, wife);
        adult16 = add(house_a, Sex::Male, day(1687, 1, 1));
        minor15 = add(house_a, Sex::Male, day(1687, 1, 2));
        bride = add(house_b, Sex::Female, day(1690, 1, 1));
        widow = add(house_a, Sex::Female, day(1665, 1, 1));
        late = add(house_b, Sex::Male, day(1660, 1, 1));
        REQUIRE(world.marry(widow, late));
        REQUIRE(world.die(late, CauseOfDeath::OldAge));
        kid = add(house_c, Sex::Male, day(1695, 1, 1));
        uncle = add(house_c, Sex::Male, day(1670, 5, 5));
        REQUIRE(world.set_dynasty_head(house_c, kid));

        REQUIRE(world.set_dynasty_head(house_a, head));
        world.set_dynasty_succession_law(house_a, SuccessionLaw::Majorat);
        world.set_dynasty_succession_law(house_b, SuccessionLaw::Minorat);
        world.add_honor(house_a, 40);
        world.set_dynasty_unity(house_a, fx32(35));
        world.add_prestige(house_a, fx32(20), kStandingReasonUnspecified);

        world.set_character_skill(head, Skill::Martial, fx32(10));
        world.set_character_skill(head, Skill::Stewardship, fx32(4));
        world.set_character_health(head, fx32(45));
        world.set_character_stress(head, fx32(62));
        world.set_character_piety(head, fx32(55));
        world.set_character_fertility(head, fx32(40));
        world.set_character_ambition(head, fx32(35));
        world.set_character_loyalty_head(head, fx32(-5));
        const std::array<std::uint16_t, 2> head_traits{kSlotWarlike, kSlotPlain};
        REQUIRE(world.set_character_traits(head, head_traits));
        Education education;
        education.focus = static_cast<std::uint8_t>(Skill::Martial) + 1;
        REQUIRE(world.set_character_education(head, education));
        REQUIRE(world.add_opinion(head, stranger, fx32(25), 1));

        Pregnancy pregnancy;
        pregnancy.mother = wife;
        pregnancy.father = head;
        pregnancy.conceived = day(1702, 9, 1);
        pregnancy.due = day(1703, 6, 1);
        REQUIRE(world.begin_pregnancy(pregnancy));

        family::PersonStatus tonsure;
        tonsure.character = brother;
        tonsure.flags = family::kPersonTonsured;
        tonsure.tonsured_on = day(1702, 1, 1);
        REQUIRE(world.set_family_status(tonsure));
        family::PersonStatus disowned;
        disowned.character = minor15;
        disowned.flags = family::kPersonDisowned;
        disowned.disowned_on = day(1702, 5, 5);
        REQUIRE(world.set_family_status(disowned));
        family::SuccessionState succession;
        succession.dynasty = house_a;
        succession.designated_heir = son;
        REQUIRE(world.set_family_succession(succession));
        family::Regency regency;
        regency.dynasty = house_c;
        regency.ward = kid;
        regency.regent = uncle;
        regency.kind = family::RegencyKind::Uncle;
        regency.started = day(1702, 12, 1);
        REQUIRE(world.set_family_regency(regency));

        family::Betrothal betrothal;
        betrothal.groom = adult16;
        betrothal.bride = bride;
        betrothal.groom_house = house_a;
        betrothal.bride_house = house_b;
        betrothal.contracted = day(1702, 11, 1);
        betrothal.wedding_not_before = day(1704, 1, 1);
        betrothal.wedding_deadline = day(1704, 4, 1);
        betrothal.dowry_source = house_b;
        betrothal.origin = family::BetrothalOrigin::Command;
        REQUIRE(world.create_family_betrothal(betrothal).has_value());
        pat::test::require_valid(world);
    }

    [[nodiscard]] TriggerEnv env(bool with_traits = false) const {
        TriggerEnv e;
        e.world = &world;
        e.traits = with_traits ? &traits : nullptr;
        e.trait_slots = trait_slots;
        return e;
    }
};

Scope ch(CharacterId id) {
    return Scope::make(ScopeKind::Character, id.value());
}
Scope dy(DynastyId id) {
    return Scope::make(ScopeKind::Dynasty, id.value());
}
Word scope_word(CharacterId id) {
    return ch(id).word();
}

/// One predicate; a code outside the block would fail the test.
bool pred(const TriggerEnv& env, Scope scope, std::uint16_t code, std::vector<Word> args = {}) {
    const auto r = eval_character_predicate(env, scope, code, args);
    REQUIRE(r.has_value());
    return r->value == 1;
}

Word val(const TriggerEnv& env, Scope scope, std::uint16_t code, std::vector<Word> args = {}) {
    const auto r = eval_character_value(env, scope, code, args);
    REQUIRE(r.has_value());
    return r->value;
}

Word fixed_word(Fixed32 v) {
    return word_from_fixed(Fixed64::from_raw(static_cast<std::int64_t>(v.raw()) * 65536));
}

Catalog t07_catalog() {
    Catalog c = Catalog::engine();
    register_character_triggers(c);
    return c;
}

}  // namespace

// ---- the table ------------------------------------------------------------------------------------------

TEST_CASE("character triggers: the table of section 8.2 is registered with stable codes") {
    const Catalog c = t07_catalog();
    const std::map<std::uint16_t, std::string_view> predicates{
        {1001, "is_adult"},
        {1002, "is_female"},
        {1003, "is_married"},
        {1004, "is_pregnant"},
        {1005, "is_head_of_dynasty"},
        {1006, "is_heir"},
        {1007, "is_tonsured"},
        {1008, "is_widowed"},
        {1009, "has_trait"},
        {1010, "has_education"},
        {1011, "same_dynasty"},
        {1012, "is_alive"},
        {1013, "has_living_child"},
        {1014, "is_regent"},
        {1015, "is_betrothed"},
        {1016, "is_disowned"},
        {1035, "has_succession_law"},
        {1200, "has_office"},
    };
    const std::map<std::uint16_t, std::string_view> values{
        {1021, "age"},
        {1022, "skill"},
        {1023, "health"},
        {1024, "stress"},
        {1025, "piety"},
        {1026, "opinion_of"},
        {1027, "kinship_degree"},
        {1028, "fertility"},
        {1029, "ambition"},
        {1030, "loyalty_head"},
        {1031, "honor"},
        {1032, "unity"},
        {1033, "prestige"},
        {1034, "treasury"},
        {1036, "child_count"},
    };
    for (const auto& [code, name] : predicates) {
        CAPTURE(name);
        const CatalogEntry* e = c.find(NameSpace::Predicate, name);
        REQUIRE(e != nullptr);
        CHECK(e->code == code);
        CHECK(e->result == ValueType::Bool);
        CHECK_FALSE(e->example.empty());
        CHECK(c.find_code(NameSpace::Predicate, code) == e);
    }
    for (const auto& [code, name] : values) {
        CAPTURE(name);
        const CatalogEntry* e = c.find(NameSpace::Value, name);
        REQUIRE(e != nullptr);
        CHECK(e->code == code);
        CHECK(e->result == ValueType::Number);
        CHECK_FALSE(e->example.empty());
        CHECK(c.find_code(NameSpace::Value, code) == e);
    }
    std::size_t registered = 0;
    for (const CatalogEntry& e : c.entries()) {
        registered += (e.space == NameSpace::Predicate || e.space == NameSpace::Value) ? 1 : 0;
    }
    CHECK(registered == predicates.size() + values.size());

    // Only has_office is a stub (E10); the codes 1210..1214 (portfolios), 1250..1319 stay untouched.
    for (const CatalogEntry& e : c.entries()) {
        CHECK((e.stub.empty() == (e.name != "has_office")));
        CHECK((e.code < 1210 || e.code > 1319));
    }
    CHECK(Catalog::standard().find(NameSpace::Predicate, "is_adult") != nullptr);
}

TEST_CASE("character triggers: scopes and argument signatures") {
    const Catalog c = t07_catalog();
    const ScopeMask character = scope_bit(ScopeKind::Character);
    const ScopeMask dynasty = scope_bit(ScopeKind::Dynasty);
    CHECK(c.find(NameSpace::Predicate, "is_adult")->from == character);
    CHECK(c.find(NameSpace::Predicate, "same_dynasty")->from == (character | dynasty));
    CHECK(c.find(NameSpace::Predicate, "has_succession_law")->from == dynasty);
    CHECK(c.find(NameSpace::Value, "honor")->from == dynasty);
    CHECK(c.find(NameSpace::Value, "skill")->suffixes.size() == kSkillCount);
    const CatalogEntry* trait = c.find(NameSpace::Predicate, "has_trait");
    REQUIRE(trait->args.size() == 1);
    CHECK(trait->args[0].kind == ArgKind::Ref);
    CHECK(trait->args[0].ref_kind == "trait");
    const CatalogEntry* opinion = c.find(NameSpace::Value, "opinion_of");
    REQUIRE(opinion->args.size() == 1);
    CHECK(opinion->args[0].kind == ArgKind::Scope);
    CHECK(opinion->args[0].scope_kind == ScopeKind::Character);
    CHECK(c.find(NameSpace::Predicate, "has_education")->args[0].enum_values.size() == kSkillCount);
    CHECK(c.find(NameSpace::Predicate, "has_succession_law")->args[0].enum_values.size() ==
          kSuccessionLawCount);
    CHECK(tooltip_key(*c.find(NameSpace::Predicate, "is_adult")) == "tooltip.trigger.is_adult");
    CHECK(tooltip_not_key(*c.find(NameSpace::Predicate, "is_adult")) == "tooltip.trigger.is_adult.not");
    CHECK(tooltip_fail_key(*c.find(NameSpace::Value, "age")) == "tooltip.trigger.age.fail");
}

// Every trigger and value of the standard catalog (E11-T07, T08 and the blocks after them): the files of
// the blocks are listed here.
TEST_CASE("trigger catalog: every entry has Russian tooltip templates with the right parameters") {
    pat::data::MemorySource source("tooltips");
    for (const char* file : {"loc/ru/events/tooltip_character.pd", "loc/ru/events/tooltip_realm.pd"}) {
        source.add(file, pat::court_fixture::shipped_text(file));
    }
    const auto loaded = pat::loc::load_localization(source);
    std::string report;
    for (const auto& error : loaded.errors) {
        report += error.format() + "\n";
    }
    INFO(report);
    REQUIRE(loaded.ok());
    const auto& strings = loaded.localizer;

    for (const char* op : {"ge", "gt", "le", "lt", "eq", "ne"}) {
        const std::string key = std::string(kTooltipComparePrefix) + op;
        CAPTURE(key);
        REQUIRE(strings.contains("ru", key));
        CHECK(strings.parameters("ru", key) == std::vector<std::string>{"value"});
    }
    REQUIRE(strings.contains("ru", std::string(kTooltipRequiredKey)));
    CHECK(strings.parameters("ru", std::string(kTooltipRequiredKey)) == std::vector<std::string>{"text"});

    const auto check_params = [&](const CatalogEntry& e, const std::string& key, bool value_text) {
        CAPTURE(key);
        REQUIRE(strings.contains("ru", key));
        std::set<std::string> allowed;
        for (const ArgSpec& a : e.args) {
            allowed.insert(a.name);
        }
        if (!e.suffixes.empty()) {
            allowed.insert("kind");
        }
        if (value_text) {
            allowed.insert("need");
            allowed.insert("have");
        }
        for (const std::string& p : strings.parameters("ru", key)) {
            CHECK(allowed.contains(p));
        }
    };

    const Catalog c = Catalog::standard();
    for (const CatalogEntry& e : c.entries()) {
        if (e.space != NameSpace::Predicate && e.space != NameSpace::Value) {
            continue;
        }
        CAPTURE(e.name);
        const bool is_value = e.space == NameSpace::Value;
        const std::string key = tooltip_key(e);
        const std::string fail = tooltip_fail_key(e);
        check_params(e, key, is_value);
        check_params(e, fail, is_value);
        const auto has_param = [&](const std::string& k, const std::string& p) {
            const auto params = strings.parameters("ru", k);
            return std::ranges::find(params, p) != params.end();
        };
        if (is_value) {
            CHECK(has_param(key, "need"));
            CHECK(has_param(fail, "need"));
            CHECK(has_param(fail, "have"));
        } else {
            check_params(e, tooltip_not_key(e), false);
            // Every non-optional argument is named by the condition and by its negation.
            for (const ArgSpec& a : e.args) {
                if (a.required) {
                    CHECK(has_param(key, a.name));
                    CHECK(has_param(tooltip_not_key(e), a.name));
                }
            }
        }
    }
}

// ---- predicates -----------------------------------------------------------------------------------------

TEST_CASE("character triggers: is_adult at the 16th birthday") {
    Scenario s;
    const TriggerEnv env = s.env();
    CHECK(pred(env, ch(s.adult16), kTrigIsAdult));        // 16 years and 0 days
    CHECK_FALSE(pred(env, ch(s.minor15), kTrigIsAdult));  // one day short
    CHECK(pred(env, ch(s.head), kTrigIsAdult));
    CHECK_FALSE(pred(env, ch(s.son), kTrigIsAdult));
    CHECK_FALSE(pred(env, ch(s.late), kTrigIsAdult));  // the dead are not adults for the rules
    CHECK(val(env, ch(s.adult16), kValAge) == whole(16));
    CHECK(val(env, ch(s.minor15), kValAge) == whole(15));
}

TEST_CASE("character triggers: sex, marriage, pregnancy, widowhood, betrothal") {
    Scenario s;
    const TriggerEnv env = s.env();
    CHECK(pred(env, ch(s.wife), kTrigIsFemale));
    CHECK_FALSE(pred(env, ch(s.head), kTrigIsFemale));
    CHECK_FALSE(pred(env, ch(s.late), kTrigIsFemale));

    CHECK(pred(env, ch(s.head), kTrigIsMarried));
    CHECK(pred(env, ch(s.wife), kTrigIsMarried));
    CHECK_FALSE(pred(env, ch(s.stranger), kTrigIsMarried));
    CHECK_FALSE(pred(env, ch(s.widow), kTrigIsMarried));

    CHECK(pred(env, ch(s.wife), kTrigIsPregnant));
    CHECK_FALSE(pred(env, ch(s.head), kTrigIsPregnant));

    CHECK(pred(env, ch(s.widow), kTrigIsWidowed));
    CHECK_FALSE(pred(env, ch(s.wife), kTrigIsWidowed));      // married
    CHECK_FALSE(pred(env, ch(s.stranger), kTrigIsWidowed));  // never married

    CHECK(pred(env, ch(s.adult16), kTrigIsBetrothed));
    CHECK(pred(env, ch(s.bride), kTrigIsBetrothed));
    CHECK_FALSE(pred(env, ch(s.head), kTrigIsBetrothed));
}

TEST_CASE("character triggers: head, heir, regent, tonsure, disowned, alive") {
    Scenario s;
    const TriggerEnv env = s.env();
    CHECK(pred(env, ch(s.head), kTrigIsHeadOfDynasty));
    CHECK_FALSE(pred(env, ch(s.son), kTrigIsHeadOfDynasty));
    CHECK_FALSE(pred(env, ch(s.stranger), kTrigIsHeadOfDynasty));  // house B has no head

    CHECK(pred(env, ch(s.son), kTrigIsHeir));
    CHECK_FALSE(pred(env, ch(s.head), kTrigIsHeir));
    CHECK_FALSE(pred(env, ch(s.stranger), kTrigIsHeir));  // no succession row at all

    CHECK(pred(env, ch(s.uncle), kTrigIsRegent));
    CHECK_FALSE(pred(env, ch(s.kid), kTrigIsRegent));  // the ward
    CHECK_FALSE(pred(env, ch(s.brother), kTrigIsRegent));
    CHECK_FALSE(pred(env, ch(s.head), kTrigIsRegent));
    CHECK_FALSE(pred(env, ch(s.stranger), kTrigIsRegent));

    CHECK(pred(env, ch(s.brother), kTrigIsTonsured));
    CHECK_FALSE(pred(env, ch(s.head), kTrigIsTonsured));
    CHECK(pred(env, ch(s.minor15), kTrigIsDisowned));
    CHECK_FALSE(pred(env, ch(s.brother), kTrigIsDisowned));

    CHECK(pred(env, ch(s.head), kTrigIsAlive));
    CHECK_FALSE(pred(env, ch(s.late), kTrigIsAlive));
    CHECK_FALSE(pred(env, Scope::make(ScopeKind::Character, 9999), kTrigIsAlive));  // unknown person
    CHECK_FALSE(pred(env, dy(s.house_a), kTrigIsAlive));                            // wrong kind of scope
}

TEST_CASE("character triggers: has_trait, has_education, same_dynasty, has_living_child") {
    Scenario s;
    const TriggerEnv env = s.env();
    CHECK(pred(env, ch(s.head), kTrigHasTrait, {whole(1)}));        // registry index 1 -> slot 5
    CHECK(pred(env, ch(s.head), kTrigHasTrait, {whole(2)}));        // registry index 2 -> slot 9
    CHECK_FALSE(pred(env, ch(s.head), kTrigHasTrait, {whole(0)}));  // index 0 -> slot 0: nothing
    CHECK_FALSE(pred(env, ch(s.head), kTrigHasTrait, {whole(7)}));  // outside the table
    CHECK_FALSE(pred(env, ch(s.son), kTrigHasTrait, {whole(1)}));
    TriggerEnv identity = s.env();
    identity.trait_slots = {};  // the argument is the slot itself
    CHECK(pred(identity, ch(s.head), kTrigHasTrait, {whole(kSlotWarlike)}));
    CHECK_FALSE(pred(identity, ch(s.head), kTrigHasTrait, {whole(6)}));

    CHECK(pred(env, ch(s.head), kTrigHasEducation, {whole(static_cast<int>(Skill::Martial))}));
    CHECK_FALSE(pred(env, ch(s.head), kTrigHasEducation, {whole(static_cast<int>(Skill::Learning))}));
    CHECK_FALSE(pred(env, ch(s.son), kTrigHasEducation, {whole(static_cast<int>(Skill::Martial))}));

    CHECK(pred(env, ch(s.head), kTrigSameDynasty, {scope_word(s.brother)}));
    CHECK_FALSE(pred(env, ch(s.head), kTrigSameDynasty, {scope_word(s.wife)}));
    CHECK(pred(env, ch(s.head), kTrigSameDynasty, {dy(s.house_a).word()}));
    CHECK(pred(env, dy(s.house_a), kTrigSameDynasty, {scope_word(s.son)}));
    CHECK(pred(env, ch(s.late), kTrigSameDynasty, {scope_word(s.stranger)}));  // the dead keep their house
    CHECK_FALSE(pred(env, ch(s.head), kTrigSameDynasty, {Scope{}.word()}));

    CHECK(pred(env, ch(s.head), kTrigHasLivingChild));
    CHECK(pred(env, ch(s.wife), kTrigHasLivingChild));
    CHECK_FALSE(pred(env, ch(s.son), kTrigHasLivingChild));
    CHECK_FALSE(pred(env, ch(s.stranger), kTrigHasLivingChild));
}

TEST_CASE("character triggers: has_succession_law and the office stub") {
    Scenario s;
    const TriggerEnv env = s.env();
    CHECK(pred(env, dy(s.house_a), kTrigHasSuccessionLaw, {whole(static_cast<int>(SuccessionLaw::Majorat))}));
    CHECK_FALSE(
        pred(env, dy(s.house_a), kTrigHasSuccessionLaw, {whole(static_cast<int>(SuccessionLaw::Minorat))}));
    CHECK(pred(env, dy(s.house_b), kTrigHasSuccessionLaw, {whole(static_cast<int>(SuccessionLaw::Minorat))}));
    CHECK_FALSE(pred(env, ch(s.head), kTrigHasSuccessionLaw, {whole(0)}));  // not a dynasty scope

    // has_office: no offices in the World yet: false, and the evaluator says there is no data.
    const auto office = eval_character_predicate(env, ch(s.head), kTrigHasOffice, {});
    REQUIRE(office.has_value());
    CHECK(office->value == 0);
    CHECK_FALSE(office->has_data);
    const auto adult = eval_character_predicate(env, ch(s.head), kTrigIsAdult, {});
    REQUIRE(adult.has_value());
    CHECK(adult->has_data);
}

// ---- values ---------------------------------------------------------------------------------------------

TEST_CASE("character triggers: age at the day before and on the birthday, and after death") {
    Scenario s;
    CHECK(val(s.env(), ch(s.head), kValAge) == whole(33));
    CHECK(val(s.env(), ch(s.son), kValAge) == whole(2));
    CHECK(val(s.env(), ch(s.late), kValAge) == whole(43));  // 1660-01-01 .. 1703-01-01 (died today)
    CHECK(val(s.env(), Scope::make(ScopeKind::Character, 9999), kValAge) == 0);
    CHECK(val(s.env(), dy(s.house_a), kValAge) == 0);
    // one day later the son is still 2, the minor turns 16 on 1703-01-02
    (void)s.world.advance_day();
    CHECK(val(s.env(), ch(s.minor15), kValAge) == whole(16));
    CHECK(pred(s.env(), ch(s.minor15), kTrigIsAdult));
}

TEST_CASE("character triggers: skills, health, stress, piety, fertility, ambition, loyalty") {
    Scenario s;
    const TriggerEnv stored = s.env();
    const TriggerEnv effective = s.env(true);
    const Word martial = whole(static_cast<int>(Skill::Martial));
    const Word stewardship = whole(static_cast<int>(Skill::Stewardship));
    CHECK(val(stored, ch(s.head), kValSkill, {martial}) == whole(10));
    CHECK(val(stored, ch(s.head), kValSkill, {stewardship}) == whole(4));
    CHECK(val(effective, ch(s.head), kValSkill, {martial}) == whole(12));  // the warlike trait: +2
    CHECK(val(effective, ch(s.head), kValSkill, {stewardship}) == whole(4));
    CHECK(val(stored, ch(s.head), kValSkill, {whole(7)}) == 0);  // no such skill
    CHECK(val(stored, ch(s.late), kValSkill, {martial}) == 0);

    CHECK(val(stored, ch(s.head), kValHealth) == whole(45));
    CHECK(val(stored, ch(s.head), kValStress) == whole(62));
    CHECK(val(effective, ch(s.head), kValStress) == whole(72));  // +10 from the trait
    CHECK(val(stored, ch(s.head), kValPiety) == whole(55));
    CHECK(val(stored, ch(s.head), kValFertility) == whole(40));
    CHECK(val(stored, ch(s.head), kValAmbition) == whole(35));
    CHECK(val(stored, ch(s.head), kValLoyaltyHead) == whole(-5));
    CHECK(val(stored, ch(s.late), kValHealth) == 0);
    CHECK(val(stored, dy(s.house_a), kValHealth) == 0);
}

TEST_CASE("character triggers: opinion, kinship degree, number of children") {
    Scenario s;
    const TriggerEnv env = s.env();
    CHECK(val(env, ch(s.head), kValOpinionOf, {scope_word(s.stranger)}) == whole(25));
    CHECK(val(env, ch(s.stranger), kValOpinionOf, {scope_word(s.head)}) == 0);  // one-directional
    CHECK(val(env, ch(s.head), kValOpinionOf, {dy(s.house_b).word()}) == 0);    // not a person
    CHECK(val(env, ch(s.late), kValOpinionOf, {scope_word(s.head)}) == 0);      // the dead hold none

    CHECK(val(env, ch(s.head), kValKinshipDegree, {scope_word(s.brother)}) == whole(2));
    CHECK(val(env, ch(s.head), kValKinshipDegree, {scope_word(s.father)}) == whole(1));
    CHECK(val(env, ch(s.head), kValKinshipDegree, {scope_word(s.son)}) == whole(1));
    CHECK(val(env, ch(s.head), kValKinshipDegree, {scope_word(s.stranger)}) == whole(kNoKinship));
    CHECK(val(env, ch(s.head), kValKinshipDegree, {Scope{}.word()}) == whole(kNoKinship));

    CHECK(val(env, ch(s.head), kValChildCount) == whole(1));
    CHECK(val(env, ch(s.father), kValChildCount) == whole(2));
    CHECK(val(env, ch(s.stranger), kValChildCount) == 0);
}

TEST_CASE("character triggers: dynasty honor, unity, prestige and treasury") {
    Scenario s;
    const TriggerEnv env = s.env();
    CHECK(val(env, dy(s.house_a), kValHonor) == whole(s.world.dynasties().honor[s.house_a.index()]));
    CHECK(val(env, dy(s.house_a), kValUnity) == fixed_word(fx32(35)));
    CHECK(val(env, dy(s.house_a), kValPrestige) ==
          fixed_word(s.world.dynasties().prestige[s.house_a.index()]));
    CHECK(val(env, ch(s.head), kValHonor) == 0);  // a character is not a dynasty

    // without an economy the treasury has no data
    const auto none = eval_character_value(env, dy(s.house_a), kValTreasury, {});
    REQUIRE(none.has_value());
    CHECK(none->value == 0);
    CHECK_FALSE(none->has_data);

    REQUIRE(s.world.enable_economy());
    REQUIRE(s.world.set_initial_treasury(economy::AccountRef::of(s.house_a), 123'456));  // 1234.56 roubles
    const auto rich = eval_character_value(env, dy(s.house_a), kValTreasury, {});
    REQUIRE(rich.has_value());
    CHECK(rich->has_data);
    CHECK(rich->value == word_from_fixed(Fixed64::from_ratio(123'456, 100)));
    CHECK(eval_character_value(env, dy(s.house_b), kValTreasury, {})->value == 0);
}

TEST_CASE("character triggers: codes of other blocks are not answered") {
    Scenario s;
    const TriggerEnv env = s.env();
    CHECK_FALSE(eval_character_predicate(env, ch(s.head), 1101, {}).has_value());     // T08: has_flag
    CHECK_FALSE(eval_character_value(env, ch(s.head), 1109, {}).has_value());         // T08: current_year
    CHECK_FALSE(eval_character_predicate(env, ch(s.head), kValAge, {}).has_value());  // a value code
}

// ---- compiled event, machine, host over the World --------------------------------------------------------

namespace {

/// The host of the machine over a World: transitions `dynasty`, predicates and values of this block.
class WorldHost final : public VmHost {
public:
    explicit WorldHost(TriggerEnv env) : env_(env), rng_(1, 7) {}

    Scope transition(Scope from, std::uint16_t code) override {
        if (from.kind != ScopeKind::Character || !world().is_alive(CharacterId{from.id})) {
            return {};
        }
        const std::size_t i = CharacterId{from.id}.index();
        const CharacterTable& t = world().characters();
        if (code == 1) {  // `dynasty`
            return dy(t.dynasty[i]);
        }
        if (code == 2) {  // `father`
            return t.father[i].valid() ? ch(t.father[i]) : Scope{};
        }
        return {};
    }
    Word load(Scope scope, std::uint16_t code, std::span<const Word> args) override {
        const auto r = eval_character_value(env_, scope, code, args);
        return r ? r->value : 0;
    }
    Word load_var(Scope, bool, std::uint64_t) override { return 0; }
    bool predicate(Scope scope, std::uint16_t code, std::span<const Word> args) override {
        const auto r = eval_character_predicate(env_, scope, code, args);
        return r && r->value != 0;
    }
    bool list(Scope, std::uint16_t, std::uint32_t, std::vector<Scope>&) override { return false; }
    pat::core::Pcg32& rng() override { return rng_; }

private:
    [[nodiscard]] const World& world() const { return *env_.world; }
    TriggerEnv env_;
    pat::core::Pcg32 rng_;
};

}  // namespace

TEST_CASE("character triggers: a compiled event trigger runs against the World") {
    Scenario s;
    REQUIRE(s.world.add_opinion(s.head, s.father, fx32(25), 1));
    const Catalog catalog = Catalog::standard();
    const auto compiled = pat::event_fixture::compile_event_files(
        {{"events/t.pd", pat::event_fixture::character_event(R"(trigger {
        is_adult = yes
        is_married = yes
        skill.martial >= 8
        has_education = martial
        opinion_of = { target = father  value >= 20 }
        dynasty = { honor >= 30  has_succession_law = majorat  unity < 40 }
    })")}},
        {},
        catalog);
    INFO(compiled.all_errors());
    REQUIRE(compiled.ok());
    REQUIRE(compiled.set->events.size() == 1);
    const EventDef& event = compiled.set->events.front();
    REQUIRE(event.trigger.has_value());

    const auto holds = [&](CharacterId who) {
        WorldHost host(s.env());
        Vm vm;
        RunOptions options;
        options.kind = Kind::Trigger;
        options.root = ch(who);
        const RunResult result = vm.run(*event.trigger, host, options);
        CHECK(result.ok);
        return result.truthy();
    };
    CHECK(holds(s.head));
    CHECK_FALSE(holds(s.son));     // a child
    CHECK_FALSE(holds(s.wife));    // no martial skill, no education
    CHECK_FALSE(holds(s.father));  // not married
    // each condition matters: lower the martial skill below 8
    s.world.set_character_skill(s.head, Skill::Martial, fx32(7));
    CHECK_FALSE(holds(s.head));
    s.world.set_character_skill(s.head, Skill::Martial, fx32(8));
    CHECK(holds(s.head));
    s.world.set_dynasty_succession_law(s.house_a, SuccessionLaw::Partition);
    CHECK_FALSE(holds(s.head));
}
