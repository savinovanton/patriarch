#include "pat/content/event_content.hpp"
#include "pat/data/content_loader.hpp"
#include "pat/data/content_schemas.hpp"
#include "pat/data/event_schemas.hpp"
#include "pat/sim/date.hpp"
#include "pat/sim/events/catalog.hpp"
#include "pat/sim/events/event_set.hpp"
#include "pat/sim/events/vm.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "court_fixture.hpp"
#include "event_compile_fixture.hpp"
#include "event_fixture.hpp"
#include "test_files.hpp"

// E11-T06: schemas of the event kinds, the compiler from .pd blocks to programs, the EventSet.
// The compiler is tested on a small catalog (event_fixture.hpp) and a hand-made world for the host.

namespace ev = pat::sim::events;
namespace fx = pat::event_fixture;
using pat::core::Fixed64;
using pat::data::ErrorCode;

namespace {

using fx::Compiled;

const ev::Catalog& catalog() {
    static const ev::Catalog c = fx::test_catalog();
    return c;
}

Compiled compile_files(const std::vector<std::pair<std::string, std::string>>& files,
                       const pat::content::EventBuildOptions& options = {},
                       const ev::Catalog& cat = catalog()) {
    return fx::compile_event_files(files, options, cat);
}

Compiled compile(const std::string& body,
                 const pat::content::EventBuildOptions& options = {},
                 const ev::Catalog& cat = catalog()) {
    return compile_files({{"events/t.pd", body}}, options, cat);
}

using fx::character_event;

pat::data::SourcePos where(const Compiled& c, std::string_view anchor, std::string_view needle) {
    return pat::court_fixture::position_after(c.text, anchor, needle);
}

const ev::EventDef& only(const Compiled& c) {
    REQUIRE(c.ok());
    REQUIRE(c.set->events.size() == 1);
    return c.set->events.front();
}

// ---- the GDD 08 section 4.2 example, in .pd -----------------------------------------------------------

constexpr std::string_view kEstate = R"(event "estate.embezzling_administrator" {
    scope = province
    type = provincial
    category = economy
    review = draft
    on = monthly_pulse
    mean_time_to_happen = { months = 24
        modifier = { factor = 0.5  trigger = { corruption >= 0.5 } } }
    cooldown = { months = 36 }

    trigger {
        exists = administrator
        corruption > 0.3
    }
    immediate { administrator = { save_scope_as = governor } }
    speaker = scope:governor

    option "denounce" {
        ai_chance = { base = 20  add = { value = owner.honor  divide = 5 } }
        trigger { owner = { honor >= 30 } }
        effect {
            owner = { change_honor = -5 }
            change_control = 5
            scope.governor = { change_opinion = { target = root.owner.acting_head  value = -20 } }
        }
    }
    option "hush" {
        ai_chance = 40
        effect {
            owner = { pay = 200 }
            set_flag = { name = hushed_theft  days = 365 }
        }
    }
    option "ignore" {
        ai_chance = 40
        effect {
            add_modifier = { name = "estate.ignored_theft"  months = 12 }
            change_control = -3
        }
    }
}

event_modifier "estate.ignored_theft" {
    scope = province
    duration = { months = 12 }
    stack = refresh
    values = { corruption_hint = 1 }
}
)";

ev::RunResult run_program(const ev::Program& program,
                          fx::EventTestHost& host,
                          ev::Kind kind,
                          ev::Scope root,
                          ev::Mode mode = ev::Mode::Run,
                          const std::array<ev::Scope, ev::limits::kMaxSavedScopes>& saved = {}) {
    ev::Vm vm;
    ev::RunOptions options;
    options.mode = mode;
    options.kind = kind;
    options.root = root;
    options.saved = saved;
    return vm.run(program, host, options);
}

// The world of the example: province 5 of dynasty 10, administrator 31, acting head 77.
fx::EventTestHost estate_world(double /*unused*/ = 0) {
    fx::EventTestHost host;
    host.transitions[{fx::province(5).word(), 10}] = fx::character(31);
    host.transitions[{fx::province(5).word(), 9}] = fx::dynasty(10);
    host.transitions[{fx::dynasty(10).word(), 6}] = fx::character(77);
    host.values[{fx::province(5).word(), fx::kCorruption}] = Fixed64::from_ratio(2, 5).raw();
    host.values[{fx::dynasty(10).word(), fx::kHonor}] = fx::whole(40);
    return host;
}

}  // namespace

// ---- the data layer: Type::rules() and labeled fields -------------------------------------------------

TEST_CASE("event compiler: the schemas of the event kinds are registered and documented") {
    const pat::data::SchemaSet schemas = pat::data::content_schemas();
    for (const std::string_view kind : pat::data::kEventKinds) {
        const pat::data::ObjectSchema* schema = schemas.find(kind);
        REQUIRE(schema != nullptr);
        CHECK(schema->interned_last());
        CHECK_FALSE(schema->description().empty());
    }
    const pat::data::ObjectSchema* event = schemas.find(pat::data::kEventKind);
    const int trigger = event->find_field("trigger");
    REQUIRE(trigger >= 0);
    CHECK(event->fields()[static_cast<std::size_t>(trigger)].type.kind == pat::data::TypeKind::Rules);
    const int option = event->find_field("option");
    REQUIRE(option >= 0);
    CHECK(event->fields()[static_cast<std::size_t>(option)].labeled);
    CHECK(event->fields()[static_cast<std::size_t>(option)].repeatable);
}

// ---- the GDD example ------------------------------------------------------------------------------------

TEST_CASE("event compiler: the example of GDD 08 section 4.2 compiles and runs on a test world") {
    const Compiled c = compile(std::string(kEstate));
    INFO(c.all_errors());
    REQUIRE(c.ok());
    REQUIRE(c.set->events.size() == 1);
    REQUIRE(c.set->modifiers.size() == 1);
    const ev::EventDef& e = c.set->events.front();

    SUBCASE("the fields are read") {
        CHECK(e.id == "estate.embezzling_administrator");
        CHECK(e.key == ev::event_key("estate.embezzling_administrator"));
        CHECK(e.scope == ev::ScopeKind::Province);
        CHECK(e.type == ev::EventType::Provincial);
        CHECK(e.display == ev::Display::Modal);
        CHECK(e.review == ev::ReviewStatus::Draft);
        REQUIRE(e.on.size() == 1);
        CHECK(e.on[0].action == 2);  // monthly_pulse
        CHECK(e.cooldown_days == 36 * 30);
        CHECK(e.mtth_days == 24 * 30);
        // 24 months on the monthly pulse: 41 667 ppm (section 12.4)
        CHECK(e.mtth_ppm[static_cast<std::size_t>(ev::PulseKind::Monthly)] == 41667);
        REQUIRE(e.mtth_modifiers.size() == 1);
        CHECK(e.mtth_modifiers[0].factor_permille == 500);
        REQUIRE(e.options.size() == 3);
        CHECK(e.options[0].name == "denounce");
        CHECK(e.options[0].text_key == "evt.estate.embezzling_administrator.opt.denounce");
        CHECK(e.title_key == "evt.estate.embezzling_administrator.title");
        REQUIRE(e.saved_names.size() == 1);
        CHECK(e.saved_names[0] == "governor");
        CHECK(e.options[1].local_saved == 0);
        const ev::EventModifierDef& m = c.set->modifiers.front();
        CHECK(m.duration_days == 360);
        CHECK(m.scope == ev::ScopeKind::Province);
        CHECK(m.tooltip_key == "modifier.estate.ignored_theft");
        CHECK(m.values.size() == 1);
    }

    SUBCASE("the trigger holds only with an administrator and corruption above 0.3") {
        fx::EventTestHost host = estate_world();
        REQUIRE(e.trigger.has_value());
        CHECK(run_program(*e.trigger, host, ev::Kind::Trigger, fx::province(5)).truthy());
        host.values[{fx::province(5).word(), fx::kCorruption}] = Fixed64::from_ratio(1, 5).raw();
        CHECK_FALSE(run_program(*e.trigger, host, ev::Kind::Trigger, fx::province(5)).truthy());
        host.values[{fx::province(5).word(), fx::kCorruption}] = Fixed64::from_ratio(2, 5).raw();
        host.transitions.erase({fx::province(5).word(), 10});
        const ev::RunResult none = run_program(*e.trigger, host, ev::Kind::Trigger, fx::province(5));
        CHECK(none.ok);
        CHECK_FALSE(none.truthy());
    }

    SUBCASE("immediate saves the governor, the options read it") {
        fx::EventTestHost host = estate_world();
        REQUIRE(e.immediate.has_value());
        const ev::RunResult imm = run_program(*e.immediate, host, ev::Kind::Effect, fx::province(5));
        REQUIRE(imm.ok);
        CHECK(imm.saved[0] == fx::character(31));

        // the denounce option: three commands, the third one aimed at the saved governor
        REQUIRE(e.options[0].effect.has_value());
        const ev::RunResult run = run_program(
            *e.options[0].effect, host, ev::Kind::Effect, fx::province(5), ev::Mode::Run, imm.saved);
        REQUIRE(run.ok);
        REQUIRE(run.commands.size() == 3);
        CHECK(run.commands.entry(0).code == fx::kChangeHonor);
        CHECK(run.commands.entry(0).target == fx::dynasty(10));
        CHECK(run.commands.args_of(0)[0] == fx::whole(-5));
        CHECK(run.commands.entry(1).code == fx::kChangeControl);
        CHECK(run.commands.entry(1).target == fx::province(5));
        CHECK(run.commands.args_of(1)[0] == fx::whole(5));
        CHECK(run.commands.entry(2).code == fx::kChangeOpinion);
        CHECK(run.commands.entry(2).target == fx::character(31));
        CHECK(run.commands.args_of(2)[0] == fx::character(77).word());  // root.owner.acting_head
        CHECK(run.commands.args_of(2)[1] == fx::whole(-20));
    }

    SUBCASE("the other options emit their commands and state operations") {
        fx::EventTestHost host = estate_world();
        const ev::RunResult hush = run_program(*e.options[1].effect, host, ev::Kind::Effect, fx::province(5));
        REQUIRE(hush.ok);
        REQUIRE(hush.commands.size() == 1);
        CHECK(hush.commands.entry(0).code == fx::kPay);
        CHECK(hush.commands.args_of(0)[0] == fx::whole(200));
        REQUIRE(hush.state_ops.size() == 1);
        CHECK(hush.state_ops.entry(0).code == fx::kSetFlag);
        CHECK(hush.state_ops.args_of(0)[0] == static_cast<ev::Word>(ev::event_key("hushed_theft")));
        CHECK(hush.state_ops.args_of(0)[1] == fx::whole(365));
        const ev::RunResult ignore =
            run_program(*e.options[2].effect, host, ev::Kind::Effect, fx::province(5));
        REQUIRE(ignore.ok);
        REQUIRE(ignore.state_ops.size() == 1);
        CHECK(ignore.state_ops.entry(0).code == fx::kAddModifier);
        CHECK(ignore.state_ops.args_of(0)[0] == static_cast<ev::Word>(ev::event_key("estate.ignored_theft")));
        CHECK(ignore.state_ops.args_of(0)[1] == fx::whole(12));
        REQUIRE(ignore.commands.size() == 1);
        CHECK(ignore.commands.args_of(0)[0] == fx::whole(-3));
    }

    SUBCASE("availability and the AI weight") {
        fx::EventTestHost host = estate_world();
        REQUIRE(e.options[0].available.has_value());
        CHECK(run_program(*e.options[0].available, host, ev::Kind::Trigger, fx::province(5)).truthy());
        host.values[{fx::dynasty(10).word(), fx::kHonor}] = fx::whole(20);
        CHECK_FALSE(run_program(*e.options[0].available, host, ev::Kind::Trigger, fx::province(5)).truthy());
        host.values[{fx::dynasty(10).word(), fx::kHonor}] = fx::whole(40);
        // 20 + honor / 5 = 28; the plain weights are 40
        const ev::RunResult weight =
            run_program(*e.options[0].ai_chance, host, ev::Kind::Value, fx::province(5));
        REQUIRE(weight.ok);
        CHECK(weight.value == fx::whole(28));
        CHECK(run_program(*e.options[1].ai_chance, host, ev::Kind::Value, fx::province(5)).value ==
              fx::whole(40));
    }

    SUBCASE("the speaker is the saved governor, the MTTH modifier looks at corruption") {
        fx::EventTestHost host = estate_world();
        REQUIRE(e.speaker.has_value());
        std::array<ev::Scope, ev::limits::kMaxSavedScopes> saved{};
        saved[0] = fx::character(31);
        const ev::RunResult speaker =
            run_program(*e.speaker, host, ev::Kind::Value, fx::province(5), ev::Mode::Run, saved);
        REQUIRE(speaker.ok);
        CHECK(speaker.value == fx::character(31).word());
        CHECK_FALSE(
            run_program(e.mtth_modifiers[0].trigger, host, ev::Kind::Trigger, fx::province(5)).truthy());
        host.values[{fx::province(5).word(), fx::kCorruption}] = Fixed64::from_ratio(3, 5).raw();
        CHECK(run_program(e.mtth_modifiers[0].trigger, host, ev::Kind::Trigger, fx::province(5)).truthy());
    }

    SUBCASE("preview of an option lists what Run applies") {
        fx::EventTestHost host = estate_world();
        ev::PreviewRecorder sink;
        ev::Vm vm;
        ev::RunOptions options;
        options.mode = ev::Mode::Preview;
        options.kind = ev::Kind::Effect;
        options.root = fx::province(5);
        options.preview = &sink;
        options.saved[0] = fx::character(31);
        const ev::RunResult preview = vm.run(*e.options[0].effect, host, options);
        REQUIRE(preview.ok);
        std::vector<std::uint16_t> lines;
        for (const ev::PreviewEvent& event : sink.events()) {
            if (event.type == ev::PreviewEvent::Type::Line) {
                lines.push_back(event.code);
            }
        }
        CHECK(lines == std::vector<std::uint16_t>{fx::kChangeHonor, fx::kChangeControl, fx::kChangeOpinion});
    }

    SUBCASE("the disassembler names the catalog entries") {
        const std::string listing = ev::disassemble(*e.options[0].effect, &catalog());
        CHECK(listing.find("SCOPE_LOAD") != std::string::npos);
        CHECK(listing.find("PUSH_SCOPE") != std::string::npos);
        CHECK(listing.find("EMIT") != std::string::npos);
    }
}

// ---- constructs of the language, each run against the machine
// -----------------------------------------------

namespace {

// Compiles one event whose trigger / effect is `body` over `fields`; returns the single event.
Compiled one_event(std::string_view fields, std::string_view options = "option \"ok\" { }") {
    return compile(character_event(fields, options));
}

fx::EventTestHost family_world(std::uint64_t seed = 1) {
    fx::EventTestHost host(seed);
    const ev::Scope me = fx::character(1);
    host.transitions[{me.word(), 1}] = fx::dynasty(10);   // dynasty
    host.transitions[{me.word(), 4}] = fx::character(2);  // spouse
    host.transitions[{fx::character(2).word(), 1}] = fx::dynasty(11);
    host.values[{me.word(), fx::kAge}] = fx::whole(30);
    host.values[{fx::character(2).word(), fx::kAge}] = fx::whole(25);
    host.values[{fx::dynasty(10).word(), fx::kHonor}] = fx::whole(50);
    host.values[{fx::dynasty(11).word(), fx::kHonor}] = fx::whole(10);
    host.preds[{me.word(), fx::kIsAdult}] = true;
    host.lists[{me.word(), 2}] = {fx::character(3), fx::character(4), fx::character(5)};  // child
    host.values[{fx::character(3).word(), fx::kAge}] = fx::whole(5);
    host.values[{fx::character(4).word(), fx::kAge}] = fx::whole(17);
    host.values[{fx::character(5).word(), fx::kAge}] = fx::whole(20);
    host.preds[{fx::character(4).word(), fx::kIsAdult}] = true;
    host.preds[{fx::character(5).word(), fx::kIsAdult}] = true;
    return host;
}

bool holds(const Compiled& c, fx::EventTestHost& host) {
    REQUIRE(c.ok());
    const ev::EventDef& e = c.set->events.front();
    REQUIRE(e.trigger.has_value());
    const ev::RunResult r = run_program(*e.trigger, host, ev::Kind::Trigger, fx::character(1));
    REQUIRE(r.ok);
    return r.truthy();
}

}  // namespace

TEST_CASE("event compiler: triggers compile to the right logic") {
    fx::EventTestHost host = family_world();
    const auto trig = [&](std::string_view conditions) {
        const Compiled c = one_event(std::format("trigger {{ {} }}", conditions));
        INFO(c.all_errors());
        return holds(c, host);
    };

    SUBCASE("comparisons and predicates") {
        CHECK(trig("age >= 30"));
        CHECK_FALSE(trig("age > 30"));
        CHECK(trig("age != 31"));
        CHECK(trig("is_adult = yes"));
        CHECK_FALSE(trig("is_adult = no"));
        CHECK(trig("age >= 18  is_adult = yes"));
        CHECK_FALSE(trig("age >= 18  age < 18"));
    }
    SUBCASE("paths: an empty transition is false, in a comparison too") {
        CHECK(trig("spouse.age < 30"));
        CHECK_FALSE(trig("spouse.age >= 30"));
        CHECK(trig("exists = spouse"));
        CHECK_FALSE(trig("exists = father"));
        CHECK_FALSE(trig("father.age >= 0"));
        CHECK_FALSE(trig("age >= father.age"));
        CHECK(trig("age >= spouse.age"));
        CHECK(trig("spouse.dynasty.honor < dynasty.honor"));
        CHECK(trig("spouse = { is_adult = no }"));  // spouse 2 is no adult: `no` holds
        CHECK_FALSE(trig("father = { is_adult = no }"));
    }
    SUBCASE("and, or, not, nor") {
        CHECK(trig("or = { age >= 100  is_adult = yes }"));
        CHECK_FALSE(trig("or = { age >= 100  age < 0 }"));
        CHECK(trig("not = { age >= 100 }"));
        CHECK_FALSE(trig("not = { age >= 10  is_adult = yes }"));
        CHECK(trig("nor = { age >= 100  age < 0 }"));
        CHECK(trig("and = { age >= 10  or = { age < 0  is_adult = yes } }"));
    }
    SUBCASE("any_, all_ and count over a list") {
        CHECK(trig("any_child = { is_adult = yes }"));
        CHECK(trig("any_child = { age >= 20 }"));
        CHECK_FALSE(trig("any_child = { age >= 21 }"));
        CHECK(trig("any_child = { count >= 2  is_adult = yes }"));
        CHECK_FALSE(trig("any_child = { count >= 3  is_adult = yes }"));
        CHECK(trig("any_child = { count = 3 }"));
        CHECK(trig("all_child = { age >= 5 }"));
        CHECK_FALSE(trig("all_child = { age >= 6 }"));
        CHECK(trig("all_sibling = { age >= 100 }"));  // an empty list: vacuously true
        CHECK_FALSE(trig("any_sibling = { }"));
    }
    SUBCASE("values with a suffix, arguments and numbers of the right-hand side") {
        fx::EventTestHost h2 = family_world();
        // skill.martial is value 1022 with the suffix index 2 as the first argument
        h2.values[{fx::character(1).word(), fx::kSkill}] = fx::whole(9);
        const Compiled c = one_event("trigger { skill.martial >= 9 }");
        REQUIRE(c.ok());
        CHECK(holds(c, h2));
        REQUIRE_FALSE(h2.last_args.empty());
        CHECK(h2.last_args[0] == fx::whole(2));
        CHECK(trig("opinion_of = { target = spouse  value >= 0 }"));  // no opinion stored: 0 >= 0
    }
    SUBCASE("a date literal and a decimal") {
        CHECK_FALSE(trig("date >= 1703.05.16"));
        CHECK(trig("age > 29.5"));
    }
}

TEST_CASE("event compiler: the 'opinion_of' value block compares its value") {
    fx::EventTestHost host = family_world();
    host.values[{fx::character(1).word(), fx::kOpinionOf}] = fx::whole(15);
    const Compiled c = one_event("trigger { opinion_of = { target = spouse  value >= 10 } }");
    INFO(c.all_errors());
    CHECK(holds(c, host));
    REQUIRE_FALSE(host.last_args.empty());
    CHECK(host.last_args[0] == fx::character(2).word());
    const Compiled d = one_event("trigger { opinion_of = { target = spouse  value >= 20 } }");
    CHECK_FALSE(holds(d, host));
}

TEST_CASE("event compiler: accumulators") {
    fx::EventTestHost host = family_world();
    const auto weight = [&](std::string_view accumulator) {
        const Compiled c = one_event("", std::format("option \"o\" {{ ai_chance = {} }}", accumulator));
        INFO(c.all_errors());
        REQUIRE(c.ok());
        const auto& program = c.set->events.front().options.front().ai_chance;
        REQUIRE(program.has_value());
        const ev::RunResult r = run_program(*program, host, ev::Kind::Value, fx::character(1));
        REQUIRE(r.ok);
        return r.fixed();
    };
    CHECK(weight("40") == Fixed64::from_int(40));
    CHECK(weight("2.5") == Fixed64::from_ratio(5, 2));
    CHECK(weight("age") == Fixed64::from_int(30));
    CHECK(weight("{ base = 10  add = 5  subtract = 3  multiply = 2  divide = 4 }") ==
          Fixed64::from_ratio(12, 2));
    CHECK(weight("{ base = 10  min = 4  max = 7 }") == Fixed64::from_int(7));
    CHECK(weight("{ add = age }") == Fixed64::from_int(30));
    CHECK(weight("{ base = 1  if = { limit = { age >= 30 }  add = 10 } }") == Fixed64::from_int(11));
    CHECK(weight("{ base = 1  if = { limit = { age >= 31 }  add = 10 } }") == Fixed64::from_int(1));
    CHECK(weight("{ base = 7.9  round = yes }") == Fixed64::from_int(7));
    CHECK(weight("{ base = 0  add = { count_child = { limit = { is_adult = yes } } } }") ==
          Fixed64::from_int(2));
    CHECK(weight("{ base = 0  add = { value = spouse.age  multiply = 2 } }") == Fixed64::from_int(50));
    CHECK(weight("{ base = 0  add = { opinion_of = { target = spouse } } }") == Fixed64::from_int(0));
    CHECK(weight("{ base = 5  add = variable.reputation }") == Fixed64::from_int(5));
    // a path through an empty transition contributes 0, it does not abort
    CHECK(weight("{ base = 5  add = father.age }") == Fixed64::from_int(5));
}

TEST_CASE("event compiler: a variable is read through LOAD_VAR with the key of its name") {
    fx::EventTestHost host = family_world();
    host.vars[{fx::character(1).word(), ev::event_key("reputation")}] = fx::whole(8);
    host.vars[{-1, ev::event_key("year_mood")}] = fx::whole(3);
    CHECK(holds(one_event("trigger { variable.reputation >= 8 }"), host));
    CHECK_FALSE(holds(one_event("trigger { variable.reputation >= 9 }"), host));
    CHECK(holds(one_event("trigger { global_variable.year_mood = 3 }"), host));
}

TEST_CASE("event compiler: effects compile to commands in program order") {
    const auto run_effect = [](std::string_view effects) {
        const Compiled c = one_event("", std::format("option \"o\" {{ effect {{ {} }} }}", effects));
        INFO(c.all_errors());
        REQUIRE(c.ok());
        fx::EventTestHost host = family_world();
        const auto& program = c.set->events.front().options.front().effect;
        REQUIRE(program.has_value());
        const ev::RunResult r = run_program(*program, host, ev::Kind::Effect, fx::character(1));
        REQUIRE(r.ok);
        return r;
    };
    SUBCASE("scope blocks target the scope, an empty one is skipped") {
        const ev::RunResult r = run_effect("dynasty = { change_honor = 3 } father = { die = yes } spouse = { "
                                           "dynasty = { change_honor = 1 } }");
        REQUIRE(r.commands.size() == 2);
        CHECK(r.commands.entry(0).target == fx::dynasty(10));
        CHECK(r.commands.entry(1).target == fx::dynasty(11));
    }
    SUBCASE("if, else_if, else") {
        const char* chain = "if = { limit = { age >= 40 } dynasty = { change_honor = 1 } } "
                            "else_if = { limit = { age >= 30 } dynasty = { change_honor = 2 } } "
                            "else = { dynasty = { change_honor = 3 } }";
        const ev::RunResult r = run_effect(chain);
        REQUIRE(r.commands.size() == 1);
        CHECK(r.commands.args_of(0)[0] == fx::whole(2));
        const ev::RunResult first = run_effect("if = { limit = { age >= 10 } dynasty = { change_honor = 1 } "
                                               "} else = { dynasty = { change_honor = 3 } }");
        REQUIRE(first.commands.size() == 1);
        CHECK(first.commands.args_of(0)[0] == fx::whole(1));
        const ev::RunResult last = run_effect("if = { limit = { age >= 99 } dynasty = { change_honor = 1 } } "
                                              "else = { dynasty = { change_honor = 3 } }");
        REQUIRE(last.commands.size() == 1);
        CHECK(last.commands.args_of(0)[0] == fx::whole(3));
        CHECK(run_effect("if = { limit = { age >= 99 } dynasty = { change_honor = 1 } }").commands.empty());
    }
}

TEST_CASE("event compiler: iterators in effects") {
    const auto run_effect = [](std::string_view effects, std::uint64_t seed = 1) {
        const Compiled c = one_event("", std::format("option \"o\" {{ effect {{ {} }} }}", effects));
        INFO(c.all_errors());
        REQUIRE(c.ok());
        fx::EventTestHost h = family_world(seed);
        const auto& program = c.set->events.front().options.front().effect;
        REQUIRE(program.has_value());
        const ev::RunResult r = run_program(*program, h, ev::Kind::Effect, fx::character(1));
        REQUIRE(r.ok);
        return r;
    };
    const auto die_targets = [](const ev::RunResult& r) {
        std::vector<std::uint32_t> ids;
        for (const auto& entry : r.commands.entries()) {
            ids.push_back(entry.target.id);
        }
        return ids;
    };
    SUBCASE("every_ walks the list in host order and honours limit") {
        const ev::RunResult r = run_effect("every_child = { limit = { age >= 17 }  die = yes }");
        CHECK(die_targets(r) == std::vector<std::uint32_t>{4, 5});
    }
    SUBCASE("ordered_ takes the highest first, then the next, ties by id") {
        const ev::RunResult r = run_effect("ordered_child = { order_by = age  max = 2  die = yes }");
        CHECK(die_targets(r) == std::vector<std::uint32_t>{5, 4});
        const ev::RunResult one = run_effect("ordered_child = { order_by = age  die = yes }");
        CHECK(die_targets(one) == std::vector<std::uint32_t>{5});
        const ev::RunResult filtered =
            run_effect("ordered_child = { limit = { age < 18 }  order_by = age  die = yes }");
        CHECK(die_targets(filtered) == std::vector<std::uint32_t>{4});
    }
    SUBCASE("random_ picks exactly one element that passes the limit") {
        const ev::RunResult r = run_effect("random_child = { limit = { is_adult = yes }  die = yes }");
        const std::vector<std::uint32_t> ids = die_targets(r);
        REQUIRE(ids.size() == 1);
        CHECK((ids[0] == 4 || ids[0] == 5));
        CHECK(r.counters.draws == 1);
        // a weight of zero for adults and one for the baby: the baby is always picked
        const ev::RunResult weighted = run_effect(
            "random_child = { weight = { base = 0  if = { limit = { age < 6 }  add = 1 } }  die = yes }");
        CHECK(die_targets(weighted) == std::vector<std::uint32_t>{3});
    }
}

TEST_CASE("event compiler: random_list and random take exactly one draw") {
    const Compiled c = one_event("",
                                 R"(option "o" { effect {
        random_list = {
            30 = { dynasty = { change_honor = 1 } }
            70 = { dynasty = { change_honor = 2 } dynasty = { change_honor = 3 } }
        }
        random = { chance = 25  dynasty = { change_honor = 9 } }
    } })");
    INFO(c.all_errors());
    REQUIRE(c.ok());
    const ev::Program& program = *c.set->events.front().options.front().effect;
    std::set<std::size_t> sizes;
    std::set<std::int64_t> firsts;
    for (std::uint64_t seed = 1; seed <= 60; ++seed) {
        fx::EventTestHost host = family_world(seed);
        const ev::RunResult r = run_program(program, host, ev::Kind::Effect, fx::character(1));
        REQUIRE(r.ok);
        CHECK(r.counters.draws == 2);
        REQUIRE(r.commands.size() >= 1);
        firsts.insert(r.commands.args_of(0)[0] / fx::whole(1));
        sizes.insert(r.commands.size());
    }
    CHECK(firsts.count(1) == 1);
    CHECK(firsts.count(2) == 1);
    CHECK(sizes.size() >= 2);

    // Preview lists both branches with their shares, shown = applied
    fx::EventTestHost host = family_world();
    ev::PreviewRecorder sink;
    ev::Vm vm;
    ev::RunOptions options;
    options.mode = ev::Mode::Preview;
    options.kind = ev::Kind::Effect;
    options.root = fx::character(1);
    options.preview = &sink;
    REQUIRE(vm.run(program, host, options).ok);
    std::vector<std::uint32_t> shares;
    for (const ev::PreviewEvent& event : sink.events()) {
        if (event.type == ev::PreviewEvent::Type::GroupBegin) {
            shares.push_back(event.value);
        }
    }
    CHECK(shares == std::vector<std::uint32_t>{300000, 700000, 250000});
}

TEST_CASE("event compiler: hidden_effect and save_scope_as inside an option") {
    const Compiled c = one_event("",
                                 R"(option "o" { effect {
        spouse = { save_scope_as = partner }
        hidden_effect = { scope.partner = { die = yes } }
        scope.partner = { dynasty = { change_honor = 1 } }
    } })");
    INFO(c.all_errors());
    REQUIRE(c.ok());
    const ev::EventDef& e = c.set->events.front();
    CHECK(e.options.front().local_saved == 1);  // the option's own saved scope
    CHECK(e.saved_names.empty());
    fx::EventTestHost host = family_world();
    ev::PreviewRecorder sink;
    ev::Vm vm;
    ev::RunOptions options;
    options.mode = ev::Mode::Preview;
    options.kind = ev::Kind::Effect;
    options.root = fx::character(1);
    options.preview = &sink;
    REQUIRE(vm.run(*e.options.front().effect, host, options).ok);
    std::size_t lines = 0;
    for (const ev::PreviewEvent& event : sink.events()) {
        lines += event.type == ev::PreviewEvent::Type::Line ? 1U : 0U;
    }
    CHECK(lines == 1);  // the hidden die is not listed
}

// ---- timing and the other fields
// ----------------------------------------------------------------------------

TEST_CASE("event compiler: mean time to happen in ppm without floating point") {
    CHECK(ev::mtth_ppm(30, 720) == 41667);     // 24 months, monthly pulse
    CHECK(ev::mtth_ppm(365, 365) == 1000000);  // once a year on the yearly pulse
    CHECK(ev::mtth_ppm(30, 30) == 1000000);
    CHECK(ev::mtth_ppm(30, 10) == 1000000);  // clamped
    CHECK(ev::mtth_ppm(1, 100000000) == 1);  // clamped up to 1
    CHECK(ev::mtth_ppm(0, 10) == 0);
    CHECK(ev::mtth_ppm(30, 0) == 0);

    const Compiled c = compile(R"(event "t.pulses" {
    scope = character
    type = personal
    category = general
    on = [monthly_pulse, yearly_pulse, quarterly_pulse]
    mean_time_to_happen = { years = 2 }
    option "ok" { }
}
)");
    INFO(c.all_errors());
    const ev::EventDef& e = only(c);
    CHECK(e.mtth_days == 730);
    CHECK(e.mtth_ppm[static_cast<std::size_t>(ev::PulseKind::Monthly)] == ev::mtth_ppm(30, 730));
    CHECK(e.mtth_ppm[static_cast<std::size_t>(ev::PulseKind::Quarterly)] == ev::mtth_ppm(91, 730));
    CHECK(e.mtth_ppm[static_cast<std::size_t>(ev::PulseKind::Yearly)] == ev::mtth_ppm(365, 730));
    CHECK(e.mtth_ppm[static_cast<std::size_t>(ev::PulseKind::Daily)] == 0);
}

TEST_CASE("event compiler: fields, defaults, texts and the window") {
    SUBCASE("defaults follow the type and display") {
        const Compiled c = compile(R"(event "t.quiet" {
    scope = dynasty
    type = hidden
    category = general
    on = yearly_pulse
    immediate { change_honor = 1 }
}
event "t.news" {
    scope = dynasty
    type = imperial
    category = court
    on = yearly_pulse
    option "ok" { }
}
)");
        INFO(c.all_errors());
        REQUIRE(c.ok());
        const ev::EventDef& news = *c.set->find("t.news");
        CHECK(news.display == ev::Display::Notification);
        CHECK(news.urgency == ev::Urgency::Info);
        CHECK(news.priority == 50);
        const ev::EventDef& quiet = *c.set->find("t.quiet");
        CHECK(quiet.display == ev::Display::Silent);
        CHECK(quiet.options.empty());
        CHECK(quiet.immediate.has_value());
    }
    SUBCASE("window, cooldown group, pool, once, history and sources") {
        const Compiled c = compile(R"(event "t.full" {
    scope = character
    type = court
    category = court
    image = court_hall
    tags = [one, "two"]
    on = on_death
    window = { from = 1703.01.01  to = 1710.12.31  anchor = yes }
    cooldown_group = grp
    pool = p1
    weight = 3
    once = global
    history = checked
    sources = ["a", "b"]
    priority = 80
    urgency = critical
    autopause = always
    rumor = yes
    option "a" { default = yes }
    option "b" { neutral = yes }
}
)");
        INFO(c.all_errors());
        const ev::EventDef& e = only(c);
        CHECK(e.has_window);
        CHECK(e.anchor);
        CHECK(e.window_from == pat::sim::Date::from_ymd(1703, 1, 1).days());
        CHECK(e.window_to == pat::sim::Date::from_ymd(1710, 12, 31).days());
        CHECK(e.cooldown_group == ev::event_key("grp"));
        CHECK(e.pool == ev::event_key("p1"));
        CHECK(e.weight.has_value());
        CHECK(e.once == ev::OnceMode::Global);
        CHECK(e.history == ev::HistoryStatus::Checked);
        CHECK(e.sources == std::vector<std::string>{"a", "b"});
        CHECK(e.priority == 80);
        CHECK(e.urgency == ev::Urgency::Critical);
        CHECK(e.autopause == ev::AutoPause::Always);
        CHECK(e.rumor);
        CHECK(e.tags == std::vector<std::string>{"one", "two"});
        CHECK(e.on[0].action == 8);
        CHECK(e.on[0].role == 0);
        CHECK(e.options[0].is_default);
        CHECK(e.options[1].neutral);
        CHECK(e.image == "court_hall");
        CHECK(c.set->name_of_key(ev::event_key("grp")) == "grp");
    }
    SUBCASE("roles of an on-action and the scopes it provides") {
        const Compiled c = compile(R"(event "t.bride" {
    scope = character
    type = family
    category = family
    on = on_marriage.bride
    immediate { scope.groom = { die = yes } }
    option "ok" { }
}
)");
        INFO(c.all_errors());
        const ev::EventDef& e = only(c);
        REQUIRE(e.on.size() == 1);
        CHECK(e.on[0].action == 9);
        CHECK(e.on[0].role == 1);
        CHECK(e.provided_saved == 2);
        CHECK(e.saved_names == std::vector<std::string>{"groom", "bride"});
    }
}

// ---- errors
// ---------------------------------------------------------------------------------------------------------

TEST_CASE("event compiler: a province trigger in a character scope blocks the load with a clear message") {
    const Compiled c = compile(character_event("trigger { corruption > 0.3 }"));
    REQUIRE_FALSE(c.ok());
    REQUIRE(c.errors.size() == 1);
    const auto& d = c.errors[0].diagnostic;
    CHECK(d.code == ErrorCode::EventScopeMismatch);
    CHECK(pat::data::error_code_name(d.code) == "EV102");
    CHECK(d.pos == where(c, "trigger {", "corruption"));
    CHECK(d.message.find("province") != std::string::npos);
    CHECK(d.message.find("character") != std::string::npos);
    CHECK(c.all_errors().find("events/t.pd:") != std::string::npos);
    CHECK(c.all_errors().find("error[EV102]") != std::string::npos);
}

TEST_CASE("event compiler: unknown names are EV101 with a hint") {
    SUBCASE("a trigger") {
        const Compiled c = compile(character_event("trigger { is_adlt = yes }"));
        REQUIRE(c.errors.size() == 1);
        CHECK(c.errors[0].diagnostic.code == ErrorCode::EventUnknownName);
        CHECK(c.errors[0].diagnostic.pos == where(c, "trigger {", "is_adlt"));
        CHECK(c.errors[0].diagnostic.message.find("did you mean 'is_adult'") != std::string::npos);
    }
    SUBCASE("an effect and a transition") {
        const Compiled c = compile(character_event("", "option \"o\" { effect { change_honr = 1 } }"));
        REQUIRE(c.errors.size() == 1);
        CHECK(c.errors[0].diagnostic.code == ErrorCode::EventUnknownName);
        const Compiled d = compile(character_event("trigger { spose.age > 3 }"));
        REQUIRE(d.errors.size() == 1);
        CHECK(d.errors[0].diagnostic.message.find("did you mean 'spouse'") != std::string::npos);
    }
    SUBCASE("an unknown list") {
        const Compiled c = compile(character_event("trigger { any_childs = { } }"));
        REQUIRE(c.errors.size() == 1);
        CHECK(c.errors[0].diagnostic.code == ErrorCode::EventUnknownName);
        CHECK(c.errors[0].diagnostic.message.find("child") != std::string::npos);
    }
}

TEST_CASE("event compiler: broken references are EV104 with a position") {
    SUBCASE("a trait") {
        const Compiled c = compile(character_event("trigger { has_trait = greedyy }"));
        REQUIRE(c.errors.size() == 1);
        CHECK(c.errors[0].diagnostic.code == ErrorCode::EventUnresolvedReference);
        CHECK(c.errors[0].diagnostic.pos == where(c, "trigger {", "greedyy"));
        CHECK(c.errors[0].diagnostic.message.find("did you mean 'greedy'") != std::string::npos);
        CHECK(compile(character_event("trigger { has_trait = greedy }")).ok());
    }
    SUBCASE("an event of a chain, a modifier") {
        const Compiled c =
            compile(character_event("", R"(option "o" { effect { trigger_event = { id = t.missing } } })"));
        REQUIRE(c.errors.size() == 1);
        CHECK(c.errors[0].diagnostic.code == ErrorCode::EventUnresolvedReference);
        CHECK(c.errors[0].diagnostic.pos == where(c, "trigger_event", "t.missing"));
        const Compiled m =
            compile(character_event("", R"(option "o" { effect { add_modifier = { name = nope } } })"));
        REQUIRE(m.errors.size() == 1);
        CHECK(m.errors[0].diagnostic.code == ErrorCode::EventUnresolvedReference);
    }
    SUBCASE("a saved scope that was never saved") {
        const Compiled c =
            compile(character_event("", R"(option "o" { effect { scope.ghost = { die = yes } } })"));
        REQUIRE(c.errors.size() == 1);
        CHECK(c.errors[0].diagnostic.code == ErrorCode::EventUnresolvedReference);
        CHECK(c.errors[0].diagnostic.pos == where(c, "effect", "ghost"));  // the name, not the path
    }
    SUBCASE("a localization key and an image") {
        pat::content::EventBuildOptions options;
        options.has_loc_key = [](std::string_view key) {
            return key != "evt.t.ev.desc" && key != "evt.t.ev.opt.ok";
        };
        options.has_image = [](std::string_view pool) { return pool != "general"; };
        const Compiled c = compile(character_event(), options);
        REQUIRE_FALSE(c.ok());
        std::vector<ErrorCode> expected{
            ErrorCode::EventUnresolvedReference, ErrorCode::EventMissingText, ErrorCode::EventMissingText};
        std::vector<ErrorCode> codes = c.codes();
        std::sort(codes.begin(), codes.end());
        std::sort(expected.begin(), expected.end());
        CHECK(codes == expected);
        CHECK(c.all_errors().find("evt.t.ev.desc") != std::string::npos);
        CHECK(c.all_errors().find("evt.t.ev.opt.ok") != std::string::npos);
        CHECK(c.all_errors().find("image pool 'general'") != std::string::npos);
        // every key present: fine
        pat::content::EventBuildOptions all;
        all.has_loc_key = [](std::string_view) { return true; };
        all.has_image = [](std::string_view) { return true; };
        CHECK(compile(character_event(), all).ok());
    }
}

TEST_CASE("event compiler: two events with one id are an error of the loader") {
    const Compiled c =
        compile_files({{"events/a.pd", character_event()}, {"events/b.pd", character_event()}});
    REQUIRE_FALSE(c.ok());
    REQUIRE(c.errors.size() == 1);
    CHECK(c.errors[0].diagnostic.code == ErrorCode::DuplicateObject);
}

TEST_CASE("event compiler: a key collision of two ids is EV105") {
    pat::content::EventBuildOptions options;
    options.key_function = [](std::string_view) { return std::uint64_t{7}; };  // every name has the same key
    const Compiled c = compile(character_event({}, "option \"ok\" { }", "t.one") +
                                   character_event({}, "option \"ok\" { }", "t.two"),
                               options);
    REQUIRE_FALSE(c.ok());
    CHECK(std::ranges::count(c.codes(), ErrorCode::EventKeyCollision) >= 1);
    CHECK(c.all_errors().find("EV105") != std::string::npos);
}

TEST_CASE("event compiler: argument errors are EV103 and EV112") {
    const auto effect_error = [](std::string_view effect) {
        return compile(character_event("", std::format("option \"o\" {{ effect {{ {} }} }}", effect)));
    };
    SUBCASE("a missing, an extra and a mistyped argument") {
        const Compiled missing = effect_error("dynasty = { change_honor = { } }");
        REQUIRE(missing.errors.size() == 1);
        CHECK(missing.errors[0].diagnostic.code == ErrorCode::EventBadArgument);
        const Compiled extra = effect_error("dynasty = { change_honor = { value = 1  bonus = 2 } }");
        REQUIRE(extra.errors.size() == 1);
        CHECK(extra.errors[0].diagnostic.code == ErrorCode::EventUnknownEntry);
        CHECK(extra.errors[0].diagnostic.pos == where(extra, "effect", "bonus"));
        const Compiled bad = effect_error("dynasty = { change_honor = yes }");
        REQUIRE(bad.errors.size() == 1);
        CHECK(bad.errors[0].diagnostic.code == ErrorCode::EventBadArgument);
        const Compiled cause = effect_error("die = {cause = poison}");
        REQUIRE(cause.errors.size() == 1);
        CHECK(cause.errors[0].diagnostic.code == ErrorCode::EventBadArgument);
        CHECK(cause.errors[0].diagnostic.message.find("natural") != std::string::npos);
        CHECK(effect_error("die = { cause = murder }").ok());
        CHECK(effect_error("die = yes").ok());
    }
    SUBCASE("a trigger operator on an effect and a computed flag name") {
        const Compiled op = effect_error("dynasty = { change_honor >= 1 }");
        REQUIRE(op.errors.size() == 1);
        CHECK(op.errors[0].diagnostic.code == ErrorCode::EventBadArgument);
        const Compiled name = effect_error("set_flag = { name = scope:x }");
        REQUIRE_FALSE(name.errors.empty());
        CHECK(name.errors[0].diagnostic.code == ErrorCode::EventForbidden);
    }
    SUBCASE("a scope argument of the wrong kind") {
        const Compiled c = effect_error("change_opinion = { target = dynasty  value = 1 }");
        REQUIRE(c.errors.size() == 1);
        CHECK(c.errors[0].diagnostic.code == ErrorCode::EventScopeMismatch);
    }
}

TEST_CASE("event compiler: limits are EV106") {
    SUBCASE("iterators nested too deep") {
        const Compiled c = compile(character_event(
            "trigger { any_child = { any_child = { any_child = { any_child = { is_adult = yes } } } } }"));
        REQUIRE_FALSE(c.ok());
        CHECK(std::ranges::count(c.codes(), ErrorCode::EventLimit) >= 1);
    }
    SUBCASE("a stack that is too deep") {
        std::string expr = "1";
        for (int i = 0; i < 40; ++i) {
            expr = std::format("{{ base = 1  add = {} }}", expr);
        }
        const Compiled c =
            compile(character_event("", std::format("option \"o\" {{ ai_chance = {} }}", expr)));
        REQUIRE_FALSE(c.ok());
        CHECK(std::ranges::count(c.codes(), ErrorCode::EventLimit) >= 1);
    }
    SUBCASE("more saved scopes than the machine has slots") {
        std::string effects;
        for (int i = 0; i < 9; ++i) {
            effects += std::format("save_scope_as = s{} ", i);
        }
        // the same scope saved under 9 names
        const Compiled c =
            compile(character_event("", std::format("option \"o\" {{ effect {{ {} }} }}", effects)));
        REQUIRE_FALSE(c.ok());
        CHECK(std::ranges::count(c.codes(), ErrorCode::EventLimit) >= 1);
    }
}

TEST_CASE("event compiler: forbidden constructs are EV107") {
    const Compiled world = compile(character_event("trigger { any_character = { age > 1 } }"));
    REQUIRE(world.errors.size() == 1);
    CHECK(world.errors[0].diagnostic.code == ErrorCode::EventForbidden);
    CHECK(world.errors[0].diagnostic.pos == where(world, "trigger {", "any_character"));
    const Compiled tif = compile(character_event("trigger { trigger_if = { limit = { age > 1 } } }"));
    REQUIRE(tif.errors.size() == 1);
    CHECK(tif.errors[0].diagnostic.code == ErrorCode::EventForbidden);
    const Compiled floor =
        compile(character_event("", "option \"o\" { ai_chance = { base = 1.5  floor = yes } }"));
    REQUIRE(floor.errors.size() == 1);
    CHECK(floor.errors[0].diagnostic.code == ErrorCode::EventForbidden);
}

TEST_CASE("event compiler: on-action errors are EV108 and EV102") {
    const auto on = [](std::string_view line, std::string_view scope = "character") {
        return compile(std::format(R"(event "t.on" {{
    scope = {}
    type = personal
    category = general
    {}
    option "ok" {{ }}
}}
)",
                                   scope,
                                   line));
    };
    const Compiled unknown = on("on = on_deth");
    REQUIRE(unknown.errors.size() == 1);
    CHECK(unknown.errors[0].diagnostic.code == ErrorCode::EventBadOnAction);
    CHECK(unknown.errors[0].diagnostic.message.find("did you mean 'on_death'") != std::string::npos);
    CHECK(unknown.errors[0].diagnostic.pos == where(unknown, "on =", "on_deth"));
    const Compiled reserved = on("on = on_scheme_started");
    REQUIRE(reserved.errors.size() == 1);
    CHECK(reserved.errors[0].diagnostic.code == ErrorCode::EventBadOnAction);
    const Compiled role = on("on = on_marriage.groomm");
    REQUIRE(role.errors.size() == 1);
    CHECK(role.errors[0].diagnostic.code == ErrorCode::EventBadOnAction);
    CHECK(role.errors[0].diagnostic.message.find("did you mean 'groom'") != std::string::npos);
    const Compiled mismatch = on("on = on_death", "province");
    REQUIRE(mismatch.errors.size() == 1);
    CHECK(mismatch.errors[0].diagnostic.code == ErrorCode::EventScopeMismatch);
    const Compiled house = on("on = on_death.house", "dynasty");
    CHECK(house.ok());
    const Compiled daily = on("on = daily_pulse");
    REQUIRE(daily.errors.size() == 1);
    CHECK(daily.errors[0].diagnostic.code == ErrorCode::EventScopeMismatch);
    const Compiled pulse_role = on("on = monthly_pulse.child");
    REQUIRE(pulse_role.errors.size() == 1);
    CHECK(pulse_role.errors[0].diagnostic.code == ErrorCode::EventBadOnAction);
}

TEST_CASE("event compiler: options are checked, EV109") {
    const Compiled none = compile(character_event("", ""));
    REQUIRE(none.errors.size() == 1);
    CHECK(none.errors[0].diagnostic.code == ErrorCode::EventBadOptions);
    std::string five;
    for (int i = 0; i < 5; ++i) {
        five += std::format("option \"o{}\" {{ }} ", i);
    }
    const Compiled many = compile(character_event("", five));
    REQUIRE(many.errors.size() == 1);
    CHECK(many.errors[0].diagnostic.code == ErrorCode::EventBadOptions);
    const Compiled hidden = compile(R"(event "t.h" {
    scope = character
    type = hidden
    category = general
    on = monthly_pulse
    option "x" { }
}
)");
    REQUIRE(hidden.errors.size() == 1);
    CHECK(hidden.errors[0].diagnostic.code == ErrorCode::EventBadOptions);
    const Compiled twice =
        compile(character_event("", "option \"a\" { default = yes } option \"b\" { default = yes }"));
    REQUIRE(twice.errors.size() == 1);
    CHECK(twice.errors[0].diagnostic.code == ErrorCode::EventBadOptions);
    const Compiled single =
        compile(character_event("single_option = yes", "option \"a\" { } option \"b\" { }"));
    REQUIRE(single.errors.size() == 1);
    CHECK(single.errors[0].diagnostic.code == ErrorCode::EventBadOptions);
    CHECK(compile(character_event("single_option = yes", "option \"a\" { }")).ok());
    // a repeated option name is refused by the schema
    const Compiled dup = compile(character_event("", "option \"a\" { } option \"a\" { }"));
    REQUIRE(dup.errors.size() == 1);
    CHECK(dup.errors[0].diagnostic.code == ErrorCode::DuplicateField);
}

TEST_CASE("event compiler: field combinations are EV110") {
    const Compiled both = compile(character_event("mean_time_to_happen = { months = 6 }  chance = 10"));
    REQUIRE(both.errors.size() == 1);
    CHECK(both.errors[0].diagnostic.code == ErrorCode::EventBadField);
    const Compiled window = compile(character_event("window = { from = 1710.01.01  to = 1703.01.01 }"));
    REQUIRE(window.errors.size() == 1);
    CHECK(window.errors[0].diagnostic.code == ErrorCode::EventBadField);
    const Compiled pool = compile(character_event("pool = p"));
    REQUIRE(pool.errors.size() == 1);
    CHECK(pool.errors[0].diagnostic.code == ErrorCode::EventBadField);
    const Compiled once = compile(R"(event "t.once" {
    scope = character
    type = personal
    category = general
    once = scope
    option "ok" { }
}
)");
    REQUIRE(once.errors.size() == 1);
    CHECK(once.errors[0].diagnostic.code == ErrorCode::EventBadField);
    // a chain target may use once
    const Compiled chain = compile(R"(event "t.first" {
    scope = character
    type = personal
    category = general
    on = monthly_pulse
    option "ok" { effect { trigger_event = { id = t.second  months = 3 } } }
}
event "t.second" {
    scope = character
    type = personal
    category = general
    once = scope
    option "ok" { }
}
)");
    INFO(chain.all_errors());
    CHECK(chain.ok());
    const Compiled no_pulse = compile(R"(event "t.mtth" {
    scope = character
    type = personal
    category = general
    on = on_death
    mean_time_to_happen = { months = 6 }
    option "ok" { }
}
)");
    REQUIRE(no_pulse.errors.size() == 1);
    CHECK(no_pulse.errors[0].diagnostic.code == ErrorCode::EventBadField);
}

TEST_CASE("event compiler: unknown entries are EV112, bad constants are EV113, retired names EV114") {
    const Compiled stray = compile(character_event("", "option \"o\" { effect { else = { } } }"));
    REQUIRE(stray.errors.size() == 1);
    CHECK(stray.errors[0].diagnostic.code == ErrorCode::EventUnknownEntry);
    const Compiled step = compile(character_event("", "option \"o\" { ai_chance = { base = 1  plus = 2 } }"));
    REQUIRE(step.errors.size() == 1);
    CHECK(step.errors[0].diagnostic.code == ErrorCode::EventUnknownEntry);
    const Compiled zero =
        compile(character_event("", "option \"o\" { ai_chance = { base = 1  divide = 0 } }"));
    REQUIRE(zero.errors.size() == 1);
    CHECK(zero.errors[0].diagnostic.code == ErrorCode::EventBadConstant);
    CHECK(zero.errors[0].diagnostic.pos == where(zero, "divide", "0"));
    const Compiled retired = compile(character_event("trigger { dynasty = { prestige_old >= 1 } }"));
    REQUIRE(retired.errors.size() == 1);
    CHECK(retired.errors[0].diagnostic.code == ErrorCode::EventRetired);
    CHECK(retired.errors[0].diagnostic.message.find("use honor") != std::string::npos);
    const Compiled factor = compile(character_event(
        "mean_time_to_happen = { months = 6  modifier = { factor = 0  trigger = { age > 1 } } }"));
    REQUIRE(factor.errors.size() == 1);
    CHECK(factor.errors[0].diagnostic.code == ErrorCode::EventBadConstant);
}

TEST_CASE("event compiler: every error of a file is reported at once, sorted by position") {
    const Compiled c =
        compile(std::format("{}{}{}",
                            character_event("trigger { corruption > 0.3 }", "option \"ok\" { }", "t.one"),
                            character_event("trigger { is_adlt = yes }", "option \"ok\" { }", "t.two"),
                            character_event("", "option \"o\" { effect { change_honor = 1 } }", "t.three")));
    REQUIRE_FALSE(c.ok());
    REQUIRE(c.errors.size() == 3);
    const std::vector<ErrorCode> codes = c.codes();
    CHECK(codes[0] == ErrorCode::EventScopeMismatch);
    CHECK(codes[1] == ErrorCode::EventUnknownName);
    CHECK(codes[2] == ErrorCode::EventScopeMismatch);  // change_honor at a character
    for (std::size_t i = 1; i < c.errors.size(); ++i) {
        CHECK(c.errors[i - 1].diagnostic.pos <= c.errors[i].diagnostic.pos);
    }
}

// ---- hashes
// ---------------------------------------------------------------------------------------------------------

TEST_CASE("event compiler: the hash of the set follows the rules, not the texts") {
    const std::string base = character_event("trigger { age >= 18 }",
                                             "option \"a\" { effect { dynasty = { change_honor = 5 } } }");
    const Compiled a = compile(base);
    REQUIRE(a.ok());
    const std::uint64_t hash = a.set->hash();
    CHECK(hash != 0);
    CHECK(compile(base).set->hash() == hash);

    SUBCASE("a rule change moves the hash") {
        std::string edited = base;
        pat::court_fixture::replace_once(edited, "change_honor = 5", "change_honor = 6");
        CHECK(compile(edited).set->hash() != hash);
        edited = base;
        pat::court_fixture::replace_once(edited, "age >= 18", "age >= 19");
        CHECK(compile(edited).set->hash() != hash);
    }
    SUBCASE("texts, review, tags, category and notes do not") {
        std::string edited = base;
        pat::court_fixture::replace_once(edited, "category = general", "category = court");
        pat::court_fixture::replace_once(edited,
                                         "on = monthly_pulse",
                                         "on = monthly_pulse\n    review = reviewed\n    tags = [x]\n    "
                                         "notes = \"hi\"\n    title = \"evt.other.title\"");
        CHECK(compile(edited).set->hash() == hash);
    }
    SUBCASE("the order of the options and the on-action count") {
        std::string edited = base;
        pat::court_fixture::replace_once(edited, "on = monthly_pulse", "on = yearly_pulse");
        CHECK(compile(edited).set->hash() != hash);
    }
}

TEST_CASE("event compiler: events stay out of the simulation content hash") {
    const Compiled without = compile("");
    const Compiled with = compile(character_event());
    REQUIRE(without.ok());
    REQUIRE(with.ok());
    CHECK(without.content_hash == with.content_hash);
    // while the full content hash of the registry does move
    auto source_a = pat::court_fixture::shipped_source(false);
    auto source_b = pat::court_fixture::shipped_source(false);
    source_b.add("events/t.pd", "format_version = 1\n" + character_event());
    const auto a = pat::data::load_content(source_a, pat::data::content_schemas());
    const auto b = pat::data::load_content(source_b, pat::data::content_schemas());
    REQUIRE(a.ok());
    REQUIRE(b.ok());
    CHECK(a.registry.content_hash() != b.registry.content_hash());
    CHECK(a.registry.content_hash_excluding(pat::data::kEventKinds) ==
          b.registry.content_hash_excluding(pat::data::kEventKinds));
}

TEST_CASE("event compiler: settings read from event_settings, defaults otherwise") {
    const Compiled none = compile(character_event());
    CHECK(none.set->settings.modal_per_month == 6);
    CHECK(none.set->settings.frequency_choices == std::vector<std::int32_t>{500, 1000, 2000});
    const Compiled set = compile(R"(event_settings "main" {
    modal_per_month = 4
    frequency_choices = [500, 1000]
    frequency_applies_to = all
    rumor_delay_days = [3, 9]
    pulse_month_days = 31
    limits = { max_commands = 16 }
}
)" + character_event("mean_time_to_happen = { months = 2 }"));
    INFO(set.all_errors());
    REQUIRE(set.ok());
    CHECK(set.set->settings.modal_per_month == 4);
    CHECK(set.set->settings.frequency_choices == std::vector<std::int32_t>{500, 1000});
    CHECK(set.set->settings.frequency_applies_to_all);
    CHECK(set.set->settings.rumor_delay_min_days == 3);
    CHECK(set.set->settings.max_commands == 16);
    CHECK(set.set->settings.max_effect_instr == ev::limits::kMaxEffectInstr);
    // the month length of the settings is used for the intervals
    CHECK(set.set->events.front().mtth_days == 62);
    // the settings take part in the hash
    CHECK(set.set->hash() != compile(character_event("mean_time_to_happen = { months = 2 }")).set->hash());
    const Compiled wrong = compile("event_settings \"other\" { }\n");
    REQUIRE_FALSE(wrong.ok());
    CHECK(wrong.errors[0].diagnostic.code == ErrorCode::EventBadField);
}

// ---- the corpus of right and wrong files
// ----------------------------------------------------------------------------

namespace {

std::filesystem::path corpus_dir(std::string_view kind) {
    return std::filesystem::path(PATRIARCH_TEST_DATA_DIR) / "events" / "compiler" / kind;
}

// "# expect: EV102 7:9" lines of a bad file.
std::vector<std::string> expected_errors(const std::string& text) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t end = text.find('\n', start);
        const std::string line =
            text.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (line.starts_with("# expect: ")) {
            std::string value = line.substr(10);
            while (!value.empty() && (value.back() == '\r' || value.back() == ' ')) {
                value.pop_back();
            }
            out.push_back(value);
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    std::ranges::sort(out);
    return out;
}

}  // namespace

TEST_CASE("event compiler: the corpus of right files compiles") {
    int files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(corpus_dir("good"))) {
        if (entry.path().extension() != ".pd") {
            continue;
        }
        ++files;
        const std::string text = pat::test::read_text(entry.path());
        auto source = pat::court_fixture::shipped_source(false);
        source.add("events/c.pd", text);
        const auto loaded = pat::data::load_content(source, pat::data::content_schemas());
        INFO(entry.path().filename().string());
        REQUIRE(loaded.ok());
        const auto set = pat::content::build_event_set(loaded.registry, catalog());
        const std::string failure = set ? std::string() : pat::content::format_errors(set.error());
        INFO(failure);
        CHECK(set.has_value());
    }
    CHECK(files >= 2);
}

TEST_CASE("event compiler: every wrong file reports exactly the errors it announces") {
    int files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(corpus_dir("bad"))) {
        if (entry.path().extension() != ".pd") {
            continue;
        }
        ++files;
        const std::string text = pat::test::read_text(entry.path());
        auto source = pat::court_fixture::shipped_source(false);
        source.add("events/c.pd", text);
        const auto loaded = pat::data::load_content(source, pat::data::content_schemas());
        INFO(entry.path().filename().string());
        REQUIRE(loaded.ok());
        const auto set = pat::content::build_event_set(loaded.registry, catalog());
        REQUIRE_FALSE(set.has_value());
        std::vector<std::string> actual;
        for (const auto& error : set.error()) {
            actual.push_back(std::format("{} {}:{}",
                                         pat::data::error_code_name(error.diagnostic.code),
                                         error.diagnostic.pos.line,
                                         error.diagnostic.pos.column));
        }
        std::ranges::sort(actual);
        const std::vector<std::string> expected = expected_errors(text);
        CHECK_MESSAGE(!expected.empty(), "a bad file lists its errors with '# expect: EVnnn line:column'");
        CHECK(actual == expected);
    }
    CHECK(files >= 4);
}

// ---- the size of a realistic load
// ----------------------------------------------------------------------------------

namespace {

std::string synthetic_events(int count) {
    std::string text = "format_version = 1\n";
    for (int i = 0; i < count; ++i) {
        text += std::format(R"(event "syn.e{}" {{
    scope = province
    type = provincial
    category = economy
    on = monthly_pulse
    mean_time_to_happen = {{ months = {}  modifier = {{ factor = 0.5  trigger = {{ corruption >= 0.5 }} }} }}
    cooldown = {{ months = 12 }}
    trigger {{ exists = administrator  corruption > 0.3  owner = {{ honor >= {} }} }}
    immediate {{ administrator = {{ save_scope_as = governor }} }}
    option "a" {{
        ai_chance = {{ base = 20  add = {{ value = owner.honor  divide = 5 }} if = {{ limit = {{ corruption > 0.5 }}  add = 10 }} }}
        trigger {{ owner = {{ honor >= 30 }} }}
        effect {{
            owner = {{ change_honor = -5 }}
            change_control = 5
            scope.governor = {{ change_opinion = {{ target = root.owner.acting_head  value = -20 }} }}
            random_list = {{ 30 = {{ change_control = 1 }} 70 = {{ change_control = 2 }} }}
        }}
    }}
    option "b" {{ ai_chance = 40  effect {{ owner = {{ pay = 200 }}  set_flag = {{ name = f{}  days = 365 }} }} }}
}}
)",
                            i,
                            12 + i % 36,
                            i % 100,
                            i % 50);
    }
    return text;
}

}  // namespace

TEST_CASE("event compiler: 550 events compile") {
    const std::string text = synthetic_events(550);
    auto source = pat::court_fixture::shipped_source(false);
    source.add("events/synthetic.pd", text);
    const auto loaded = pat::data::load_content(source, pat::data::content_schemas());
    REQUIRE(loaded.ok());
    const auto set = pat::content::build_event_set(loaded.registry, catalog());
    const std::string failure = set ? std::string() : pat::content::format_errors(set.error());
    INFO(failure);
    REQUIRE(set.has_value());
    const ev::EventSet& events = set.value();
    CHECK(events.events.size() == 550);
    // ascending by key
    for (std::size_t i = 1; i < events.events.size(); ++i) {
        CHECK(events.events[i - 1].key < events.events[i].key);
    }
}

TEST_CASE("event compiler: load time of 550 events" * doctest::skip()) {
    // Run with --no-skip; the target is <= 500 ms for parsing, validation and compilation together.
    const std::string text = synthetic_events(550);
    auto source = pat::court_fixture::shipped_source(false);
    source.add("events/synthetic.pd", text);
    const auto t0 = std::chrono::steady_clock::now();
    const auto loaded = pat::data::load_content(source, pat::data::content_schemas());
    const auto t1 = std::chrono::steady_clock::now();
    REQUIRE(loaded.ok());
    const auto set = pat::content::build_event_set(loaded.registry, catalog());
    const auto t2 = std::chrono::steady_clock::now();
    REQUIRE(set.has_value());
    const auto load_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    const auto compile_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();
    MESSAGE("load " << load_ms << " ms, compile " << compile_ms << " ms");
    CHECK(load_ms + compile_ms <= 500);
}
