// E11-T10: the effects of the province, the money and the economy of a province as commands, and the
// constructions of the flow over them. The catalog table (names, codes, signatures), the Russian tooltip
// templates (completeness and golden strings), the new commands PostEventMoney / AdjustProvinceUnrest /
// AdjustProvinceControl / AdjustGroupMood (validate, apply, bytes) and the ledger pair of the payments of
// events; for every effect one compiled event option run in Run and in Preview over a hand-made World: both
// give the same commands ("shown = applied"), the commands carry the system as actor, pass validate and do
// what they say. The flow: `if` / `else_if` / `else`, `every_`, `ordered_` give equal Run and Preview,
// `hidden_effect` is applied but not shown, `random_list` / `random` / `random_` apply only what Preview
// shows. Fifty years of a game whose events pay and receive keep the invariants of the accounts, and a
// replay reproduces the hashes.

#include "pat/core/bytes.hpp"
#include "pat/data/content_source.hpp"
#include "pat/loc/localizer.hpp"
#include "pat/sim/command.hpp"
#include "pat/sim/command_log.hpp"
#include "pat/sim/economy/ledger.hpp"
#include "pat/sim/economy/rules.hpp"
#include "pat/sim/economy/systems.hpp"
#include "pat/sim/economy/tables.hpp"
#include "pat/sim/economy/unrest.hpp"
#include "pat/sim/events/catalog.hpp"
#include "pat/sim/events/character_effects.hpp"
#include "pat/sim/events/effects.hpp"
#include "pat/sim/events/realm_effects.hpp"
#include "pat/sim/events/vm.hpp"
#include "pat/sim/events/world_host.hpp"
#include "pat/sim/replay.hpp"
#include "pat/sim/simulation.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
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
using economy::AccountRef;
using economy::Group;
using economy::Kopecks;
using economy::LedgerCode;
using pat::core::Fixed32;
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
Scope prov(ProvinceId id) {
    return Scope::make(ScopeKind::Province, id.value());
}
Scope house(DynastyId id) {
    return Scope::make(ScopeKind::Dynasty, id.value());
}

constexpr std::uint8_t kMill = 1;  // the type of the mill in EconomyRules::defaults()

/// Two houses: the head of A married to a woman of B, his brother (the administrator of the first province of
/// A) and the head of B. Provinces 0...2 belong to A (development 4, 2, 1), 3 to B. The first province is
/// restless (45), weakly held (25), its nobles sullen (-30); it has two free slots, a recruit pool of 640 and
/// 20 000 peasants. House A holds 10 000 roubles, the crown 500 000.
struct Scenario {
    World world{day(1703, 1, 1), 9};
    DynastyId house_a;
    DynastyId house_b;
    CharacterId head;
    CharacterId wife;
    CharacterId brother;
    CharacterId rival;
    std::vector<ProvinceId> provinces;
    economy::EconomyRules rules = economy::EconomyRules::defaults();
    std::vector<std::uint8_t> building_slots;  // registry index -> building type (bound from a run)

    CharacterId add(DynastyId of, Sex sex, Date birth) {
        CharacterInit init;
        init.dynasty = of;
        init.birth_date = birth;
        init.sex = sex;
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
        brother = add(house_a, Sex::Male, day(1672, 1, 1));
        rival = add(house_b, Sex::Male, day(1668, 1, 1));
        REQUIRE(world.marry(head, wife));
        REQUIRE(world.set_dynasty_head(house_a, head));
        REQUIRE(world.set_dynasty_head(house_b, rival));
        for (int i = 0; i < 4; ++i) {
            provinces.push_back(world.create_province(StringId(10 + i), i < 3 ? house_a : house_b, fx32(12)));
        }
        world.set_province_development(provinces[0], 4);
        world.set_province_development(provinces[1], 2);
        world.set_province_development(provinces[2], 1);
        REQUIRE(world.enable_economy());
        economy::EconomyColumnsMut cols = world.economy_columns_mut();
        cols.unrest[0] = fx32(45);
        cols.unrest[1] = fx32(10);
        cols.unrest[2] = fx32(0);
        cols.control[0] = fx32(25);
        cols.recruit_pool[0] = 640;
        cols.slots_total[0] = 2;
        cols.mood[economy::group_row(0, Group::Nobility)] = fx32(-30);
        REQUIRE(world.set_group_size(provinces[0], Group::Peasants, 20'000) == 20'000);
        REQUIRE(world.set_province_administrator(provinces[0], brother));
        REQUIRE(world.set_initial_treasury(AccountRef::of(house_a), 1'000'000));
        REQUIRE(world.set_initial_treasury(AccountRef::crown(), 50'000'000));
        pat::test::require_valid(world);
    }

    [[nodiscard]] EffectEnv env() const {
        EffectEnv e;
        e.triggers.world = &world;
        e.triggers.economy = &rules;
        e.triggers.building_slots = building_slots;
        return e;
    }

    [[nodiscard]] CommandContent content() const { return CommandContent{nullptr, &rules}; }
    [[nodiscard]] Kopecks treasury(AccountRef account) const { return world.treasury(account); }
    [[nodiscard]] Kopecks line(AccountRef account, LedgerCode code) const {
        return world.economy().accounts.open_line(*world.resolve_account(account), code);
    }
};

std::size_t incidents_of(const World& world, economy::IncidentKind kind) {
    std::size_t n = 0;
    for (std::size_t i = 0; i < world.economy().incidents.size(); ++i) {
        n += world.economy().incidents.kind[i] == static_cast<std::uint8_t>(kind) ? 1U : 0U;
    }
    return n;
}

// ---- one event with one option per effect ------------------------------------------------------------

constexpr std::string_view kFirst = "dynasty = { every_owned_province = { limit = { development >= 4 } ";

struct Case {
    std::string name;
    std::string effect;
};

std::vector<Case> cases() {
    const auto first = [](std::string_view body) { return std::string(kFirst) + std::string(body) + " } }"; };
    return {
        {"set_province_owner", first("set_province_owner = root.spouse.dynasty")},
        {"assign_administrator", first("assign_administrator = root")},
        {"dismiss_administrator", first("assign_administrator = { dismiss = yes }")},
        {"change_unrest", first("change_unrest = 30")},
        {"change_control", first("change_control = -10.5")},
        {"change_group_mood", first("change_group_mood = { group = nobility  delta = -80 }")},
        {"change_control_clamped", first("change_control = 250")},
        {"pay", "dynasty = { pay = 120 }"},
        {"receive", "dynasty = { receive = 50.5 }"},
        {"start_construction", first("start_construction = mill")},
        {"conscript", first("conscript = 200")},
        // the flow (section 8.3, 2150...2156)
        {"flow_if",
         "dynasty = { every_owned_province = { if = { limit = { unrest >= 40 } change_unrest = -5 } "
         "else_if = { limit = { unrest >= 5 } change_unrest = 1 } else = { change_control = 1 } } }"},
        {"flow_ordered",
         "dynasty = { ordered_owned_province = { order_by = unrest  max = 2  change_control = 2 } }"},
        {"flow_hidden", "dynasty = { pay = 1  hidden_effect = { receive = 2 } }"},
        {"flow_random",
         "dynasty = { random_list = { 30 = { pay = 1 } 70 = { receive = 1 } }  "
         "random = { chance = 50  receive = 3 }  random_owned_province = { change_unrest = 5 } }"},
    };
}

const pat::event_fixture::Compiled& compiled_cases() {
    static const pat::event_fixture::Compiled out = [] {
        std::string events;
        for (const Case& k : cases()) {
            events += pat::event_fixture::character_event(
                {}, "option \"ok\" { effect { " + k.effect + " } }", "t." + k.name);
        }
        return pat::event_fixture::compile_event_files({{"events/t.pd", events}}, {}, Catalog::standard());
    }();
    return out;
}

const Program& option_program(std::string_view name) {
    const pat::event_fixture::Compiled& c = compiled_cases();
    INFO(c.all_errors());
    REQUIRE(c.ok());
    const std::string id = "t." + std::string(name);
    for (const EventDef& event : c.set->events) {
        if (event.id == id) {
            REQUIRE(event.options.front().effect.has_value());
            return *event.options.front().effect;
        }
    }
    FAIL("no event " << id);
    return *c.set->events.front().options.front().effect;
}

struct Translated {
    std::vector<Command> run;
    std::vector<Command> preview;
    std::vector<EffectIssue> issues;
    std::vector<EffectStatus> preview_status;
};

/// Runs the option of the case over the head of house A in Run and in Preview and translates both.
Translated run_case(const Scenario& s, std::string_view name, std::uint64_t seed = 1) {
    const Catalog catalog = Catalog::standard();
    const Program& program = option_program(name);
    const EffectEnv env = s.env();
    WorldVmHost host(s.world, catalog, env.triggers);
    pat::core::Pcg32 rng(seed, 54);
    host.set_rng(&rng);
    Vm vm;
    RunOptions options;
    options.kind = Kind::Effect;
    options.root = ch(s.head);

    Translated t;
    const RunResult run = vm.run(program, host, options);
    REQUIRE(run.ok);
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

/// The index the compiler pushed for `mill` (from a run of start_construction), mapped to the mill.
void bind_mill(Scenario& s) {
    const Catalog catalog = Catalog::standard();
    WorldVmHost host(s.world, catalog, s.env().triggers);
    Vm vm;
    RunOptions options;
    options.kind = Kind::Effect;
    options.root = ch(s.head);
    const RunResult r = vm.run(option_program("start_construction"), host, options);
    REQUIRE(r.ok);
    REQUIRE(r.commands.size() == 1);
    const auto index = static_cast<std::size_t>(int_from_word(r.commands.args_of(0)[0]));
    s.building_slots.assign(index + 1, 0);
    s.building_slots[index] = kMill;
}

/// Runs a case, checks Run == Preview and the actor (the system), applies the commands; returns the log.
CommandLog apply_case(Scenario& s, std::string_view name, std::size_t expected_commands = 1) {
    const Translated t = run_case(s, name);
    CAPTURE(name);
    CHECK(t.run == t.preview);
    CHECK(t.issues.empty());
    REQUIRE(t.run.size() == expected_commands);
    for (const Command& c : t.run) {
        CHECK_FALSE(c.actor.valid());
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

const std::map<std::uint16_t, std::string_view>& table() {
    static const std::map<std::uint16_t, std::string_view> effects{
        {2101, "set_province_owner"},
        {2102, "assign_administrator"},
        {2103, "change_unrest"},
        {2104, "change_control"},
        {2105, "change_group_mood"},
        {2106, "pay"},
        {2107, "receive"},
        {2108, "start_construction"},
        {2109, "conscript"},
    };
    return effects;
}

}  // namespace

// ---- the table -----------------------------------------------------------------------------------------

TEST_CASE("realm effects: the table of section 8.3 is registered with stable codes") {
    Catalog c = Catalog::engine();
    register_realm_effects(c);
    std::size_t registered = 0;
    for (const CatalogEntry& e : c.entries()) {
        if (e.space != NameSpace::Effect) {
            continue;
        }
        ++registered;
        CAPTURE(e.name);
        REQUIRE(table().contains(e.code));
        CHECK(table().at(e.code) == e.name);
        CHECK(c.find_code(NameSpace::Effect, e.code) == &e);
        CHECK_FALSE(e.example.empty());
        CHECK(e.stub.empty());
        CHECK(e.from != 0);
        // the codes stay in the block of T10, clear of T09 (2001...2024), T11 (2201...) and the epics
        // (2300...)
        CHECK((e.code >= 2101 && e.code <= 2109));
    }
    CHECK(registered == table().size());
    const Catalog standard = Catalog::standard();
    CHECK(standard.find(NameSpace::Effect, "pay") != nullptr);
    CHECK(standard.find(NameSpace::Effect, "change_honor") != nullptr);

    // Each block answers its own codes only.
    const Scenario s;
    std::vector<Command> out;
    CHECK_FALSE(realm_effect_commands(s.env(), kEffChangeHonor, house(s.house_a), {}, out).has_value());
    CHECK_FALSE(character_effect_commands(s.env(), kEffPay, house(s.house_a), {}, out).has_value());
    CHECK(effect_commands(s.env(), 2201, house(s.house_a), {}, out) == EffectStatus::Unknown);
    CHECK(effect_commands(s.env(), 2150, house(s.house_a), {}, out) == EffectStatus::Unknown);
    CHECK(out.empty());
}

TEST_CASE("realm effects: every entry has a Russian template; golden strings") {
    pat::data::MemorySource source("tooltips");
    const char* file = "loc/ru/events/tooltip_effects_realm.pd";
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
    register_realm_effects(c);
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
    CHECK(effects == table().size());

    using pat::loc::Arg;
    const std::vector<std::pair<std::string, std::string>> golden{
        {strings.format("tooltip.effect.set_province_owner", {Arg("owner", "Шереметевых")}),
         "Провинция переходит к роду Шереметевых"},
        {strings.format("tooltip.effect.assign_administrator", {Arg("administrator", "Фёдор")}),
         "Управитель провинции: Фёдор"},
        {strings.format("tooltip.effect.change_unrest", {Arg("value", "+10")}), "Беспорядки +10"},
        {strings.format("tooltip.effect.change_control", {Arg("value", "−5")}), "Контроль −5"},
        {strings.format("tooltip.effect.change_group_mood", {Arg("group", "крестьяне"), Arg("delta", "−8")}),
         "Настроение (крестьяне) −8"},
        {strings.format("tooltip.effect.pay", {Arg("value", "120 ₽")}), "Казна рода: −120 ₽"},
        {strings.format("tooltip.effect.receive", {Arg("value", "50,5 ₽")}), "Казна рода: +50,5 ₽"},
        {strings.format("tooltip.effect.start_construction", {Arg("building", "мельница")}),
         "Начинается постройка: мельница"},
        {strings.format("tooltip.effect.conscript", {Arg("count", "200")}), "Набор рекрутов: 200"},
    };
    CHECK(golden.size() == effects);
    for (const auto& [have, want] : golden) {
        CHECK(have == want);
    }
}

// ---- the ledger and the new commands -------------------------------------------------------------------

TEST_CASE("realm effects: the payments of events are a ledger pair between a house and the crown") {
    CHECK(static_cast<int>(LedgerCode::EventIncome) == 17);
    CHECK(static_cast<int>(LedgerCode::EventExpense) == 113);
    CHECK(economy::is_issued(LedgerCode::EventIncome));
    CHECK(economy::is_issued(LedgerCode::EventExpense));
    CHECK(economy::is_ledger_pair(LedgerCode::EventExpense, LedgerCode::EventIncome));
    CHECK_FALSE(economy::is_ledger_pair(LedgerCode::EventIncome, LedgerCode::EventExpense));
    CHECK(economy::is_transfer_code(LedgerCode::EventIncome));
    CHECK(economy::is_transfer_code(LedgerCode::EventExpense));
    // (a single posting on a line of the pair is a bug World::post_money asserts against: only transfer())
}

TEST_CASE("realm effects: PostEventMoney and the province shifts validate, apply and serialize") {
    Scenario s;
    const auto check = [&](const Command& c) { return validate(s.world, c, s.content()); };
    const ProvinceId p0 = s.provinces[0];
    const Command pay{DynastyId(), cmd::PostEventMoney{s.house_a, 12'000, LedgerCode::EventExpense}};
    const Command gift{DynastyId(), cmd::PostEventMoney{s.house_a, 5'050, LedgerCode::EventIncome}};
    const Command unrest{DynastyId(), cmd::AdjustProvinceUnrest{p0, fx32(30)}};
    const Command control{DynastyId(), cmd::AdjustProvinceControl{p0, Fixed32::from_ratio(-21, 2)}};
    const Command mood{DynastyId(), cmd::AdjustGroupMood{p0, 0, fx32(-80)}};
    for (const Command& c : {pay, gift, unrest, control, mood}) {
        CAPTURE(to_string(type_of(c)));
        REQUIRE(check(c).has_value());
        pat::core::ByteWriter out;
        write_command(out, c);
        pat::core::ByteReader in(out.data());
        Command back;
        REQUIRE(read_command(in, back));
        CHECK(back == c);
        CHECK(in.remaining() == 0);
    }
    CHECK(to_string(CommandType::PostEventMoney) == "PostEventMoney");
    CHECK(to_string(CommandType::AdjustProvinceUnrest) == "AdjustProvinceUnrest");
    CHECK(to_string(CommandType::AdjustProvinceControl) == "AdjustProvinceControl");
    CHECK(to_string(CommandType::AdjustGroupMood) == "AdjustGroupMood");
    CHECK(static_cast<int>(CommandType::PostEventMoney) == 103);
    CHECK(static_cast<int>(CommandType::AdjustProvinceUnrest) == 107);
    CHECK(static_cast<int>(CommandType::AdjustProvinceControl) == 108);
    CHECK(static_cast<int>(CommandType::AdjustGroupMood) == 109);

    // refusals
    const auto reason = [&](const Command& c) { return check(c).error().reason; };
    CHECK(reason(Command{s.house_a, cmd::PostEventMoney{s.house_a, 1, LedgerCode::EventIncome}}) ==
          Rejection::NotAuthorized);
    CHECK(reason(Command{s.house_a, cmd::AdjustProvinceControl{p0, fx32(1)}}) == Rejection::NotAuthorized);
    CHECK(reason(Command{DynastyId(), cmd::PostEventMoney{s.house_a, 1, LedgerCode::Duties}}) ==
          Rejection::InvalidArgument);
    CHECK(reason(Command{DynastyId(), cmd::PostEventMoney{s.house_a, 0, LedgerCode::EventIncome}}) ==
          Rejection::InvalidArgument);
    CHECK(reason(Command{DynastyId(), cmd::PostEventMoney{DynastyId(77), 1, LedgerCode::EventIncome}}) ==
          Rejection::UnknownEntity);
    // a house pays only what it holds; the crown gives even into debt
    CHECK(reason(Command{DynastyId(), cmd::PostEventMoney{s.house_a, 1'000'001, LedgerCode::EventExpense}}) ==
          Rejection::InvalidState);
    CHECK(check(Command{DynastyId(), cmd::PostEventMoney{s.house_a, 60'000'000, LedgerCode::EventIncome}})
              .has_value());
    CHECK(reason(Command{DynastyId(), cmd::AdjustProvinceUnrest{p0, fx32(101)}}) ==
          Rejection::InvalidArgument);
    CHECK(reason(Command{DynastyId(), cmd::AdjustGroupMood{p0, 6, fx32(1)}}) == Rejection::InvalidArgument);
    CHECK(reason(Command{DynastyId(), cmd::AdjustProvinceControl{ProvinceId(99), fx32(1)}}) ==
          Rejection::UnknownEntity);
    // the states of unrest need the rules
    CHECK(validate(s.world, unrest, CommandContent{}).error().reason == Rejection::InvalidState);
    CHECK(validate(s.world, control, CommandContent{}).has_value());
    World plain{day(1703, 1, 1), 1};
    const ProvinceId bare = plain.create_province(StringId(0), DynastyId(), fx32(1));
    CHECK(validate(plain, Command{DynastyId(), cmd::AdjustProvinceControl{bare, fx32(1)}}).error().reason ==
          Rejection::InvalidState);

    // apply: one transfer each way, the columns clamped to their scales
    const Kopecks crown = s.treasury(AccountRef::crown());
    apply(s.world, pay, s.content());
    apply(s.world, gift, s.content());
    CHECK(s.treasury(AccountRef::of(s.house_a)) == 1'000'000 - 12'000 + 5'050);
    CHECK(s.treasury(AccountRef::crown()) == crown + 12'000 - 5'050);
    CHECK(s.line(AccountRef::of(s.house_a), LedgerCode::EventExpense) == -12'000);
    CHECK(s.line(AccountRef::of(s.house_a), LedgerCode::EventIncome) == 5'050);
    CHECK(s.line(AccountRef::crown(), LedgerCode::EventIncome) == 12'000);
    CHECK(s.line(AccountRef::crown(), LedgerCode::EventExpense) == -5'050);

    apply(s.world, unrest, s.content());   // 45 + 30 = 75: the uprising begins
    apply(s.world, control, s.content());  // 25 - 10.5
    apply(s.world, mood, s.content());     // -30 - 80, clamped to -100
    const economy::EconomyState& e = s.world.economy();
    CHECK(e.provinces.unrest[0] == fx32(75));
    CHECK(e.provinces.control[0] == Fixed32::from_ratio(29, 2));
    CHECK(e.groups.mood[economy::group_row(0, Group::Nobility)] == fx32(-100));
    CHECK(incidents_of(s.world, economy::IncidentKind::Uprising) == 1);
    CHECK(incidents_of(s.world, economy::IncidentKind::UnrestStateChanged) == 1);
    apply(s.world, Command{DynastyId(), cmd::AdjustProvinceControl{p0, fx32(-100)}}, s.content());
    CHECK(e.provinces.control[0] == fx32(0));
    apply(s.world, Command{DynastyId(), cmd::AdjustProvinceUnrest{p0, fx32(100)}}, s.content());
    CHECK(e.provinces.unrest[0] == fx32(100));
    CHECK(incidents_of(s.world, economy::IncidentKind::Uprising) == 1);  // still the same uprising
    pat::test::require_valid(s.world);
}

// ---- every effect: Run == Preview, the commands and what they do --------------------------------------

TEST_CASE("realm effects: the compiled options compile against the standard catalog") {
    const pat::event_fixture::Compiled& c = compiled_cases();
    INFO(c.all_errors());
    REQUIRE(c.ok());
    CHECK(c.set->events.size() == cases().size());
}

TEST_CASE("realm effects: the owner and the administrator of a province") {
    Scenario s;
    const auto owner = only<cmd::SetProvinceOwner>(apply_case(s, "set_province_owner"));
    CHECK(owner.province == s.provinces[0]);
    CHECK(owner.new_owner == s.house_b);
    CHECK(s.world.provinces().owner[0] == s.house_b);

    Scenario t;
    const auto appointed = only<cmd::AssignProvinceAdministrator>(apply_case(t, "assign_administrator"));
    CHECK(appointed.administrator == t.head);
    CHECK(t.world.economy().provinces.administrator[0] == t.head);
    const auto dismissed = only<cmd::AssignProvinceAdministrator>(apply_case(t, "dismiss_administrator"));
    CHECK_FALSE(dismissed.administrator.valid());
    CHECK_FALSE(t.world.economy().provinces.administrator[0].valid());
}

TEST_CASE("realm effects: unrest, control and the mood of a group, clamped to the scales") {
    Scenario s;
    CHECK(only<cmd::AdjustProvinceUnrest>(apply_case(s, "change_unrest")).delta == fx32(30));
    CHECK(s.world.economy().provinces.unrest[0] == fx32(75));
    CHECK(incidents_of(s.world, economy::IncidentKind::Uprising) == 1);
    CHECK(only<cmd::AdjustProvinceControl>(apply_case(s, "change_control")).delta ==
          Fixed32::from_ratio(-21, 2));
    CHECK(s.world.economy().provinces.control[0] == Fixed32::from_ratio(29, 2));
    const auto mood = only<cmd::AdjustGroupMood>(apply_case(s, "change_group_mood"));
    CHECK(mood.group == static_cast<std::uint8_t>(Group::Nobility));
    CHECK(s.world.economy().groups.mood[economy::group_row(0, Group::Nobility)] == fx32(-100));
    // 250 becomes the largest change the command accepts; the column stops at 100
    CHECK(only<cmd::AdjustProvinceControl>(apply_case(s, "change_control_clamped")).delta ==
          cmd::kMaxVitalDelta);
    CHECK(s.world.economy().provinces.control[0] == fx32(100));
}

TEST_CASE("realm effects: pay and receive move money between the house and the crown") {
    Scenario s;
    const Kopecks crown = s.treasury(AccountRef::crown());
    const auto paid = only<cmd::PostEventMoney>(apply_case(s, "pay"));
    CHECK(paid.dynasty == s.house_a);
    CHECK(paid.amount_kop == 12'000);
    CHECK(paid.ledger == LedgerCode::EventExpense);
    const auto got = only<cmd::PostEventMoney>(apply_case(s, "receive"));
    CHECK(got.amount_kop == 5'050);
    CHECK(got.ledger == LedgerCode::EventIncome);
    CHECK(s.treasury(AccountRef::of(s.house_a)) == 1'000'000 - 12'000 + 5'050);
    CHECK(s.treasury(AccountRef::crown()) + s.treasury(AccountRef::of(s.house_a)) == crown + 1'000'000);
}

TEST_CASE("realm effects: a construction and a levy through the commands of E9") {
    Scenario s;
    bind_mill(s);
    const Kopecks before = s.treasury(AccountRef::of(s.house_a));
    const auto build = only<cmd::StartConstruction>(apply_case(s, "start_construction"));
    CHECK(build.building == kMill);
    CHECK(s.world.economy().construction.size() == 1);
    CHECK(s.treasury(AccountRef::of(s.house_a)) == before - s.rules.buildings.defs[0].cost_kop);

    const auto levy = only<cmd::ConscriptRecruits>(apply_case(s, "conscript"));
    CHECK(levy.count == 200);
    CHECK(s.world.economy().provinces.recruit_pool[0] == 440);
}

// ---- the flow --------------------------------------------------------------------------------------------

TEST_CASE("realm effects: if / else_if / else, every_ and ordered_ apply what Preview shows") {
    Scenario s;
    const CommandLog branches = apply_case(s, "flow_if", 3);
    const auto& entries = branches.entries();
    CHECK(std::get<cmd::AdjustProvinceUnrest>(entries[0].command.payload) ==
          cmd::AdjustProvinceUnrest{s.provinces[0], fx32(-5)});
    CHECK(std::get<cmd::AdjustProvinceUnrest>(entries[1].command.payload) ==
          cmd::AdjustProvinceUnrest{s.provinces[1], fx32(1)});
    CHECK(std::get<cmd::AdjustProvinceControl>(entries[2].command.payload) ==
          cmd::AdjustProvinceControl{s.provinces[2], fx32(1)});

    // the two most restless provinces (40 and 11 now), by unrest descending
    const CommandLog ordered = apply_case(s, "flow_ordered", 2);
    CHECK(std::get<cmd::AdjustProvinceControl>(ordered.entries()[0].command.payload).province ==
          s.provinces[0]);
    CHECK(std::get<cmd::AdjustProvinceControl>(ordered.entries()[1].command.payload).province ==
          s.provinces[1]);
}

TEST_CASE("realm effects: hidden_effect is applied but not shown; random forms apply what Preview offers") {
    const Scenario s;
    const Translated hidden = run_case(s, "flow_hidden");
    REQUIRE(hidden.run.size() == 2);
    REQUIRE(hidden.preview.size() == 1);
    CHECK(hidden.run[0] == hidden.preview[0]);
    CHECK(std::get<cmd::PostEventMoney>(hidden.run[1].payload).ledger == LedgerCode::EventIncome);

    // Preview offers every branch and the body of random_ once ("one of N"); a Run applies some of the
    // branches and the body over one element of the list, never anything else.
    std::set<std::size_t> run_sizes;
    std::set<std::uint32_t> picked;
    for (std::uint64_t seed = 1; seed <= 16; ++seed) {
        const Translated t = run_case(s, "flow_random", seed);
        CAPTURE(seed);
        CHECK(t.issues.empty());
        CHECK(t.preview.size() == 4);  // two branches, one chance, the body of random_
        for (const Command& c : t.run) {
            if (const auto* shift = std::get_if<cmd::AdjustProvinceUnrest>(&c.payload)) {
                CHECK(shift->delta == fx32(5));
                CHECK(std::find(s.provinces.begin(), s.provinces.begin() + 3, shift->province) !=
                      s.provinces.begin() + 3);
                picked.insert(shift->province.value());
                continue;
            }
            CHECK(std::ranges::find(t.preview, c) != t.preview.end());
        }
        run_sizes.insert(t.run.size());
        // the same seed draws the same way
        CHECK(run_case(s, "flow_random", seed).run == t.run);
    }
    CHECK(run_sizes.size() == 2);  // with and without the chance
    CHECK(picked.size() > 1);      // random_ picks among the provinces of the house
}

// ---- errors and the implicit condition of `pay` --------------------------------------------------------

TEST_CASE("realm effects: wrong scopes and arguments are reported, not issued") {
    const Scenario s;
    const EffectEnv env = s.env();
    std::vector<Command> out;
    const Scope p0 = prov(s.provinces[0]);
    CHECK(effect_commands(env, kEffPay, p0, std::vector<Word>{word_from_int(1)}, out) ==
          EffectStatus::BadScope);
    CHECK(effect_commands(env, kEffPay, ch(s.head), std::vector<Word>{word_from_int(1)}, out) ==
          EffectStatus::BadScope);
    CHECK(
        effect_commands(env, kEffChangeUnrest, house(s.house_a), std::vector<Word>{word_from_int(1)}, out) ==
        EffectStatus::BadScope);
    // a payment of nothing or of less than a kopeck; an owner that is no house or nobody; an administrator
    // without a name or with a dismissal; an unknown group, building and an empty levy
    CHECK(effect_commands(env, kEffPay, house(s.house_a), std::vector<Word>{word_from_int(0)}, out) ==
          EffectStatus::BadArgument);
    CHECK(effect_commands(env, kEffReceive, house(s.house_a), std::vector<Word>{word_from_int(-5)}, out) ==
          EffectStatus::BadArgument);
    CHECK(effect_commands(env, kEffSetProvinceOwner, p0, std::vector<Word>{ch(s.head).word()}, out) ==
          EffectStatus::BadArgument);
    CHECK(effect_commands(env, kEffSetProvinceOwner, p0, std::vector<Word>{0}, out) ==
          EffectStatus::BadArgument);
    CHECK(effect_commands(env, kEffAssignAdministrator, p0, std::vector<Word>{0, 0}, out) ==
          EffectStatus::BadArgument);
    CHECK(effect_commands(env, kEffAssignAdministrator, p0, std::vector<Word>{ch(s.head).word(), 1}, out) ==
          EffectStatus::BadArgument);
    CHECK(effect_commands(env, kEffChangeGroupMood, p0, std::vector<Word>{word_from_int(6), 0}, out) ==
          EffectStatus::BadArgument);
    CHECK(effect_commands(env, kEffStartConstruction, p0, std::vector<Word>{word_from_int(0)}, out) ==
          EffectStatus::BadArgument);
    CHECK(effect_commands(env, kEffConscript, p0, std::vector<Word>{word_from_int(0)}, out) ==
          EffectStatus::BadArgument);
    CHECK(out.empty());
}

TEST_CASE(
    "realm effects: what a house lacks for the payments of an option; a refused payment stops nothing") {
    Scenario s;
    const auto money = [&](Kopecks kop, LedgerCode ledger) {
        return Command{DynastyId(), cmd::PostEventMoney{s.house_a, kop, ledger}};
    };
    const std::vector<Command> affordable{money(600'000, LedgerCode::EventExpense),
                                          money(400'000, LedgerCode::EventExpense)};
    CHECK(payment_shortfall(s.world, s.house_a, affordable) == 0);
    const std::vector<Command> too_much{money(600'000, LedgerCode::EventExpense),
                                        money(500'000, LedgerCode::EventExpense)};
    CHECK(payment_shortfall(s.world, s.house_a, too_much) == 100'000);
    // what the house receives first pays what follows; another house's payments do not count
    const std::vector<Command> funded{
        money(200'000, LedgerCode::EventIncome),
        money(1'100'000, LedgerCode::EventExpense),
        Command{DynastyId(), cmd::PostEventMoney{s.house_b, 9, LedgerCode::EventExpense}}};
    CHECK(payment_shortfall(s.world, s.house_a, funded) == 0);
    CHECK(payment_shortfall(s.world, s.house_b, funded) == 9);

    CommandLog log;
    const ApplyStats stats = process_commands(s.world, too_much, CommandSource::Internal, log, s.content());
    CHECK(stats.applied == 1);
    CHECK(stats.rejected == 1);
    CHECK(log.entries()[1].outcome == Rejection::InvalidState);
    CHECK(s.treasury(AccountRef::of(s.house_a)) == 400'000);
    pat::test::require_valid(s.world);
}

// ---- long runs and replay ----------------------------------------------------------------------------

namespace {

/// A system that runs one compiled option over the head of house A on the first day of every month and
/// queues its commands, like the dispatcher will (E11-T12).
class EventEffectSystem final : public System {
public:
    EventEffectSystem(const Program& program, std::shared_ptr<const economy::EconomyRules> rules)
        : program_(program), rules_(std::move(rules)) {}

    [[nodiscard]] std::string_view name() const override { return "test.event_effects"; }
    [[nodiscard]] Cadence cadence() const override { return Cadence::Monthly; }
    void run(World& world, TickContext& context) override {
        const std::span<const CharacterId> members = world.members_of(DynastyId(0));
        if (members.empty()) {
            return;
        }
        EffectEnv env;
        env.triggers.world = &world;
        env.triggers.economy = rules_.get();
        WorldVmHost host(world, catalog_, env.triggers);
        host.set_rng(&world.rng().stream("events.run"));
        RunOptions options;
        options.kind = Kind::Effect;
        options.root = ch(*std::ranges::min_element(members));
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
    std::shared_ptr<const economy::EconomyRules> rules_;
    Catalog catalog_ = Catalog::standard();
    Vm vm_;
};

const Program& monthly_program() {
    // Pay more than the house earns now and then, receive, shake the provinces: every kind of T10 command.
    static const pat::event_fixture::Compiled compiled = pat::event_fixture::compile_event_files(
        {{"events/t.pd",
          pat::event_fixture::character_event(
              {},
              "option \"ok\" { effect { dynasty = { "
              "random_list = { 50 = { pay = 300 } 50 = { receive = 250 } }  "
              "every_owned_province = { change_unrest = 3  change_control = -2  "
              "change_group_mood = { group = peasants  delta = -4 } } } } }",
              "t.monthly")}},
        {},
        Catalog::standard());
    INFO(compiled.all_errors());
    REQUIRE(compiled.ok());
    return *compiled.set->events.front().options.front().effect;
}

Simulation effect_simulation() {
    Scenario s;
    auto rules = std::make_shared<const economy::EconomyRules>(s.rules);
    Simulation sim(std::move(s.world));
    economy::install_economy_systems(sim, *rules);
    sim.add_system(std::make_unique<EventEffectSystem>(monthly_program(), rules));
    return sim;
}

}  // namespace

TEST_CASE("realm effects: fifty years of event payments keep the invariants of the accounts") {
    Simulation sim = effect_simulation();
    const auto event_lines = [&] {
        Kopecks sum = 0;
        const economy::AccountTable& accounts = sim.world().economy().accounts;
        for (std::size_t row = 0; row < accounts.size(); ++row) {
            sum += accounts.open_line(row, LedgerCode::EventIncome) +
                   accounts.open_line(row, LedgerCode::EventExpense);
        }
        return sum;
    };
    Kopecks moved = 0;
    for (int year = 0; year < 50; ++year) {
        for (int d = 0; d < 365; ++d) {
            (void)sim.tick();
            CHECK(event_lines() == 0);  // a payment of an event never makes or loses money
        }
        pat::test::require_valid(sim.world());  // E-M1: treasury == opening + sum of the lines, E-M3: pairs
        const economy::AccountTable& accounts = sim.world().economy().accounts;
        moved += accounts.open_line(1 + DynastyId(0).index(), LedgerCode::EventIncome);
    }
    CHECK(moved > 0);
    const economy::EconomyState& e = sim.world().economy();
    for (std::size_t p = 0; p < 3; ++p) {
        CHECK(e.provinces.unrest[p] >= Fixed32{});
        CHECK(e.provinces.unrest[p] <= fx32(100));
        CHECK(e.provinces.control[p] >= Fixed32{});
        CHECK(e.provinces.control[p] <= fx32(100));
    }
}

TEST_CASE("realm effects: a replay of a game with event commands reproduces the hashes") {
    Simulation recorded = effect_simulation();
    ReplayRecorder recorder(9, 0xE11710ULL, recorded.world().today(), 30);
    for (int d = 0; d < 400; ++d) {
        (void)recorded.tick();
        recorder.after_tick(recorded);
    }
    const Replay replay = recorder.finish(recorded);
    CHECK(recorded.world().economy().provinces.unrest[1] != fx32(10));  // the commands were applied

    auto parsed = deserialize_replay(serialize_replay(replay));
    REQUIRE(parsed.has_value());
    Simulation same = effect_simulation();
    const ReplayResult result = play_replay(same, parsed.value());
    CHECK(result.ok);
    CHECK(same.world().compute_hash() == recorded.world().compute_hash());
}
