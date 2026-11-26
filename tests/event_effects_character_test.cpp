// E11-T09: the effects of the scopes character and dynasty as commands. The catalog table (names, codes,
// signatures, stubs), the Russian tooltip templates (completeness and golden strings), the new commands
// AdjustSkill / AdjustHealth / AdjustStress (validate, apply, bytes), and for every effect one compiled
// event option run in Run and in Preview over a hand-made World: both give the same commands ("shown =
// applied"), the commands carry the system as actor and pass validate. A refused command does not stop the
// others, and a replay of a game whose system issues event commands reproduces the hashes.

#include "pat/core/bytes.hpp"
#include "pat/data/content_source.hpp"
#include "pat/loc/localizer.hpp"
#include "pat/sim/command.hpp"
#include "pat/sim/command_log.hpp"
#include "pat/sim/events/catalog.hpp"
#include "pat/sim/events/character_effects.hpp"
#include "pat/sim/events/effects.hpp"
#include "pat/sim/events/vm.hpp"
#include "pat/sim/events/world_host.hpp"
#include "pat/sim/replay.hpp"
#include "pat/sim/simulation.hpp"
#include "pat/sim/traits.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
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

Date day(int y, int m, int d) {
    return Date::from_ymd(y, m, d);
}

Scope ch(CharacterId id) {
    return Scope::make(ScopeKind::Character, id.value());
}

constexpr std::uint16_t kSlotCruel = 7;

TraitCatalog trait_catalog() {
    TraitCatalog catalog;
    TraitDef cruel;
    cruel.id = kSlotCruel;
    cruel.key = "cruel";
    cruel.name_key = "trait.cruel.name";
    cruel.desc_key = "trait.cruel.desc";
    catalog.definitions.push_back(cruel);
    return catalog;
}

/// Two houses: the head of A with a wife from B and their son, a brother, a young man to betroth; the head
/// of B, a lover and a bride in B.
struct Scenario {
    World world{day(1703, 1, 1), 9};
    DynastyId house_a;
    DynastyId house_b;
    CharacterId head;     // A, born 1670, head of A
    CharacterId wife;     // B, born 1680, married to the head
    CharacterId son;      // A, born 1693 (a minor of 9)
    CharacterId brother;  // A, born 1672
    CharacterId groom;    // A, born 1684
    CharacterId rival;    // B, born 1668, head of B
    CharacterId lover;    // B, born 1675
    CharacterId bride;    // B, born 1687
    TraitCatalog traits = trait_catalog();
    std::vector<std::uint16_t> trait_slots;  // registry index -> slot (filled when the trait index is known)

    CharacterId add(DynastyId house, Sex sex, Date birth, CharacterId father = {}, CharacterId mother = {}) {
        CharacterInit init;
        init.dynasty = house;
        init.birth_date = birth;
        init.sex = sex;
        init.father = father;
        init.mother = mother;
        init.health = fx32(60);
        init.stress = fx32(20);
        init.fertility = fx32(80);
        const CharacterId id = world.create_character(init);
        REQUIRE(id.valid());
        return id;
    }

    Scenario() {
        REQUIRE(world.enable_dynasty_mode());
        house_a = world.create_dynasty(StringId(0));
        house_b = world.create_dynasty(StringId(1));
        head = add(house_a, Sex::Male, day(1670, 1, 1));
        wife = add(house_b, Sex::Female, day(1680, 1, 1));
        REQUIRE(world.marry(head, wife));
        son = add(house_a, Sex::Male, day(1693, 6, 1), head, wife);
        brother = add(house_a, Sex::Male, day(1672, 1, 1));
        groom = add(house_a, Sex::Male, day(1684, 1, 1));
        rival = add(house_b, Sex::Male, day(1668, 1, 1));
        lover = add(house_b, Sex::Male, day(1675, 1, 1));
        bride = add(house_b, Sex::Female, day(1687, 1, 1));
        REQUIRE(world.set_dynasty_head(house_a, head));
        REQUIRE(world.set_dynasty_head(house_b, rival));
        world.set_character_skill(head, Skill::Martial, fx32(10));
        world.set_dynasty_unity(house_a, fx32(50));
        world.add_prestige(house_a, fx32(20), kStandingReasonUnspecified);
        world.add_honor(house_a, 40);
        pat::test::require_valid(world);
    }

    [[nodiscard]] EffectEnv env() const {
        EffectEnv e;
        e.triggers.world = &world;
        e.triggers.traits = &traits;
        e.triggers.trait_slots = trait_slots;
        return e;
    }

    [[nodiscard]] CommandContent content() const { return CommandContent{&traits}; }
};

// ---- one event with one option per effect ------------------------------------------------------------

/// What an option does: its effect block, the root and the scope saved as `scope:other`.
struct Case {
    std::string name;
    std::string effect;
    CharacterId Scenario::* root = &Scenario::head;
    CharacterId Scenario::* other = &Scenario::wife;
};

std::vector<Case> cases() {
    return {
        {"change_honor", "dynasty = { change_honor = -5 }"},
        {"change_unity", "dynasty = { change_unity = 2.5 }"},
        {"change_prestige", "dynasty = { change_prestige = 10 }"},
        {"change_opinion", "change_opinion = { target = spouse  value = -20  reason = 3 }"},
        {"add_trait", "add_trait = cruel"},
        {"remove_trait", "remove_trait = cruel"},
        {"die", "die = illness", &Scenario::lover},
        {"plan_death", "plan_death = { years = 2 }"},
        {"plan_death_date", "plan_death = 1710.05.01"},
        {"set_education", "set_education = { focus = martial  tutor = father }", &Scenario::son},
        {"conceive", "conceive = { father = spouse }", &Scenario::wife},
        {"create_person", "dynasty = { create_person = { sex = female  age = 16  mother = scope:other } }"},
        {"end_marriage", "end_marriage = annulment"},
        {"change_skill", "change_skill = { skill = martial  delta = 2 }"},
        {"change_health", "change_health = -10"},
        {"change_stress", "change_stress = 250"},
        {"record_chronicle", "dynasty = { record_chronicle = { key = duel  a = root  b = scope:other } }"},
        {"betroth",
         "scope.other = { dynasty = { betroth = { groom = root  bride = scope:other  dowry = 300 } } }",
         &Scenario::groom,
         &Scenario::bride},
        {"tonsure", "tonsure = yes", &Scenario::brother},
        {"set_heir", "dynasty = { set_heir = root }"},
        {"disown", "disown = yes", &Scenario::son},
        {"stubs",
         "dynasty = { assign_regent = { regent = root  kind = uncle }  dismiss_regent = dismissed  "
         "resolve_succession_dispute = { winner = root } }"},
    };
}

struct Compiled {
    pat::event_fixture::Compiled compiled;
    std::map<std::string, const Program*> options;
};

const Compiled& compiled_cases() {
    static const Compiled out = [] {
        // One event per case (an event has at most 4 options), all in one file.
        Compiled c;
        std::string events;
        for (const Case& k : cases()) {
            events += pat::event_fixture::character_event("immediate { save_scope_as = other }",
                                                          "option \"ok\" { effect { " + k.effect + " } }",
                                                          "t." + k.name);
        }
        c.compiled =
            pat::event_fixture::compile_event_files({{"events/t.pd", events}}, {}, Catalog::standard());
        return c;
    }();
    return out;
}

/// The program of the option of the event of the case `name`.
const Program& option_program(std::string_view name) {
    const Compiled& c = compiled_cases();
    INFO(c.compiled.all_errors());
    REQUIRE(c.compiled.ok());
    const std::string id = "t." + std::string(name);
    for (const EventDef& event : c.compiled.set->events) {
        if (event.id == id) {
            REQUIRE(event.options.front().effect.has_value());
            return *event.options.front().effect;
        }
    }
    FAIL("no event " << id);
    return *c.compiled.set->events.front().options.front().effect;
}

struct Translated {
    std::vector<Command> run;
    std::vector<Command> preview;
    std::vector<EffectIssue> issues;
    std::vector<EffectStatus> preview_status;
    OpBuffer buffer;
};

/// Runs the option over the scenario in Run and in Preview and translates both.
Translated run_case(const Scenario& s, const Case& k) {
    const Catalog catalog = Catalog::standard();
    const Program& program = option_program(k.name);
    const EffectEnv env = s.env();
    WorldVmHost host(s.world, catalog, env.triggers);
    Vm vm;
    RunOptions options;
    options.kind = Kind::Effect;
    options.root = ch(s.*k.root);
    options.saved[0] = ch(s.*k.other);

    Translated t;
    const RunResult run = vm.run(program, host, options);
    REQUIRE(run.ok);
    t.buffer = run.commands;
    (void)effects_to_commands(env, run.commands, t.run, &t.issues);

    PreviewRecorder sink;
    options.mode = Mode::Preview;
    options.preview = &sink;
    REQUIRE(vm.run(program, host, options).ok);
    for (const PreviewEvent& e : sink.events()) {
        if (e.type == PreviewEvent::Type::Line && e.line_kind == PreviewLineKind::Effect) {
            t.preview_status.push_back(effect_commands(env, e.code, e.scope, e.args, t.preview));
        }
    }
    return t;
}

const Case& case_named(std::string_view name) {
    static const std::vector<Case> all = cases();
    const auto it = std::ranges::find(all, name, &Case::name);
    REQUIRE(it != all.end());
    return *it;
}

/// The trait index the compiler pushed for `cruel` (from the Preview of add_trait), mapped to the slot.
void bind_trait(Scenario& s) {
    const Program& program = option_program("add_trait");
    const Catalog catalog = Catalog::standard();
    WorldVmHost host(s.world, catalog, s.env().triggers);
    Vm vm;
    RunOptions options;
    options.kind = Kind::Effect;
    options.root = ch(s.head);
    const RunResult r = vm.run(program, host, options);
    REQUIRE(r.ok);
    REQUIRE(r.commands.size() == 1);
    const auto index = static_cast<std::size_t>(int_from_word(r.commands.args_of(0)[0]));
    s.trait_slots.assign(index + 1, 0);
    s.trait_slots[index] = kSlotCruel;
}

/// Runs a case, checks Run == Preview and the actor (the system; for Tonsure the house of the person),
/// applies the commands; returns the log.
CommandLog apply_case(Scenario& s, std::string_view name, std::size_t expected_commands = 1) {
    const Case& k = case_named(name);
    const Translated t = run_case(s, k);
    CAPTURE(name);
    CHECK(t.run == t.preview);
    CHECK(t.issues.empty());
    REQUIRE(t.run.size() == expected_commands);
    for (const Command& c : t.run) {
        if (const auto* tonsure = std::get_if<cmd::Tonsure>(&c.payload)) {
            CHECK(c.actor == s.world.characters().dynasty[tonsure->character.index()]);
        } else {
            CHECK_FALSE(c.actor.valid());
        }
    }
    CommandLog log;
    const ApplyStats stats = process_commands(s.world, t.run, CommandSource::Internal, log, s.content());
    for (const LoggedCommand& e : log.entries()) {
        CAPTURE(to_string(type_of(e.command)));
        CHECK(e.accepted());
    }
    CHECK(stats.applied == expected_commands);
    pat::test::require_valid(s.world);
    return log;
}

template <class T>
T only(const CommandLog& log) {
    REQUIRE(log.size() == 1);
    REQUIRE(std::holds_alternative<T>(log.entries()[0].command.payload));
    return std::get<T>(log.entries()[0].command.payload);
}

bool predicate(const Scenario& s, CharacterId who, std::uint16_t code) {
    const auto r = eval_character_predicate(s.env().triggers, ch(who), code, {});
    REQUIRE(r.has_value());
    return r->value == 1;
}

}  // namespace

// ---- the table -----------------------------------------------------------------------------------------

TEST_CASE("character effects: the table of section 8.3 is registered with stable codes") {
    Catalog c = Catalog::engine();
    register_character_effects(c);
    const std::map<std::uint16_t, std::string_view> effects{
        {2001, "change_honor"},
        {2002, "change_unity"},
        {2003, "change_prestige"},
        {2004, "change_opinion"},
        {2005, "add_trait"},
        {2006, "remove_trait"},
        {2007, "die"},
        {2008, "plan_death"},
        {2009, "set_education"},
        {2010, "conceive"},
        {2011, "create_person"},
        {2012, "end_marriage"},
        {2013, "change_skill"},
        {2014, "change_health"},
        {2015, "change_stress"},
        {2016, "record_chronicle"},
        {2017, "betroth"},
        {2018, "tonsure"},
        {2019, "set_heir"},
        {2020, "disown"},
        {2021, "grant_land"},
        {2022, "assign_regent"},
        {2023, "dismiss_regent"},
        {2024, "resolve_succession_dispute"},
    };
    const std::set<std::string_view> stubs{
        "grant_land", "assign_regent", "dismiss_regent", "resolve_succession_dispute"};
    std::size_t registered = 0;
    for (const CatalogEntry& e : c.entries()) {
        if (e.space != NameSpace::Effect) {
            continue;
        }
        ++registered;
        CAPTURE(e.name);
        REQUIRE(effects.contains(e.code));
        CHECK(effects.at(e.code) == e.name);
        CHECK(c.find_code(NameSpace::Effect, e.code) == &e);
        CHECK_FALSE(e.example.empty());
        CHECK(e.stub == (stubs.contains(e.name) ? "E8" : ""));
        CHECK(e.from != 0);
        // the codes stay in the block of T09 and clear of the blocks of other epics (section 8.3)
        CHECK((e.code >= 2001 && e.code <= 2024));
    }
    CHECK(registered == effects.size());
    CHECK(Catalog::standard().find(NameSpace::Effect, "change_honor") != nullptr);
    CHECK(Catalog::standard().find(NameSpace::Predicate, "is_adult") != nullptr);

    // Codes of other blocks are not answered by the translation of this block.
    const Scenario s;
    std::vector<Command> out;
    CHECK_FALSE(character_effect_commands(s.env(), 2101, ch(s.head), {}, out).has_value());
    CHECK_FALSE(character_effect_commands(s.env(), 2305, ch(s.head), {}, out).has_value());
    CHECK(effect_commands(s.env(), 2101, ch(s.head), {}, out) == EffectStatus::Unknown);
    CHECK(out.empty());
}

TEST_CASE("character effects: every entry has a Russian template; golden strings") {
    pat::data::MemorySource source("tooltips");
    const char* file = "loc/ru/events/tooltip_effects_character.pd";
    source.add(file, pat::court_fixture::shipped_text(file));
    const auto loaded = pat::loc::load_localization(source);
    std::string report;
    for (const auto& error : loaded.errors) {
        report += error.format() + "\n";
    }
    INFO(report);
    REQUIRE(loaded.ok());
    const auto& strings = loaded.localizer;

    Catalog c = Catalog::engine();
    register_character_effects(c);
    std::size_t effects = 0;
    for (const CatalogEntry& e : c.entries()) {
        if (e.space != NameSpace::Effect) {
            continue;
        }
        ++effects;
        const std::string key = tooltip_key(e);
        CAPTURE(key);
        REQUIRE(strings.contains("ru", key));
        std::set<std::string> allowed;
        for (const ArgSpec& a : e.args) {
            allowed.insert(a.name);
        }
        for (const std::string& p : strings.parameters("ru", key)) {
            CHECK(allowed.contains(p));
        }
    }
    CHECK(effects == 24);

    using pat::loc::Arg;
    const std::vector<std::pair<std::string, std::string>> golden{
        {strings.format("tooltip.effect.change_honor", {Arg("value", "−5")}), "Честь рода −5"},
        {strings.format("tooltip.effect.change_unity", {Arg("value", "+2,5")}), "Единство рода +2,5"},
        {strings.format("tooltip.effect.change_prestige", {Arg("value", "+10")}), "Престиж рода +10"},
        {strings.format("tooltip.effect.change_opinion", {Arg("target", "Анна"), Arg("value", "−20")}),
         "Мнение: Анна −20"},
        {strings.format("tooltip.effect.add_trait", {Arg("trait", "Жестокий")}), "Получает черту «Жестокий»"},
        {strings.format("tooltip.effect.remove_trait", {Arg("trait", "Жестокий")}),
         "Теряет черту «Жестокий»"},
        {strings.format("tooltip.effect.die", {Arg("cause", "болезнь")}), "Умирает: болезнь"},
        {strings.format("tooltip.effect.plan_death", {Arg("date", "1 января 1705 г.")}),
         "Смерть суждена: 1 января 1705 г."},
        {strings.format("tooltip.effect.set_education", {Arg("focus", "военное дело")}),
         "Воспитание: военное дело"},
        {strings.format("tooltip.effect.conceive", {Arg("father", "Пётр")}),
         "Беременность, отец ребёнка — Пётр"},
        {strings.format("tooltip.effect.create_person", {Arg("sex", "женщина"), Arg("age", "16 лет")}),
         "В роду появляется человек: женщина, 16 лет"},
        {strings.format("tooltip.effect.end_marriage", {Arg("reason", "расторжение")}),
         "Брак прекращён: расторжение"},
        {strings.format("tooltip.effect.change_skill", {Arg("skill", "Военное дело"), Arg("delta", "+2")}),
         "Военное дело +2"},
        {strings.format("tooltip.effect.change_health", {Arg("delta", "−10")}), "Здоровье −10"},
        {strings.format("tooltip.effect.change_stress", {Arg("delta", "+15")}), "Стресс +15"},
        {strings.format("tooltip.effect.record_chronicle"), "Запись в летописи рода"},
        {strings.format("tooltip.effect.betroth", {Arg("groom", "Иван"), Arg("bride", "Мария")}),
         "Помолвка: Иван и Мария"},
        {strings.format("tooltip.effect.tonsure"), "Постриг в монашество"},
        {strings.format("tooltip.effect.set_heir", {Arg("heir", "Алексей")}), "Наследник рода: Алексей"},
        {strings.format("tooltip.effect.disown"), "Лишается наследства"},
        {strings.format("tooltip.effect.grant_land", {Arg("character", "Алексей"), Arg("province", "Тверь")}),
         "Алексей получает во владение Тверь"},
        {strings.format("tooltip.effect.assign_regent", {Arg("regent", "Фёдор")}), "Регент рода: Фёдор"},
        {strings.format("tooltip.effect.dismiss_regent"), "Регентство прекращено"},
        {strings.format("tooltip.effect.resolve_succession_dispute", {Arg("winner", "Алексей")}),
         "Спор о наследстве решён: Алексей"},
    };
    CHECK(golden.size() == effects);
    for (const auto& [have, want] : golden) {
        CHECK(have == want);
    }
}

// ---- the new commands ----------------------------------------------------------------------------------

TEST_CASE("character effects: AdjustSkill, AdjustHealth and AdjustStress validate, apply and serialize") {
    Scenario s;
    const auto check = [&](const Command& c) { return validate(s.world, c, s.content()); };
    const Command skill{DynastyId(), cmd::AdjustSkill{s.head, 2, fx32(15)}};
    const Command health{DynastyId(), cmd::AdjustHealth{s.head, fx32(-70)}};
    const Command stress{DynastyId(), cmd::AdjustStress{s.head, Fixed32::from_ratio(5, 2)}};
    for (const Command& c : {skill, health, stress}) {
        REQUIRE(check(c).has_value());
        // bytes -> command
        pat::core::ByteWriter out;
        write_command(out, c);
        pat::core::ByteReader in(out.data());
        Command back;
        REQUIRE(read_command(in, back));
        CHECK(back == c);
        CHECK(in.remaining() == 0);
    }
    CHECK(to_string(CommandType::AdjustSkill) == "AdjustSkill");
    CHECK(to_string(CommandType::AdjustHealth) == "AdjustHealth");
    CHECK(to_string(CommandType::AdjustStress) == "AdjustStress");
    CHECK(type_of(skill) == CommandType::AdjustSkill);
    CHECK(static_cast<int>(CommandType::AdjustSkill) == 104);
    CHECK(static_cast<int>(CommandType::AdjustHealth) == 105);
    CHECK(static_cast<int>(CommandType::AdjustStress) == 106);

    // refusals
    CHECK(check(Command{s.house_a, cmd::AdjustHealth{s.head, fx32(1)}}).error().reason ==
          Rejection::NotAuthorized);
    CHECK(check(Command{DynastyId(), cmd::AdjustSkill{s.head, 5, fx32(1)}}).error().reason ==
          Rejection::InvalidArgument);
    CHECK(check(Command{DynastyId(), cmd::AdjustStress{s.head, fx32(101)}}).error().reason ==
          Rejection::InvalidArgument);
    CHECK(check(Command{DynastyId(), cmd::AdjustStress{s.head, fx32(-101)}}).error().reason ==
          Rejection::InvalidArgument);
    CHECK(check(Command{DynastyId(), cmd::AdjustHealth{CharacterId(999), fx32(1)}}).error().reason ==
          Rejection::UnknownEntity);
    REQUIRE(s.world.die(s.lover, CauseOfDeath::Illness));
    CHECK(check(Command{DynastyId(), cmd::AdjustStress{s.lover, fx32(1)}}).error().reason ==
          Rejection::UnknownEntity);
    World e1{day(1703, 1, 1), 1};
    CharacterInit init;
    init.dynasty = e1.create_dynasty(StringId(0));
    init.birth_date = day(1680, 1, 1);
    const CharacterId plain = e1.create_character(init);
    CHECK(validate(e1, Command{DynastyId(), cmd::AdjustHealth{plain, fx32(1)}}).error().reason ==
          Rejection::InvalidState);

    // apply: clamped to the scales of the columns
    apply(s.world, skill, s.content());
    apply(s.world, health, s.content());
    apply(s.world, stress, s.content());
    const CharacterTable& t = s.world.characters();
    CHECK(t.skills[s.head.index()][2] == fx32(20));  // 10 + 15, clamped to 20
    CHECK(t.health[s.head.index()] == fx32(0));      // 60 - 70, clamped to 0
    CHECK(t.stress[s.head.index()] == Fixed32::from_ratio(45, 2));
    pat::test::require_valid(s.world);
}

// ---- every effect: Run == Preview, the commands and what they do --------------------------------------

TEST_CASE("character effects: the compiled options compile against the standard catalog") {
    const Compiled& c = compiled_cases();
    INFO(c.compiled.all_errors());
    REQUIRE(c.compiled.ok());
    CHECK(c.compiled.set->events.size() == cases().size());
}

TEST_CASE("character effects: the standing of a house (honor, unity, prestige) with the event reason") {
    Scenario s;
    const std::int32_t honor = s.world.dynasties().honor[s.house_a.index()];
    const auto& h = only<cmd::AdjustHonorWithReason>(apply_case(s, "change_honor"));
    CHECK(h.dynasty == s.house_a);
    CHECK(h.reason == kStandingReasonEvent);
    CHECK(s.world.dynasties().honor[s.house_a.index()] == honor - 5);

    (void)apply_case(s, "change_unity");
    CHECK(s.world.dynasties().unity[s.house_a.index()] == Fixed32::from_ratio(105, 2));
    const Fixed32 prestige = s.world.dynasties().prestige[s.house_a.index()];
    (void)apply_case(s, "change_prestige");
    CHECK(s.world.dynasties().prestige[s.house_a.index()] == prestige + fx32(10));
    const auto changes = s.world.standing_changes(s.house_a);
    REQUIRE(changes.size() >= 3);
    for (std::size_t i = changes.size() - 3; i < changes.size(); ++i) {
        CHECK(changes[i].reason == kStandingReasonEvent);
    }
}

TEST_CASE("character effects: opinion, traits, death and the planned death") {
    Scenario s;
    const auto& o = only<cmd::AdjustOpinion>(apply_case(s, "change_opinion"));
    CHECK(o.from == s.head);
    CHECK(o.to == s.wife);
    CHECK(o.reason == kOpinionReasonEvent + 3);
    CHECK(s.world.get_opinion(s.head, s.wife) == fx32(-20));

    bind_trait(s);
    CHECK(only<cmd::GrantTrait>(apply_case(s, "add_trait")).trait == kSlotCruel);
    CHECK(std::ranges::find(s.world.characters().traits[s.head.index()], kSlotCruel) !=
          s.world.characters().traits[s.head.index()].end());
    CHECK(only<cmd::RevokeTrait>(apply_case(s, "remove_trait")).trait == kSlotCruel);
    CHECK(std::ranges::find(s.world.characters().traits[s.head.index()], kSlotCruel) ==
          s.world.characters().traits[s.head.index()].end());

    CHECK(only<cmd::Die>(apply_case(s, "die")).cause == CauseOfDeath::Illness);
    CHECK_FALSE(s.world.is_alive(s.lover));
    REQUIRE(s.world.archived_person(s.lover).has_value());
    CHECK(s.world.archived_person(s.lover)->cause == CauseOfDeath::Illness);

    CHECK(only<cmd::PlanDeath>(apply_case(s, "plan_death")).date == day(1705, 1, 1));
    CHECK(s.world.characters().planned_death[s.head.index()] == day(1705, 1, 1));
    CHECK(only<cmd::PlanDeath>(apply_case(s, "plan_death_date")).date == day(1710, 5, 1));
}

TEST_CASE("character effects: education, conception, a new person and the end of a marriage") {
    Scenario s;
    const auto& e = only<cmd::SetEducation>(apply_case(s, "set_education"));
    CHECK(e.focus == 3);  // martial + 1
    CHECK(e.tutor == s.head);
    CHECK(s.world.characters().education_tutor[s.son.index()] == s.head);

    const auto& c = only<cmd::Conceive>(apply_case(s, "conceive"));
    CHECK(c.father == s.head);
    CHECK(c.due == day(1703, 10, 1));  // nine months, kDefaultDemography
    CHECK(c.legitimacy == Legitimacy::Legitimate);
    CHECK(predicate(s, s.wife, kTrigIsPregnant));

    const std::size_t living = s.world.living().size();
    const auto& p = only<cmd::CreatePerson>(apply_case(s, "create_person"));
    CHECK(p.init.dynasty == s.house_a);
    CHECK(p.init.sex == Sex::Female);
    CHECK(p.init.birth_date == day(1687, 1, 1));
    CHECK(p.init.mother == s.wife);
    CHECK(s.world.living().size() == living + 1);

    CHECK(only<cmd::EndMarriage>(apply_case(s, "end_marriage")).reason == MarriageEnd::Annulment);
    CHECK_FALSE(s.world.characters().spouse[s.head.index()].valid());
}

TEST_CASE("character effects: skill, health and stress, clamped to the limit of the command") {
    Scenario s;
    CHECK(only<cmd::AdjustSkill>(apply_case(s, "change_skill")).delta == fx32(2));
    CHECK(s.world.characters().skills[s.head.index()][static_cast<std::size_t>(Skill::Martial)] == fx32(12));
    (void)apply_case(s, "change_health");
    CHECK(s.world.characters().health[s.head.index()] == fx32(50));
    // 250 becomes the largest change the command accepts; the column stops at 100
    CHECK(only<cmd::AdjustStress>(apply_case(s, "change_stress")).delta == cmd::kMaxVitalDelta);
    CHECK(s.world.characters().stress[s.head.index()] == fx32(100));
}

TEST_CASE("character effects: chronicle, betrothal and tonsure") {
    Scenario s;
    const auto& r = only<cmd::RecordChronicle>(apply_case(s, "record_chronicle"));
    CHECK(r.dynasty == s.house_a);
    CHECK(r.a == s.head);
    CHECK(r.b == s.wife);
    CHECK((r.param & kChronicleEventBit) != 0);
    const auto chronicle = s.world.chronicle(s.house_a);
    REQUIRE_FALSE(chronicle.empty());
    CHECK(chronicle.back().param == r.param);
    CHECK(chronicle_event_param(0xFFFF'FFFF'FFFF'FFFFULL) == 0x7FFF'FFFF);
    CHECK(chronicle_event_param(0) == kChronicleEventBit);

    const auto& b = only<cmd::Betroth>(apply_case(s, "betroth"));
    CHECK(b.groom == s.groom);
    CHECK(b.bride == s.bride);
    CHECK(b.dowry_kop == 30'000);
    CHECK(b.dowry_source == s.house_b);
    CHECK(predicate(s, s.groom, kTrigIsBetrothed));

    (void)only<cmd::Tonsure>(apply_case(s, "tonsure"));
    CHECK(predicate(s, s.brother, kTrigIsTonsured));
}

TEST_CASE("character effects: set_heir and disown are issued by the house (E8-T15 commands)") {
    Scenario s;
    const Translated heir = run_case(s, case_named("set_heir"));
    CHECK(heir.run == heir.preview);
    REQUIRE(heir.run.size() == 1);
    CHECK(heir.run[0].actor == s.house_a);
    CHECK(std::get<cmd::SetHeir>(heir.run[0].payload).heir == s.head);
    const Translated disown = run_case(s, case_named("disown"));
    CHECK(disown.run == disown.preview);
    REQUIRE(disown.run.size() == 1);
    CHECK(disown.run[0].actor == s.house_a);
    CHECK(std::get<cmd::Disown>(disown.run[0].payload).character == s.son);
    // whatever the rules of succession answer, the commands go through validate and the log
    CommandLog log;
    const ApplyStats stats = process_commands(
        s.world, std::vector<Command>{disown.run[0]}, CommandSource::Internal, log, s.content());
    CHECK(stats.applied + stats.rejected == 1);
    CHECK(log.size() == 1);
}

TEST_CASE("character effects: the E8 stubs issue nothing and say so") {
    Scenario s;
    for (const char* name : {"stubs"}) {
        CAPTURE(name);
        const Translated t = run_case(s, case_named(name));
        CHECK(t.run.empty());
        CHECK(t.preview.empty());
        REQUIRE_FALSE(t.issues.empty());
        CHECK(t.issues.size() == t.buffer.size());
        for (const EffectIssue& i : t.issues) {
            CHECK(i.status == EffectStatus::Stub);
        }
        CHECK(
            std::ranges::all_of(t.preview_status, [](EffectStatus st) { return st == EffectStatus::Stub; }));
    }
    // grant_land needs a province scope, which a character event cannot reach without a list: directly
    std::vector<Command> out;
    const std::vector<Word> land{ch(s.son).word(), Scope::make(ScopeKind::Province, 0).word()};
    CHECK(effect_commands(
              s.env(), kEffGrantLand, Scope::make(ScopeKind::Dynasty, s.house_a.value()), land, out) ==
          EffectStatus::Stub);
    CHECK(out.empty());
    CHECK(std::string_view(effect_status_name(EffectStatus::Stub)) == "stub");
}

TEST_CASE("character effects: wrong scopes and arguments are reported, not issued") {
    const Scenario s;
    const EffectEnv env = s.env();
    std::vector<Command> out;
    const Scope house = Scope::make(ScopeKind::Dynasty, s.house_a.value());
    const Scope province = Scope::make(ScopeKind::Province, 0);
    CHECK(effect_commands(env, kEffChangeHealth, house, std::vector<Word>{word_from_int(1)}, out) ==
          EffectStatus::BadScope);
    CHECK(effect_commands(env, kEffChangeHonor, province, std::vector<Word>{word_from_int(1)}, out) ==
          EffectStatus::BadScope);
    // no target, an opinion reason out of range, an unknown trait, a cause of death out of range
    CHECK(
        effect_commands(env, kEffChangeOpinion, ch(s.head), std::vector<Word>{0, word_from_int(5), 0}, out) ==
        EffectStatus::BadArgument);
    CHECK(effect_commands(env,
                          kEffChangeOpinion,
                          ch(s.head),
                          std::vector<Word>{ch(s.wife).word(), word_from_int(5), word_from_int(100)},
                          out) == EffectStatus::BadArgument);
    CHECK(effect_commands(env, kEffAddTrait, ch(s.head), std::vector<Word>{word_from_int(0)}, out) ==
          EffectStatus::BadArgument);
    CHECK(effect_commands(env, kEffDie, ch(s.head), std::vector<Word>{word_from_int(9)}, out) ==
          EffectStatus::BadArgument);
    // plan_death without a date and without years; create_person with a negative age; betroth with a
    // negative dowry
    CHECK(effect_commands(env, kEffPlanDeath, ch(s.head), std::vector<Word>{0, word_from_int(-1)}, out) ==
          EffectStatus::BadArgument);
    CHECK(effect_commands(env, kEffCreatePerson, house, std::vector<Word>{0, word_from_int(-1), 0, 0}, out) ==
          EffectStatus::BadArgument);
    CHECK(effect_commands(env,
                          kEffBetroth,
                          house,
                          std::vector<Word>{ch(s.groom).word(), ch(s.bride).word(), word_from_int(-1), 0},
                          out) == EffectStatus::BadArgument);
    CHECK(out.empty());
    // a child of another man than the husband is illegitimate
    REQUIRE(effect_commands(env, kEffConceive, ch(s.wife), std::vector<Word>{ch(s.lover).word()}, out) ==
            EffectStatus::Issued);
    REQUIRE(out.size() == 1);
    CHECK(std::get<cmd::Conceive>(out[0].payload).legitimacy == Legitimacy::Illegitimate);
}

TEST_CASE("character effects: a refused command does not stop the others; the log keeps the refusal") {
    Scenario s;
    std::vector<Command> commands;
    const EffectEnv env = s.env();
    REQUIRE(
        effect_commands(env, kEffChangeHealth, ch(s.head), std::vector<Word>{word_from_int(-5)}, commands) ==
        EffectStatus::Issued);
    // an opinion of oneself: issued, refused by validate
    REQUIRE(effect_commands(env,
                            kEffChangeOpinion,
                            ch(s.head),
                            std::vector<Word>{ch(s.head).word(), word_from_int(5), 0},
                            commands) == EffectStatus::Issued);
    REQUIRE(
        effect_commands(env, kEffChangeStress, ch(s.head), std::vector<Word>{word_from_int(5)}, commands) ==
        EffectStatus::Issued);
    CommandLog log;
    const ApplyStats stats = process_commands(s.world, commands, CommandSource::Internal, log, s.content());
    CHECK(stats.applied == 2);
    CHECK(stats.rejected == 1);
    REQUIRE(log.size() == 3);
    CHECK(log.entries()[1].outcome == Rejection::InvalidArgument);
    CHECK(s.world.characters().health[s.head.index()] == fx32(55));
    CHECK(s.world.characters().stress[s.head.index()] == fx32(25));
}

// ---- replay -------------------------------------------------------------------------------------------

namespace {

/// A system that runs one compiled option over the head of house A on the first day of every month and
/// queues its commands, like the dispatcher will (E11-T12).
class EventEffectSystem final : public System {
public:
    EventEffectSystem(const Program& program, std::shared_ptr<const TraitCatalog> traits)
        : program_(program), traits_(std::move(traits)) {}

    [[nodiscard]] std::string_view name() const override { return "test.event_effects"; }
    [[nodiscard]] Cadence cadence() const override { return Cadence::Monthly; }
    void run(World& world, TickContext& context) override {
        const std::span<const CharacterId> members = world.members_of(DynastyId(0));
        if (members.empty()) {
            return;
        }
        EffectEnv env;
        env.triggers.world = &world;
        env.triggers.traits = traits_.get();
        WorldVmHost host(world, catalog_, env.triggers);
        RunOptions options;
        options.kind = Kind::Effect;
        options.root = ch(*std::ranges::min_element(members));
        options.saved[0] = options.root;
        const RunResult r = vm_.run(program_, host, options);
        if (r.ok) {
            std::vector<Command> out;
            (void)effects_to_commands(env, r.commands, out);
            for (Command& c : out) {
                context.emit(std::move(c));
            }
        }
    }

private:
    const Program& program_;
    std::shared_ptr<const TraitCatalog> traits_;
    Catalog catalog_ = Catalog::standard();
    Vm vm_;
};

Simulation effect_simulation(const Program& program) {
    Scenario s;
    auto traits = std::make_shared<const TraitCatalog>(s.traits);
    SimulationConfig config;
    config.traits = traits;
    Simulation sim(std::move(s.world), config);
    sim.add_system(std::make_unique<EventEffectSystem>(program, traits));
    return sim;
}

}  // namespace

TEST_CASE("character effects: a replay of a game with event commands reproduces the hashes") {
    const Program& program = option_program("change_skill");
    Simulation recorded = effect_simulation(program);
    ReplayRecorder recorder(9, 0xE11709ULL, recorded.world().today(), 30);
    for (int d = 0; d < 120; ++d) {
        (void)recorded.tick();
        recorder.after_tick(recorded);
    }
    const Replay replay = recorder.finish(recorded);
    // the system's commands were applied (Internal, regenerated by the replay, not stored in it)
    const CharacterId first = recorded.world().members_of(DynastyId(0)).front();
    CHECK(recorded.world().characters().skills[first.index()][static_cast<std::size_t>(Skill::Martial)] >
          fx32(10));

    auto parsed = deserialize_replay(serialize_replay(replay));
    REQUIRE(parsed.has_value());
    Simulation same = effect_simulation(program);
    const ReplayResult result = play_replay(same, parsed.value());
    CHECK(result.ok);
    CHECK(same.world().compute_hash() == recorded.world().compute_hash());
}
