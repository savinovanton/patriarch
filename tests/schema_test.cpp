#include "pat/data/example_schemas.hpp"
#include "pat/data/pd_parser.hpp"
#include "pat/data/schema.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <vector>

using namespace pat::data;
using pat::core::Fixed32;
using pat::core::Fixed64;

namespace {

struct Run {
    FileResult result;
    std::vector<std::string> lines;  // formatted diagnostics
};

/// Parses `format_version = 1` + body (so body line N is file line N + 1) and validates it.
Run run(const SchemaSet& schemas, const std::string& body) {
    const ParseResult parsed = parse_pd("format_version = 1\n" + body);
    REQUIRE_MESSAGE(parsed.ok(), "test source does not parse");
    Run r;
    r.result = validate_file(schemas, parsed.file);
    for (const Diagnostic& d : r.result.errors)
        r.lines.push_back(format_diagnostic("t.pd", d));
    return r;
}

/// A schema with one field of the given type: `thing "t" { f = ... }`.
SchemaSet single(Type type, bool required = true) {
    ObjectSchema thing("thing");
    if (required)
        thing.required("f", std::move(type));
    else
        thing.optional("f", std::move(type));
    SchemaSet set;
    set.add(std::move(thing));
    return set;
}

const TypedValue& field(const Run& r, const char* name) {
    REQUIRE(r.result.objects.size() == 1);
    const TypedValue* v = r.result.objects[0].body.find(name);
    REQUIRE(v != nullptr);
    return *v;
}

}  // namespace

TEST_CASE("schema: the example trait validates and yields typed values") {
    const SchemaSet schemas = make_example_schema_set();
    const Run r = run(schemas, R"(
trait "greedy" {
    name_key = "trait.greedy.name"
    cost = 3
    group = vice
    opposite = trait:honest
    weight = 1.5
    inheritable = yes
    since = 1703.01.01
    tags = [ "court", "money" ]
    skills = { stewardship = 2, intrigue = -1 }
    note = "first"
    note = "second"
    chance { base = 0.25 }
}
)");
    CHECK(r.lines.empty());
    REQUIRE(r.result.objects.size() == 1);
    const TypedObject& o = r.result.objects[0];
    CHECK(o.kind == "trait");
    CHECK(o.id == "greedy");
    CHECK(o.pos == SourcePos{3, 1});

    CHECK(*field(r, "name_key").get_if<std::string>() == "trait.greedy.name");
    CHECK(*field(r, "cost").get_if<std::int64_t>() == 3);
    const EnumValue* group = field(r, "group").get_if<EnumValue>();
    REQUIRE(group != nullptr);
    CHECK(group->name == "vice");
    CHECK(group->index == 0);
    const RefValue* opposite = field(r, "opposite").get_if<RefValue>();
    REQUIRE(opposite != nullptr);
    CHECK(opposite->kind == "trait");
    CHECK(opposite->id == "honest");
    CHECK(field(r, "opposite").pos == SourcePos{7, 16});
    CHECK(field(r, "weight").get_if<Fixed32>()->raw() == Fixed32::from_ratio(3, 2).raw());
    CHECK(*field(r, "inheritable").get_if<bool>());
    CHECK(*field(r, "since").get_if<DateValue>() == DateValue{1703, 1, 1});
    const TypedList* tags = field(r, "tags").get_if<TypedList>();
    REQUIRE(tags != nullptr);
    REQUIRE(tags->size() == 2);
    CHECK(*(*tags)[1].get_if<std::string>() == "money");
    const TypedMap* skills = field(r, "skills").get_if<TypedMap>();
    REQUIRE(skills != nullptr);
    REQUIRE(skills->entries.size() == 2);
    CHECK(skills->entries[0].name == "stewardship");
    CHECK(*skills->find("intrigue")->get_if<std::int64_t>() == -1);
    CHECK(o.body.find_all("note").size() == 2);
    const TypedBlock* chance = field(r, "chance").get_if<TypedBlock>();
    REQUIRE(chance != nullptr);
    CHECK(chance->find("base")->get_if<Fixed64>()->raw() == Fixed64::from_ratio(1, 4).raw());
    // The default of `max` is filled in.
    REQUIRE(chance->find("max") != nullptr);
    CHECK(chance->fields[1].defaulted);
    CHECK(*chance->find("max")->get_if<Fixed64>() == Fixed64::one());
}

TEST_CASE("schema: defaults and absent optional fields") {
    const Run r = run(make_example_schema_set(), "trait greedy { name_key = \"n\" cost = 0 }");
    CHECK(r.lines.empty());
    const TypedBlock& body = r.result.objects[0].body;
    REQUIRE(body.find("group") != nullptr);
    CHECK(body.find("group")->get_if<EnumValue>()->name == "neutral");
    CHECK(*body.find("weight")->get_if<Fixed32>() == Fixed32::one());
    CHECK(*body.find("inheritable")->get_if<bool>() == false);
    CHECK(body.find("opposite") == nullptr);
    CHECK(body.find("since") == nullptr);
    CHECK(body.find_all("note").empty());
    for (const TypedField& f : body.fields) {
        if (f.name == "group" || f.name == "weight" || f.name == "inheritable")
            CHECK(f.defaulted);
        else
            CHECK_FALSE(f.defaulted);
    }
}

TEST_CASE("schema: the documented error message format") {
    const Run r = run(make_example_schema_set(),
                      "trait \"greedy\" {\n"
                      "    name_key = \"x\"\n"
                      "    cost = 3\n"
                      "    skils = { a = 1 }\n"
                      "}\n");
    REQUIRE(r.lines.size() == 1);
    CHECK(r.lines[0] == "t.pd:5:5: error[PD101]: unknown field 'skils' (did you mean 'skills'?)");
    CHECK(r.result.objects.empty());
}

TEST_CASE("schema: unknown field without a close match lists the valid fields") {
    const Run r = run(single(Type::integer()), "thing t { f = 1 qqqqqqq = 2 }");
    REQUIRE(r.lines.size() == 1);
    CHECK(r.lines[0] == "t.pd:2:17: error[PD101]: unknown field 'qqqqqqq' (valid fields: f)");
}

TEST_CASE("schema: did-you-mean handles transpositions, case and distance limits") {
    const std::vector<std::string> names = {"cost", "skills", "group"};
    CHECK(suggest_similar("cots", names) == "cost");
    CHECK(suggest_similar("Skills", names) == "skills");
    CHECK(suggest_similar("skils", names) == "skills");
    CHECK(suggest_similar("grup", names) == "group");
    CHECK(suggest_similar("zzzzzz", names).empty());
    CHECK(suggest_similar("x", names).empty());
    // Ties go to the earlier candidate.
    CHECK(suggest_similar("abc", {"abd", "abe"}) == "abd");
    CHECK(suggest_similar("c", {"a", "b"}).empty());
}

TEST_CASE("schema: missing required field") {
    const Run r = run(make_example_schema_set(), "trait greedy {\n    cost = 3\n}\n");
    REQUIRE(r.lines.size() == 1);
    CHECK(r.lines[0] == "t.pd:2:1: error[PD102]: missing required field 'name_key' in trait \"greedy\"");
}

TEST_CASE("schema: missing required field in a nested block points at the block") {
    const Run r = run(make_example_schema_set(),
                      "trait greedy { name_key = \"n\" cost = 1\n"
                      "    chance { max = 0.5 }\n"
                      "}\n");
    REQUIRE(r.lines.size() == 1);
    CHECK(r.lines[0] == "t.pd:3:12: error[PD102]: missing required field 'base' in block 'chance'");
}

TEST_CASE("schema: string type") {
    const SchemaSet s = single(Type::string());
    const Run ok = run(s, "thing t { f = \"Москва\" }");
    CHECK(ok.lines.empty());
    CHECK(*field(ok, "f").get_if<std::string>() == "Москва");
    const Run bad = run(s, "thing t { f = 5 }");
    REQUIRE(bad.lines.size() == 1);
    CHECK(bad.lines[0] == "t.pd:2:15: error[PD103]: expected string, found int");
    const Run ident = run(s, "thing t { f = moscow }");
    CHECK(ident.lines[0] == "t.pd:2:15: error[PD103]: expected string, found identifier");
}

TEST_CASE("schema: int type with a range") {
    const SchemaSet s = single(Type::integer(0, 10));
    CHECK(*field(run(s, "thing t { f = 10 }"), "f").get_if<std::int64_t>() == 10);
    CHECK(*field(run(s, "thing t { f = 0 }"), "f").get_if<std::int64_t>() == 0);
    const Run high = run(s, "thing t { f = 11 }");
    REQUIRE(high.lines.size() == 1);
    CHECK(high.lines[0] == "t.pd:2:15: error[PD104]: value 11 is out of range (0..10)");
    const Run low = run(s, "thing t { f = -1 }");
    CHECK(low.lines[0] == "t.pd:2:15: error[PD104]: value -1 is out of range (0..10)");
    const Run dec = run(s, "thing t { f = 1.5 }");
    CHECK(dec.lines[0] == "t.pd:2:15: error[PD103]: expected int, found decimal");

    const Run only_min = run(single(Type::integer(5, std::nullopt)), "thing t { f = 4 }");
    CHECK(only_min.lines[0] == "t.pd:2:15: error[PD104]: value 4 is out of range (at least 5)");
    const Run only_max = run(single(Type::integer(std::nullopt, 5)), "thing t { f = 6 }");
    CHECK(only_max.lines[0] == "t.pd:2:15: error[PD104]: value 6 is out of range (at most 5)");
    CHECK(run(single(Type::integer()), "thing t { f = -123456789012345678 }").lines.empty());
}

TEST_CASE("schema: fixed32 is exact and range checked") {
    const SchemaSet s = single(Type::fixed32());
    // 0.1 = 6553.6 raw units, truncated toward zero.
    CHECK(field(run(s, "thing t { f = 0.1 }"), "f").get_if<Fixed32>()->raw() == 6553);
    CHECK(field(run(s, "thing t { f = -0.1 }"), "f").get_if<Fixed32>()->raw() == -6553);
    CHECK(field(run(s, "thing t { f = 0.1 }"), "f").get_if<Fixed32>()->raw() ==
          Fixed32::from_ratio(1, 10).raw());
    CHECK(field(run(s, "thing t { f = 1.5 }"), "f").get_if<Fixed32>()->raw() == 98304);
    CHECK(field(run(s, "thing t { f = 3 }"), "f").get_if<Fixed32>()->raw() == 3 * 65536);  // ints are fine
    CHECK(field(run(s, "thing t { f = 0.000000001 }"), "f").get_if<Fixed32>()->raw() == 0);
    CHECK(field(run(s, "thing t { f = -32768.0 }"), "f").get_if<Fixed32>()->raw() == INT32_MIN);
    CHECK(field(run(s, "thing t { f = 32767.999984742 }"), "f").get_if<Fixed32>()->raw() == INT32_MAX);

    const Run big = run(s, "thing t { f = 32768.0 }");
    REQUIRE(big.lines.size() == 1);
    CHECK(big.lines[0] ==
          "t.pd:2:15: error[PD104]: value 32768.0 is out of range for fixed32 (-32768.0000 .. 32767.9999)");
    CHECK(run(s, "thing t { f = -32768.1 }").lines[0].find("error[PD104]") != std::string::npos);
    CHECK(run(s, "thing t { f = 40000 }").lines[0].find("error[PD104]") != std::string::npos);
    CHECK(run(s, "thing t { f = \"x\" }").lines[0] ==
          "t.pd:2:15: error[PD103]: expected number, found string");
}

TEST_CASE("schema: fixed range bounds") {
    const SchemaSet s = single(Type::fixed32(Fixed64::zero(), Fixed64::from_ratio(1, 2)));
    CHECK(run(s, "thing t { f = 0.5 }").lines.empty());
    CHECK(run(s, "thing t { f = 0 }").lines.empty());
    const Run r = run(s, "thing t { f = 0.51 }");
    REQUIRE(r.lines.size() == 1);
    CHECK(r.lines[0] == "t.pd:2:15: error[PD104]: value 0.51 is out of range (0.0000..0.5000)");
    CHECK(run(s, "thing t { f = -0.01 }").lines.size() == 1);
}

TEST_CASE("schema: fixed64 is exact and range checked") {
    const SchemaSet s = single(Type::fixed64());
    CHECK(field(run(s, "thing t { f = 0.000000001 }"), "f").get_if<Fixed64>()->raw() == 4);
    CHECK(field(run(s, "thing t { f = 0.5 }"), "f").get_if<Fixed64>()->raw() ==
          Fixed64::from_ratio(1, 2).raw());
    CHECK(field(run(s, "thing t { f = 2147483647 }"), "f").get_if<Fixed64>()->raw() ==
          (std::int64_t{2147483647} << 32));
    CHECK(field(run(s, "thing t { f = -2147483648 }"), "f").get_if<Fixed64>()->raw() == INT64_MIN);
    const Run big = run(s, "thing t { f = 2147483648.0 }");
    REQUIRE(big.lines.size() == 1);
    CHECK(big.lines[0].find("error[PD104]: value 2147483648.0 is out of range for fixed64") !=
          std::string::npos);
    CHECK(run(s, "thing t { f = 999999999999999999 }").lines.size() == 1);
}

TEST_CASE("schema: decimal_to_fixed matches Fixed::from_ratio without floating point") {
    for (std::int64_t mantissa = -3000; mantissa <= 3000; mantissa += 7) {
        std::int64_t divisor = 1;
        for (unsigned scale = 0; scale <= 4; ++scale, divisor *= 10) {
            Fixed32 a;
            REQUIRE(decimal_to_fixed(mantissa, scale, a));
            CHECK(a.raw() == Fixed32::from_ratio(mantissa, divisor).raw());
            Fixed64 b;
            REQUIRE(decimal_to_fixed(mantissa, scale, b));
            CHECK(b.raw() == Fixed64::from_ratio(mantissa, divisor).raw());
        }
    }
    // The mantissa of 3.141592653 does not fit a 32-bit from_ratio argument; the conversion still works.
    Fixed32 pi;
    REQUIRE(decimal_to_fixed(3141592653, 9, pi));
    CHECK(pi.raw() == 205887);  // floor(3.141592653 * 65536) = 205887
    Fixed32 unused;
    CHECK_FALSE(decimal_to_fixed(1, 19, unused));
}

TEST_CASE("schema: bool type") {
    const SchemaSet s = single(Type::boolean());
    CHECK(*field(run(s, "thing t { f = yes }"), "f").get_if<bool>());
    CHECK_FALSE(*field(run(s, "thing t { f = no }"), "f").get_if<bool>());
    const Run bad = run(s, "thing t { f = 1 }");
    CHECK(bad.lines[0] == "t.pd:2:15: error[PD103]: expected bool (yes/no), found int");
}

TEST_CASE("schema: enum type") {
    const SchemaSet s = single(Type::enumeration({"vice", "virtue", "neutral"}));
    const Run ok = run(s, "thing t { f = virtue }");
    CHECK(ok.lines.empty());
    CHECK(field(ok, "f").get_if<EnumValue>()->index == 1);

    const Run unknown = run(s, "thing t { f = vise }");
    REQUIRE(unknown.lines.size() == 1);
    CHECK(unknown.lines[0] == "t.pd:2:15: error[PD106]: unknown value 'vise' (allowed: vice, virtue, "
                              "neutral) (did you mean 'vice'?)");
    const Run far = run(s, "thing t { f = banana }");
    CHECK(far.lines[0] == "t.pd:2:15: error[PD106]: unknown value 'banana' (allowed: vice, virtue, neutral)");
    const Run quoted = run(s, "thing t { f = \"vice\" }");
    CHECK(quoted.lines[0] == "t.pd:2:15: error[PD103]: expected identifier (enum), found string");
}

TEST_CASE("schema: reference type records kind, id and position") {
    const SchemaSet s = single(Type::reference("trait"));
    const Run a = run(s, "thing t { f = trait:honest }");
    CHECK(a.lines.empty());
    CHECK(*field(a, "f").get_if<RefValue>() == RefValue{"trait", "honest"});
    CHECK(field(a, "f").pos == SourcePos{2, 15});
    // A bare identifier or a string is a reference to the expected kind as well.
    CHECK(*field(run(s, "thing t { f = honest }"), "f").get_if<RefValue>() == RefValue{"trait", "honest"});
    CHECK(*field(run(s, "thing t { f = \"honest\" }"), "f").get_if<RefValue>() ==
          RefValue{"trait", "honest"});
    const Run wrong_kind = run(s, "thing t { f = skill:war }");
    CHECK(wrong_kind.lines[0] ==
          "t.pd:2:15: error[PD103]: expected reference to 'trait', found reference to 'skill'");
    CHECK(run(s, "thing t { f = 5 }").lines[0] ==
          "t.pd:2:15: error[PD103]: expected reference to 'trait', found int");
}

TEST_CASE("schema: date type uses the Julian calendar") {
    const SchemaSet s = single(Type::date());
    CHECK(*field(run(s, "thing t { f = 1704.02.29 }"), "f").get_if<DateValue>() == DateValue{1704, 2, 29});
    CHECK(*field(run(s, "thing t { f = 1700.02.29 }"), "f").get_if<DateValue>() == DateValue{1700, 2, 29});
    CHECK(*field(run(s, "thing t { f = 1703.12.31 }"), "f").get_if<DateValue>() == DateValue{1703, 12, 31});

    const Run feb = run(s, "thing t { f = 1703.02.29 }");
    REQUIRE(feb.lines.size() == 1);
    CHECK(feb.lines[0] == "t.pd:2:15: error[PD107]: 1703.02.29 does not exist in the Julian calendar");
    const Run apr = run(s, "thing t { f = 1703.04.31 }");
    CHECK(apr.lines[0] == "t.pd:2:15: error[PD107]: 1703.04.31 does not exist in the Julian calendar");
    CHECK(run(s, "thing t { f = 1703.02.30 }").lines.size() == 1);
    CHECK(run(s, "thing t { f = \"1703.01.01\" }").lines[0] ==
          "t.pd:2:15: error[PD103]: expected date, found string");

    CHECK(is_valid_julian_date(1704, 2, 29));
    CHECK_FALSE(is_valid_julian_date(1703, 2, 29));
    CHECK(is_valid_julian_date(1900, 2, 29));  // Julian: every fourth year, no century exception
    CHECK_FALSE(is_valid_julian_date(1703, 13, 1));
    CHECK_FALSE(is_valid_julian_date(1703, 6, 31));
    CHECK_FALSE(is_valid_julian_date(1703, 6, 0));
}

TEST_CASE("schema: list type validates every element") {
    const SchemaSet s = single(Type::list(Type::integer(0, 5)));
    const Run ok = run(s, "thing t { f = [ 1, 2, 3 ] }");
    CHECK(ok.lines.empty());
    CHECK(field(ok, "f").get_if<TypedList>()->size() == 3);
    CHECK(run(s, "thing t { f = [] }").lines.empty());

    const Run bad = run(s, "thing t { f = [ 1, 9, \"x\" ] }");
    REQUIRE(bad.lines.size() == 2);
    CHECK(bad.lines[0] == "t.pd:2:20: error[PD104]: value 9 is out of range (0..5)");
    CHECK(bad.lines[1] == "t.pd:2:23: error[PD103]: expected int, found string");
    CHECK(run(s, "thing t { f = 5 }").lines[0] == "t.pd:2:15: error[PD103]: expected list of int, found int");
}

TEST_CASE("schema: nested block type") {
    ObjectSchema inner("inner");
    inner.required("a", Type::integer()).optional("b", Type::boolean());
    const SchemaSet s = single(Type::block(inner));
    const Run ok = run(s, "thing t { f = { a = 1 b = yes } }");
    CHECK(ok.lines.empty());
    CHECK(*field(ok, "f").get_if<TypedBlock>()->find("a")->get_if<std::int64_t>() == 1);
    CHECK(run(s, "thing t { f { a = 1 } }").lines.empty());

    const Run bad = run(s, "thing t { f = { a = 1 c = 2 } }");
    REQUIRE(bad.lines.size() == 1);
    CHECK(bad.lines[0] == "t.pd:2:23: error[PD101]: unknown field 'c' (valid fields: a, b)");
    CHECK(run(s, "thing t { f = 3 }").lines[0] == "t.pd:2:15: error[PD103]: expected block, found int");
    // Errors inside nested blocks keep their own positions.
    const Run deep = run(s, "thing t {\n  f = {\n    a = \"x\"\n  }\n}");
    REQUIRE(deep.lines.size() == 1);
    CHECK(deep.lines[0] == "t.pd:4:9: error[PD103]: expected int, found string");
}

TEST_CASE("schema: map type") {
    const SchemaSet s = single(Type::map(Type::integer(-5, 5)));
    const Run ok = run(s, "thing t { f = { a = 1 b = -2 } }");
    CHECK(ok.lines.empty());
    const TypedMap* map = field(ok, "f").get_if<TypedMap>();
    REQUIRE(map != nullptr);
    CHECK(map->entries.size() == 2);
    CHECK(*map->find("b")->get_if<std::int64_t>() == -2);
    CHECK(map->find("c") == nullptr);
    CHECK(run(s, "thing t { f = { } }").lines.empty());

    const Run dup = run(s, "thing t { f = { a = 1 a = 2 } }");
    REQUIRE(dup.lines.size() == 1);
    CHECK(dup.lines[0] == "t.pd:2:23: error[PD105]: duplicate key 'a' (first defined at 2:17)");
    const Run range = run(s, "thing t { f = { a = 9 } }");
    CHECK(range.lines[0] == "t.pd:2:21: error[PD104]: value 9 is out of range (-5..5)");
    const Run intkey = run(s, "thing t { f = { 7 = 1 } }");
    CHECK(intkey.lines[0] == "t.pd:2:17: error[PD103]: map keys must be identifiers, found '7'");
}

TEST_CASE("schema: required, optional and default fields") {
    ObjectSchema thing("thing");
    thing.required("r", Type::integer())
        .optional("o", Type::integer())
        .optional("d", Type::integer(), TypedValue::of(std::int64_t{42}));
    SchemaSet s;
    s.add(thing);

    const Run minimal = run(s, "thing t { r = 1 }");
    CHECK(minimal.lines.empty());
    const TypedBlock& body = minimal.result.objects[0].body;
    CHECK(body.find("o") == nullptr);
    CHECK(*body.find("d")->get_if<std::int64_t>() == 42);
    // The default is attributed to the object, not to the source of an absent key.
    CHECK(body.find("d")->pos == SourcePos{2, 1});

    const Run overridden = run(s, "thing t { r = 1 d = 7 }");
    CHECK(*overridden.result.objects[0].body.find("d")->get_if<std::int64_t>() == 7);

    const Run missing = run(s, "thing t { }");
    REQUIRE(missing.lines.size() == 1);
    CHECK(missing.lines[0] == "t.pd:2:1: error[PD102]: missing required field 'r' in thing \"t\"");
}

TEST_CASE("schema: duplicate non-repeatable key and repeatable keys") {
    ObjectSchema thing("thing");
    thing.required("once", Type::integer()).repeatable("many", Type::string());
    SchemaSet s;
    s.add(thing);

    const Run dup = run(s, "thing t {\n  once = 1\n  once = 2\n}");
    REQUIRE(dup.lines.size() == 1);
    CHECK(dup.lines[0] == "t.pd:4:3: error[PD105]: duplicate field 'once' (first defined at 3:3)");

    const Run many = run(s, "thing t { once = 1 many = \"a\" many = \"b\" many = \"c\" }");
    CHECK(many.lines.empty());
    const auto all = many.result.objects[0].body.find_all("many");
    REQUIRE(all.size() == 3);
    CHECK(*all[0]->get_if<std::string>() == "a");
    CHECK(*all[2]->get_if<std::string>() == "c");
}

TEST_CASE("schema: operators and labels are rejected where they have no meaning") {
    const SchemaSet s = single(Type::integer());
    const Run op = run(s, "thing t { f >= 3 }");
    REQUIRE(op.lines.size() == 1);
    CHECK(op.lines[0] == "t.pd:2:11: error[PD110]: expected '=' after field 'f', found '>='");
    const Run label = run(s, "thing t { f \"x\" { } }");
    REQUIRE(label.lines.size() >= 1);
    CHECK(label.lines[0].find("error[PD109]: field 'f' does not take a label") != std::string::npos);
}

TEST_CASE("schema: top-level entries need a known kind, an id and a block") {
    const SchemaSet s = make_example_schema_set();
    const Run kind = run(s, "trat greedy { }");
    REQUIRE(kind.lines.size() == 1);
    CHECK(kind.lines[0] == "t.pd:2:1: error[PD108]: unknown object kind 'trat' (did you mean 'trait'?)");
    const Run nokind = run(s, "zzzzzzzz greedy { }");
    CHECK(nokind.lines[0] == "t.pd:2:1: error[PD108]: unknown object kind 'zzzzzzzz' (known kinds: trait)");

    const Run noid = run(s, "trait { name_key = \"n\" cost = 1 }");
    REQUIRE(noid.lines.size() == 1);
    CHECK(noid.lines[0] == "t.pd:2:1: error[PD109]: 'trait' needs an id: trait \"name\" { ... }");

    const Run notblock = run(s, "trait = 5");
    REQUIRE(notblock.lines.size() == 2);
    CHECK(notblock.lines[0] == "t.pd:2:1: error[PD109]: 'trait' needs an id: trait \"name\" { ... }");
    CHECK(notblock.lines[1] == "t.pd:2:9: error[PD103]: expected block, found int");
}

TEST_CASE("schema: all errors of an object are reported at once, sorted by position") {
    const Run r = run(make_example_schema_set(),
                      "trait \"bad\" {\n"
                      "    cost = 11\n"
                      "    group = vise\n"
                      "    skils = { a = 1 }\n"
                      "    weight = 200\n"
                      "    since = 1703.02.29\n"
                      "    cost = 2\n"
                      "    inheritable = maybe\n"
                      "}\n");
    const std::vector<std::string> expected = {
        "t.pd:2:1: error[PD102]: missing required field 'name_key' in trait \"bad\"",
        "t.pd:3:12: error[PD104]: value 11 is out of range (0..10)",
        "t.pd:4:13: error[PD106]: unknown value 'vise' (allowed: vice, virtue, neutral) (did you mean "
        "'vice'?)",
        "t.pd:5:5: error[PD101]: unknown field 'skils' (did you mean 'skills'?)",
        "t.pd:6:14: error[PD104]: value 200 is out of range (0.0000..100.0000)",
        "t.pd:7:13: error[PD107]: 1703.02.29 does not exist in the Julian calendar",
        "t.pd:8:5: error[PD105]: duplicate field 'cost' (first defined at 3:5)",
        "t.pd:9:19: error[PD103]: expected bool (yes/no), found identifier",
    };
    CHECK(r.lines == expected);
    CHECK(r.result.objects.empty());
}

TEST_CASE("schema: validate_object keeps the valid part of a broken object") {
    const ParseResult parsed =
        parse_pd("format_version = 1\ntrait t { name_key = \"n\" cost = 99 group = vice }\n");
    REQUIRE(parsed.ok());
    const ObjectSchema schema = make_example_trait_schema();
    const ObjectResult r = validate_object(schema, parsed.file.entries[1]);
    CHECK_FALSE(r.ok());
    REQUIRE(r.errors.size() == 1);
    CHECK(r.errors[0].code == ErrorCode::ValueOutOfRange);
    CHECK(r.object.id == "t");
    CHECK(r.object.body.find("name_key") != nullptr);
    CHECK(r.object.body.find("cost") == nullptr);
}

TEST_CASE("schema: a file with several objects keeps the good ones") {
    const Run r = run(make_example_schema_set(),
                      "trait good { name_key = \"a\" cost = 1 }\n"
                      "trait bad { name_key = \"b\" cost = 50 }\n"
                      "trait also_good { name_key = \"c\" cost = 2 }\n");
    REQUIRE(r.result.objects.size() == 2);
    CHECK(r.result.objects[0].id == "good");
    CHECK(r.result.objects[1].id == "also_good");
    REQUIRE(r.lines.size() == 1);
    CHECK(r.lines[0].starts_with("t.pd:3:"));
}

TEST_CASE("schema: error codes have stable names") {
    CHECK(error_code_name(ErrorCode::UnknownField) == "PD101");
    CHECK(error_code_name(ErrorCode::MissingField) == "PD102");
    CHECK(error_code_name(ErrorCode::WrongType) == "PD103");
    CHECK(error_code_name(ErrorCode::ValueOutOfRange) == "PD104");
    CHECK(error_code_name(ErrorCode::DuplicateField) == "PD105");
    CHECK(error_code_name(ErrorCode::UnknownEnumValue) == "PD106");
    CHECK(error_code_name(ErrorCode::InvalidDate) == "PD107");
    CHECK(error_code_name(ErrorCode::UnknownKind) == "PD108");
    CHECK(error_code_name(ErrorCode::BadLabel) == "PD109");
    CHECK(error_code_name(ErrorCode::BadOperator) == "PD110");
}

TEST_CASE("schema: validation is deterministic") {
    const std::string body =
        "trait a { name_key = \"a\" cost = 99 skils = { } weight = 0.1 }\n"
        "trait b { name_key = \"b\" cost = 3 skills = { x = 1 y = 2 } chance { base = 0.3 } }\n";
    const SchemaSet schemas = make_example_schema_set();
    const Run first = run(schemas, body);
    const Run second = run(schemas, body);
    CHECK(first.lines == second.lines);
    REQUIRE(first.result.objects.size() == second.result.objects.size());
    for (std::size_t i = 0; i < first.result.objects.size(); ++i) {
        const TypedBlock& a = first.result.objects[i].body;
        const TypedBlock& b = second.result.objects[i].body;
        REQUIRE(a.fields.size() == b.fields.size());
        for (std::size_t j = 0; j < a.fields.size(); ++j) {
            CHECK(a.fields[j].name == b.fields[j].name);
            CHECK(a.fields[j].pos == b.fields[j].pos);
            CHECK(a.fields[j].value.data.index() == b.fields[j].value.data.index());
        }
    }
}
