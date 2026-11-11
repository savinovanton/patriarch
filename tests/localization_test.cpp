// Localisation (E4-T06): message syntax, plural rules, tables loaded from .pd files, fallback chain,
// missing keys, load-time validation with positions, mod override, the shipped Russian table.

#include "pat/core/log.hpp"
#include "pat/core/rng.hpp"
#include "pat/data/content_loader.hpp"
#include "pat/data/content_schemas.hpp"
#include "pat/data/content_source.hpp"
#include "pat/loc/localizer.hpp"
#include "pat/loc/message.hpp"
#include "pat/loc/plural.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <limits>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace pat;
using pat::data::MemorySource;
using pat::loc::Arg;
using pat::loc::CompiledMessage;
using pat::loc::FormatResult;
using pat::loc::Localizer;
using pat::loc::LocError;
using pat::loc::LocLoadResult;

// ---- allocation counting -----------------------------------------------------------------------
//
// The "no allocation on the hot path" requirement is checked by replacing the global operator new
// of the test executable with a counting version (a per-thread counter, so other threads cannot
// disturb a measurement). The replacement forwards to malloc/free and is harmless for the other
// tests of this executable. A test reads the counter before and after the code under test.

namespace {
thread_local std::size_t t_allocations = 0;
}  // namespace

void* operator new(std::size_t size) {
    ++t_allocations;
    if (void* p = std::malloc(size != 0 ? size : 1))
        return p;
    throw std::bad_alloc();
}

void operator delete(void* p) noexcept {
    std::free(p);
}

void operator delete(void* p, std::size_t) noexcept {
    std::free(p);
}

namespace {

const std::string kNbsp = "\xC2\xA0";  // U+00A0, the group separator

/// A source with one Russian table.
MemorySource ru_source(const std::string& body, const std::string& name = "test") {
    MemorySource src(name);
    src.add("loc/ru/ui.pd", "format_version = 1\n" + body);
    return src;
}

Localizer load_ok(const MemorySource& src) {
    LocLoadResult r = pat::loc::load_localization(src);
    std::string report;
    for (const LocError& e : r.errors)
        report += e.format() + "; ";
    INFO(report);
    REQUIRE(r.ok());
    return std::move(r.localizer);
}

std::string fmt(const Localizer& loc, std::string_view key, std::initializer_list<Arg> args = {}) {
    return loc.format(key, args);
}

std::string age_text(std::int64_t n) {
    const auto msg = CompiledMessage::compile("{n} {n:plural(год|года|лет)}", loc::kPluralRu);
    REQUIRE(msg.has_value());
    std::string out;
    loc::Output output(out);
    FormatResult status;
    const std::array<Arg, 1> args = {Arg("n", n)};
    msg.value().format(output, args, status);
    CHECK(status.ok());
    return out;
}

/// Loads `body` as the Russian table and returns the single expected error.
LocLoadResult load_ru_with_error(const std::string& body) {
    const MemorySource src = ru_source(body);
    LocLoadResult r = pat::loc::load_localization(src);
    REQUIRE(r.errors.size() == 1);
    return r;
}

}  // namespace

// ---- plural rules --------------------------------------------------------------------------------

TEST_CASE("plural: Russian forms for the US-4.3 table") {
    struct Row {
        std::int64_t n;
        const char* expected;
    };
    const Row rows[] = {{0, "0 лет"},     {1, "1 год"},       {2, "2 года"},      {3, "3 года"},
                        {4, "4 года"},    {5, "5 лет"},       {11, "11 лет"},     {12, "12 лет"},
                        {13, "13 лет"},   {14, "14 лет"},     {21, "21 год"},     {22, "22 года"},
                        {25, "25 лет"},   {101, "101 год"},   {111, "111 лет"},   {112, "112 лет"},
                        {100, "100 лет"}, {1000, "1000 лет"}, {1001, "1001 год"}, {-1, "-1 год"},
                        {-12, "-12 лет"}};
    for (const Row& row : rows) {
        INFO("n = " << row.n);
        CHECK(age_text(row.n) == row.expected);
    }
}

TEST_CASE("plural: form indices of the Russian rule") {
    CHECK(loc::plural_form(loc::kPluralRu, 1) == 0);
    CHECK(loc::plural_form(loc::kPluralRu, 21) == 0);
    CHECK(loc::plural_form(loc::kPluralRu, 11) == 2);
    CHECK(loc::plural_form(loc::kPluralRu, 2) == 1);
    CHECK(loc::plural_form(loc::kPluralRu, 24) == 1);
    CHECK(loc::plural_form(loc::kPluralRu, 12) == 2);
    CHECK(loc::plural_form(loc::kPluralRu, 14) == 2);
    CHECK(loc::plural_form(loc::kPluralRu, 0) == 2);
    CHECK(loc::plural_form(loc::kPluralRu, 111) == 2);
    CHECK(loc::plural_form(loc::kPluralRu, 121) == 0);
    CHECK(loc::kPluralRu.form_count == 3);
    CHECK(loc::kPluralRu.fraction_form == 1);
    // The extreme values must not misbehave (the sign is dropped with unsigned arithmetic).
    CHECK(loc::plural_form(loc::kPluralRu, std::numeric_limits<std::int64_t>::min()) == 2);  // ...808
    CHECK(loc::plural_form(loc::kPluralRu, std::numeric_limits<std::int64_t>::max()) == 2);  // ...807
}

TEST_CASE("plural: English forms") {
    const auto msg = CompiledMessage::compile("{n} {n:plural(year|years)}", loc::kPluralEn);
    REQUIRE(msg.has_value());
    const std::pair<std::int64_t, const char*> rows[] = {{0, "0 years"},
                                                         {1, "1 year"},
                                                         {2, "2 years"},
                                                         {5, "5 years"},
                                                         {11, "11 years"},
                                                         {21, "21 years"},
                                                         {101, "101 years"},
                                                         {-1, "-1 year"}};
    for (const auto& [n, expected] : rows) {
        std::string out;
        loc::Output output(out);
        FormatResult status;
        const std::array<Arg, 1> args = {Arg("n", n)};
        msg.value().format(output, args, status);
        CHECK(out == expected);
    }
    CHECK(loc::kPluralEn.form_count == 2);
}

TEST_CASE("plural: language registry") {
    const auto languages = loc::builtin_languages();
    REQUIRE(languages.size() == 2);
    CHECK(languages[0].code == "en");
    CHECK(languages[1].code == "ru");
    CHECK(loc::is_valid_plural_rule(loc::kPluralRu));
    CHECK(loc::is_valid_plural_rule(loc::kPluralEn));
    CHECK_FALSE(loc::is_valid_plural_rule(loc::PluralRule{}));
    CHECK(loc::is_valid_language_code("ru"));
    CHECK(loc::is_valid_language_code("pt_br"));
    CHECK_FALSE(loc::is_valid_language_code(""));
    CHECK_FALSE(loc::is_valid_language_code("RU"));
    CHECK_FALSE(loc::is_valid_language_code("1ru"));
    CHECK_FALSE(loc::is_valid_language_code("ru-RU"));
}

// ---- parameters, escapes, grouping ---------------------------------------------------------------

TEST_CASE("message: parameters, escapes and unused arguments") {
    const Localizer loc = load_ok(ru_source("ui.hello = \"Привет, {name}! Тебе {age} лет.\"\n"
                                            "ui.braces = \"{{не параметр}} и }} и {{ и {n}\"\n"
                                            "ui.plain = \"Просто текст\"\n"
                                            "ui.dup = \"{x}{x}\"\n"));
    CHECK(fmt(loc, "ui.hello", {Arg("name", "Пётр"), Arg("age", 31)}) == "Привет, Пётр! Тебе 31 лет.");
    CHECK(fmt(loc, "ui.hello", {Arg("age", 7), Arg("name", std::string_view("Анна")), Arg("unused", 1)}) ==
          "Привет, Анна! Тебе 7 лет.");
    CHECK(fmt(loc, "ui.braces", {Arg("n", -42)}) == "{не параметр} и } и { и -42");
    CHECK(fmt(loc, "ui.plain") == "Просто текст");
    CHECK(fmt(loc, "ui.dup", {Arg("x", 1), Arg("x", 2)}) == "11");  // the first argument of a name wins
    CHECK(loc.parameters("ru", "ui.hello") == std::vector<std::string>{"age", "name"});
    CHECK(loc.parameters("ru", "ui.plain").empty());
}

TEST_CASE("message: missing or mistyped arguments are visible and flagged") {
    const Localizer loc = load_ok(ru_source("ui.a = \"{x}\"\n"
                                            "ui.b = \"{n:group}\"\n"
                                            "ui.c = \"{n:plural(а|б|в)}\"\n"));
    core::ScopedLogCapture capture;
    std::string out;
    FormatResult r = loc.format_to(out, "ui.a");
    CHECK(out == "{!x}");
    CHECK(r.bad_argument);
    CHECK_FALSE(r.ok());

    out.clear();
    r = loc.format_to(out, "ui.b", {Arg("n", "текст")});
    CHECK(out == "{!n}");
    CHECK(r.bad_argument);

    out.clear();
    r = loc.format_to(out, "ui.c", {Arg("n", "текст")});
    CHECK(out == "{!n}");
    CHECK(r.bad_argument);

    // Logged once per key, however often it is formatted.
    out.clear();
    for (int i = 0; i < 5; ++i)
        (void)loc.format_to(out, "ui.a");
    std::size_t warnings = 0;
    for (const auto& entry : capture.sink().entries()) {
        if (entry.level == core::LogLevel::Warn && entry.message.find("'ui.a'") != std::string::npos)
            ++warnings;
    }
    CHECK(warnings == 1);
}

TEST_CASE("message: integers are formatted without locale, grouping uses U+00A0") {
    const Localizer loc = load_ok(ru_source("ui.plain = \"{n}\"\n"
                                            "ui.group = \"{n:group}\"\n"));
    CHECK(fmt(loc, "ui.plain", {Arg("n", 12480)}) == "12480");
    CHECK(fmt(loc, "ui.plain", {Arg("n", 0)}) == "0");
    CHECK(fmt(loc, "ui.plain", {Arg("n", -7)}) == "-7");
    CHECK(fmt(loc, "ui.plain", {Arg("n", std::numeric_limits<std::int64_t>::min())}) ==
          "-9223372036854775808");
    CHECK(fmt(loc, "ui.plain", {Arg("n", std::numeric_limits<std::int64_t>::max())}) ==
          "9223372036854775807");

    CHECK(fmt(loc, "ui.group", {Arg("n", 12480)}) == "12" + kNbsp + "480");
    CHECK(fmt(loc, "ui.group", {Arg("n", 0)}) == "0");
    CHECK(fmt(loc, "ui.group", {Arg("n", 999)}) == "999");
    CHECK(fmt(loc, "ui.group", {Arg("n", 1000)}) == "1" + kNbsp + "000");
    CHECK(fmt(loc, "ui.group", {Arg("n", 1234567)}) == "1" + kNbsp + "234" + kNbsp + "567");
    CHECK(fmt(loc, "ui.group", {Arg("n", -12480)}) == "-12" + kNbsp + "480");
    CHECK(fmt(loc, "ui.group", {Arg("n", std::numeric_limits<std::int64_t>::min())}) ==
          "-9" + kNbsp + "223" + kNbsp + "372" + kNbsp + "036" + kNbsp + "854" + kNbsp + "775" + kNbsp +
              "808");
    // The separator is exactly the two bytes C2 A0, not an ordinary space.
    const std::string grouped = fmt(loc, "ui.group", {Arg("n", 12480)});
    REQUIRE(grouped.size() == 7);
    CHECK(static_cast<unsigned char>(grouped[2]) == 0xC2);
    CHECK(static_cast<unsigned char>(grouped[3]) == 0xA0);
    CHECK(grouped.find(' ') == std::string::npos);
}

TEST_CASE("message: format_integer fills the caller's scratch buffer") {
    std::array<char, loc::kIntegerBufferSize> buffer{};
    CHECK(loc::format_integer(buffer, 0, false) == "0");
    CHECK(loc::format_integer(buffer, -1, true) == "-1");
    CHECK(loc::format_integer(buffer, 1000000, false) == "1000000");
    CHECK(loc::format_integer(buffer, 1000000, true) == "1" + kNbsp + "000" + kNbsp + "000");
}

TEST_CASE("message: a compiled message reports its structure") {
    const auto msg = CompiledMessage::compile("a {b} {c:group} {d:plural(x|y|z)} {b}", loc::kPluralRu);
    REQUIRE(msg.has_value());
    CHECK(msg.value().parameter_names() == std::vector<std::string>{"b", "c", "d"});
    CHECK_FALSE(msg.value().is_plain());
    const auto plain = CompiledMessage::compile("only {{text}}", loc::kPluralRu);
    REQUIRE(plain.has_value());
    CHECK(plain.value().is_plain());
    const auto empty = CompiledMessage::compile("", loc::kPluralRu);
    REQUIRE(empty.has_value());
    std::string out;
    loc::Output output(out);
    FormatResult status;
    empty.value().format(output, {}, status);
    CHECK(out.empty());
}

TEST_CASE("message: empty plural forms are allowed") {
    const Localizer loc = load_ok(ru_source("ui.k = \"{n} книг{n:plural(а||)}\"\n"));
    CHECK(fmt(loc, "ui.k", {Arg("n", 1)}) == "1 книга");
    CHECK(fmt(loc, "ui.k", {Arg("n", 2)}) == "2 книг");
    CHECK(fmt(loc, "ui.k", {Arg("n", 5)}) == "5 книг");
}

// ---- fixed buffers -------------------------------------------------------------------------------

TEST_CASE("format_to a fixed buffer truncates on a code point boundary") {
    const Localizer loc = load_ok(ru_source("ui.long = \"Москва — третий Рим, {n} лет\"\n"));
    const std::string full = fmt(loc, "ui.long", {Arg("n", 5)});
    REQUIRE(full == "Москва — третий Рим, 5 лет");
    for (std::size_t capacity = 0; capacity <= full.size() + 2; ++capacity) {
        std::vector<char> storage(capacity + 1, '#');  // one guard byte after the span
        const FormatResult r =
            loc.format_to(std::span<char>(storage.data(), capacity), "ui.long", {Arg("n", 5)});
        INFO("capacity = " << capacity);
        CHECK(storage[capacity] == '#');  // never written past the span
        REQUIRE(r.size <= capacity);
        const std::string_view written(storage.data(), r.size);
        CHECK(full.starts_with(written));
        if (capacity >= full.size()) {
            CHECK_FALSE(r.truncated);
            CHECK(written == full);
        } else {
            CHECK(r.truncated);
            // Whole code points only: the next byte of the full text starts a character.
            CHECK((static_cast<unsigned char>(full[r.size]) & 0xC0) != 0x80);
            CHECK(r.size + 4 > capacity);
        }
    }
}

TEST_CASE("format_to appends to a string and reports the appended size") {
    const Localizer loc = load_ok(ru_source("ui.t = \"{n}!\"\n"));
    std::string out = "> ";
    const FormatResult r = loc.format_to(out, "ui.t", {Arg("n", 3)});
    CHECK(out == "> 3!");
    CHECK(r.size == 2);
    CHECK(r.ok());
}

// ---- hot path: no allocation ---------------------------------------------------------------------

TEST_CASE("formatting does not allocate when the output has capacity") {
    const Localizer loc = load_ok(ru_source("ui.age = \"{n} {n:plural(год|года|лет)}\"\n"
                                            "ui.res = \"{label}: {n:group}\"\n"
                                            "ui.plain = \"Просто текст\"\n"));
    std::string out;
    out.reserve(256);
    std::array<char, 128> fixed{};

    const std::size_t before = t_allocations;
    for (int i = 0; i < 1000; ++i) {
        out.clear();
        const std::array<Arg, 2> args = {Arg("n", i * 37), Arg("label", "Казна")};
        (void)loc.format_to(out, "ui.age", args);
        (void)loc.format_to(out, "ui.res", args);
        (void)loc.format_to(out, "ui.plain");
        (void)loc.format_to(fixed, "ui.res", args);
    }
    const std::size_t after = t_allocations;
    CHECK(after == before);

    // Sanity check of the counter itself: a string without capacity does allocate.
    const std::size_t baseline = t_allocations;
    std::string grown;
    (void)loc.format_to(
        grown, "ui.res", std::array<Arg, 2>{Arg("n", 1), Arg("label", "Казна, которая длинная")});
    CHECK(t_allocations > baseline);
}

// ---- loading and validation ----------------------------------------------------------------------

TEST_CASE("loading: keys, languages and origins") {
    MemorySource src("test");
    src.add("loc/ru/ui.pd", "format_version = 1\nui.a = \"А\"\nui.b = \"Б\"\n");
    src.add("loc/ru/events/court.pd", "format_version = 1\nevent.court.title = \"Двор\"\n");
    src.add("loc/en/ui.pd", "format_version = 1\nui.a = \"A\"\n");
    src.add("assets/readme.txt", "ignored");
    src.add("economy/goods.pd", "ignored: not a loc path");
    src.add("loc/ru/notes.txt", "ignored: not a .pd file");
    const Localizer loc = load_ok(src);
    CHECK(loc.languages() == std::vector<std::string>{"en", "ru"});
    CHECK(loc.keys("ru") == std::vector<std::string>{"event.court.title", "ui.a", "ui.b"});
    CHECK(loc.key_count("en") == 1);
    CHECK(loc.key_count("de") == 0);
    CHECK(loc.has_language("en"));
    CHECK_FALSE(loc.has_language("de"));
    CHECK(loc.contains("ru", "ui.b"));
    CHECK_FALSE(loc.contains("en", "ui.b"));
    CHECK(loc.default_language() == "ru");
    CHECK(loc.chain() == std::vector<std::string>{"ru", "en"});
    const auto origin = loc.origin("ru", "event.court.title");
    REQUIRE(origin.has_value());
    CHECK(origin->file == "loc/ru/events/court.pd");
    CHECK(origin->root == 0);
    CHECK(origin->pos.line == 2);
    CHECK(origin->pos.column == 1);
    CHECK_FALSE(loc.origin("ru", "ui.nope").has_value());
}

TEST_CASE("loading: file and entry level errors") {
    {
        MemorySource src("test");
        src.add("loc/stray.pd", "format_version = 1\n");
        const LocLoadResult r = pat::loc::load_localization(src);
        REQUIRE(r.errors.size() == 1);
        CHECK(r.errors[0].code == "LC001");
        CHECK(r.errors[0].file == "loc/stray.pd");
    }
    {
        MemorySource src("test");
        src.add("loc/de/a.pd", "format_version = 1\nui.a = \"x\"\n");
        src.add("loc/de/b.pd", "format_version = 1\nui.b = \"x\"\n");
        src.add("loc/Bad Lang/a.pd", "format_version = 1\n");
        const LocLoadResult r = pat::loc::load_localization(src);
        std::size_t unknown = 0;
        for (const LocError& e : r.errors)
            unknown += e.code == "LC002" ? 1 : 0;
        CHECK(unknown == 2);  // once per language directory, not per file
        CHECK_FALSE(r.localizer.has_language("de"));
    }
    {
        const LocLoadResult r = load_ru_with_error("ui.a = 5\n");
        CHECK(r.errors[0].code == "LC004");
        CHECK(r.errors[0].pos.line == 2);
        CHECK(r.errors[0].pos.column == 8);
    }
    {
        const LocLoadResult r = load_ru_with_error("ui.a > \"x\"\n");
        CHECK(r.errors[0].code == "LC004");
        CHECK(r.errors[0].pos.line == 2);
        CHECK(r.errors[0].pos.column == 1);
    }
    {
        const LocLoadResult r = load_ru_with_error("ui.a \"label\" { }\n");
        CHECK(r.errors[0].code == "LC004");
    }
    {
        const LocLoadResult r = load_ru_with_error("format_version = 1\n");
        CHECK(r.errors[0].code == "LC004");  // a second header is not a string entry
    }
    {
        MemorySource src("test");
        src.add("loc/ru/ui.pd", "format_version = 2\nui.a = \"x\"\n");
        const LocLoadResult r = pat::loc::load_localization(src);
        REQUIRE(r.errors.size() == 1);
        CHECK(r.errors[0].code == "PD021");
        CHECK(r.errors[0].pos.line == 1);
        CHECK(r.errors[0].pos.column == 18);
        CHECK_FALSE(r.localizer.contains("ru", "ui.a"));
    }
    {
        MemorySource src("test");
        src.add("loc/ru/ui.pd", "ui.a = \"x\"\n");
        const LocLoadResult r = pat::loc::load_localization(src);
        REQUIRE_FALSE(r.errors.empty());
        CHECK(r.errors[0].code == "PD020");
    }
    {
        MemorySource src("test");
        src.add("loc/ru/ui.pd", "format_version = 1\nui.a = \"unterminated\n");
        const LocLoadResult r = pat::loc::load_localization(src);
        REQUIRE_FALSE(r.errors.empty());
        CHECK(r.errors[0].code == "PD002");
        CHECK(r.errors[0].pos.line == 2);
    }
    {
        MemorySource src("test");
        src.add_unreadable("loc/ru/ui.pd", "access denied");
        const LocLoadResult r = pat::loc::load_localization(src);
        REQUIRE(r.errors.size() == 1);
        CHECK(r.errors[0].code == "PD113");
    }
    {
        MemorySource src("test");
        src.fail_listing("no such directory");
        const LocLoadResult r = pat::loc::load_localization(src);
        REQUIRE(r.errors.size() == 1);
        CHECK(r.errors[0].code == "PD113");
        CHECK(r.errors[0].file.empty());
    }
}

TEST_CASE("loading: duplicate keys inside one root are errors with positions") {
    {
        const LocLoadResult r = load_ru_with_error("ui.a = \"x\"\nui.a = \"y\"\n");
        CHECK(r.errors[0].code == "LC003");
        CHECK(r.errors[0].file == "loc/ru/ui.pd");
        CHECK(r.errors[0].pos.line == 3);
        CHECK(r.errors[0].pos.column == 1);
        CHECK(r.errors[0].message.find("loc/ru/ui.pd:2:1") != std::string::npos);  // where the first one is
        CHECK(r.localizer.format("ui.a") == "x");                                  // the first one is kept
    }
    {
        MemorySource src("test");
        src.add("loc/ru/a.pd", "format_version = 1\nui.k = \"1\"\n");
        src.add("loc/ru/b.pd", "format_version = 1\n\nui.k = \"2\"\n");
        const LocLoadResult r = pat::loc::load_localization(src);
        REQUIRE(r.errors.size() == 1);
        CHECK(r.errors[0].code == "LC003");
        CHECK(r.errors[0].file == "loc/ru/b.pd");  // files are visited in path order
        CHECK(r.errors[0].pos.line == 3);
        CHECK(r.localizer.format("ui.k") == "1");
    }
    {
        // The same key in two languages is not a conflict.
        MemorySource src("test");
        src.add("loc/ru/a.pd", "format_version = 1\nui.k = \"1\"\n");
        src.add("loc/en/a.pd", "format_version = 1\nui.k = \"one\"\n");
        CHECK(pat::loc::load_localization(src).ok());
    }
}

TEST_CASE("loading: message errors point into the .pd file") {
    struct Case {
        const char* body;
        const char* code;
        std::uint32_t line;
        std::uint32_t column;
    };
    const Case cases[] = {
        // `ui.a = "x }"`: the quote is column 8, `}` is the third character of the text.
        {"ui.a = \"x }\"\n", "LC010", 2, 11},
        {"ui.a = \"x {\"\n", "LC011", 2, 11},
        {"ui.a = \"{n\"\n", "LC011", 2, 9},
        {"ui.a = \"{}\"\n", "LC012", 2, 10},
        {"ui.a = \"{1x}\"\n", "LC012", 2, 10},
        {"ui.a = \"{n x}\"\n", "LC012", 2, 11},
        {"ui.a = \"{n:}\"\n", "LC013", 2, 12},
        {"ui.a = \"{n:upper}\"\n", "LC013", 2, 12},
        {"ui.a = \"{n:group(3)}\"\n", "LC012", 2, 17},
        {"ui.a = \"{n:plural}\"\n", "LC014", 2, 18},
        {"ui.a = \"{n:plural(a|b|c}\"\n", "LC014", 2, 24},
        {"ui.a = \"{n:plural(a|b|c\"\n", "LC014", 2, 9},
        {"ui.a = \"{n:plural(a|b|c)x}\"\n", "LC014", 2, 25},
        {"ui.a = \"{n:plural(a|{b}|c)}\"\n", "LC014", 2, 21},
        {"ui.a = \"{n:plural(a|b)}\"\n", "LC015", 2, 12},
        {"ui.a = \"{n:plural(a|b|c|d)}\"\n", "LC015", 2, 12},
        {"ui.a = \"{n:plural()}\"\n", "LC015", 2, 12},
        // Columns count code points: "Привет " is 7 characters.
        {"ui.a = \"Привет }\"\n", "LC010", 2, 16},
        // An escape sequence is one unit in the source: `\"` takes two columns.
        {"ui.a = \"a\\\"}\"\n", "LC010", 2, 12},
        {"ui.a = \"\\u{43F}}\"\n", "LC010", 2, 16},
        {"ui.a = \"\\n}\"\n", "LC010", 2, 11},
        // Block strings: the line break right after the opening quotes is dropped.
        {"ui.a = \"\"\"\nline one\nbad } here\"\"\"\n", "LC010", 4, 5},
        {"ui.a = \"\"\"bad } here\"\"\"\n", "LC010", 2, 15},
    };
    for (const Case& c : cases) {
        const std::string body = c.body;
        INFO(body);
        const LocLoadResult r = load_ru_with_error(c.body);
        CHECK(r.errors[0].code == c.code);
        CHECK(r.errors[0].pos.line == c.line);
        CHECK(r.errors[0].pos.column == c.column);
        CHECK(r.errors[0].message.find("ui.a") != std::string::npos);
        CHECK(r.errors[0].file == "loc/ru/ui.pd");
    }
}

TEST_CASE("loading: the English table needs two plural forms") {
    MemorySource src("test");
    src.add("loc/en/ui.pd", "format_version = 1\nui.a = \"{n:plural(a|b|c)}\"\nui.b = \"{n:plural(a|b)}\"\n");
    const LocLoadResult r = pat::loc::load_localization(src);
    REQUIRE(r.errors.size() == 1);
    CHECK(r.errors[0].code == "LC015");
    CHECK(r.errors[0].pos.line == 2);
    CHECK(r.localizer.contains("en", "ui.b"));  // valid entries still load
    CHECK_FALSE(r.localizer.contains("en", "ui.a"));
}

TEST_CASE("loading: all errors are reported, valid entries survive, output is sorted") {
    MemorySource src("test");
    src.add("loc/ru/ui.pd",
            "format_version = 1\n"
            "ui.ok = \"fine\"\n"
            "ui.bad1 = \"}\"\n"
            "ui.bad2 = \"{\"\n"
            "ui.ok2 = \"{n}\"\n"
            "ui.bad3 = \"{x:oops}\"\n");
    const LocLoadResult r = pat::loc::load_localization(src);
    REQUIRE(r.errors.size() == 3);
    CHECK(r.errors[0].pos.line == 3);
    CHECK(r.errors[1].pos.line == 4);
    CHECK(r.errors[2].pos.line == 6);
    CHECK(r.localizer.keys("ru") == std::vector<std::string>{"ui.ok", "ui.ok2"});
    CHECK(r.errors[0].format().find("loc/ru/ui.pd:3:") != std::string::npos);
    CHECK(r.errors[0].format().find("error[LC010]") != std::string::npos);
}

TEST_CASE("loading: the result does not depend on the order a source lists files") {
    const auto make = [](bool reversed) {
        std::vector<std::pair<std::string, std::string>> files = {
            {"loc/ru/a.pd", "format_version = 1\nui.k = \"1\"\nui.x = \"}\"\n"},
            {"loc/ru/b.pd", "format_version = 1\nui.k = \"2\"\nui.y = \"y\"\n"},
            {"loc/ru/c.pd", "format_version = 1\nui.z = \"z\"\n"},
            {"loc/en/a.pd", "format_version = 1\nui.k = \"k\"\n"}};
        if (reversed)
            std::reverse(files.begin(), files.end());
        MemorySource src("test");
        for (const auto& [path, text] : files)
            src.add(path, text);
        return src;
    };
    const LocLoadResult a = pat::loc::load_localization(make(false));
    const LocLoadResult b = pat::loc::load_localization(make(true));
    REQUIRE(a.errors.size() == b.errors.size());
    for (std::size_t i = 0; i < a.errors.size(); ++i)
        CHECK(a.errors[i].format() == b.errors[i].format());
    CHECK(a.localizer.keys("ru") == b.localizer.keys("ru"));
    CHECK(a.localizer.format("ui.k") == b.localizer.format("ui.k"));
    CHECK(a.localizer.format("ui.k") == "1");
}

TEST_CASE("loading: a mod root overrides single keys and may add keys and languages") {
    MemorySource base("base");
    base.add("loc/ru/ui.pd", "format_version = 1\nui.a = \"базовое а\"\nui.b = \"базовое б\"\n");
    MemorySource mod("mods/my_mod");
    mod.add("loc/ru/mod.pd", "format_version = 1\nui.b = \"мод б\"\nui.c = \"мод в\"\n");
    mod.add("loc/en/mod.pd", "format_version = 1\nui.c = \"mod c\"\n");
    mod.add("economy/other.pd", "ignored");
    const data::ContentSource* roots[] = {&base, &mod};
    const LocLoadResult r = pat::loc::load_localization(roots);
    REQUIRE(r.ok());
    CHECK(r.localizer.format("ui.a") == "базовое а");
    CHECK(r.localizer.format("ui.b") == "мод б");
    CHECK(r.localizer.format("ui.c") == "мод в");
    CHECK(r.localizer.keys("ru") == std::vector<std::string>{"ui.a", "ui.b", "ui.c"});
    CHECK(r.localizer.origin("ru", "ui.a")->root == 0);
    CHECK(r.localizer.origin("ru", "ui.b")->root == 1);
    CHECK(r.localizer.origin("ru", "ui.b")->file == "loc/ru/mod.pd");
    CHECK(r.localizer.has_language("en"));

    // Reversed roots: the other side wins.
    const data::ContentSource* reversed[] = {&mod, &base};
    const LocLoadResult r2 = pat::loc::load_localization(reversed);
    REQUIRE(r2.ok());
    CHECK(r2.localizer.format("ui.b") == "базовое б");
}

TEST_CASE("loading: the same key twice inside a mod is still an error, in the right root") {
    MemorySource base("base");
    base.add("loc/ru/ui.pd", "format_version = 1\nui.a = \"x\"\n");
    MemorySource mod("mod");
    mod.add("loc/ru/m.pd", "format_version = 1\nui.q = \"1\"\nui.q = \"2\"\n");
    const data::ContentSource* roots[] = {&base, &mod};
    const LocLoadResult r = pat::loc::load_localization(roots);
    REQUIRE(r.errors.size() == 1);
    CHECK(r.errors[0].code == "LC003");
    CHECK(r.errors[0].root == 1);
    CHECK(r.errors[0].root_name == "mod");
    CHECK(r.errors[0].format().starts_with("[root 1 'mod'] loc/ru/m.pd:3:1"));
}

TEST_CASE("loading: another language can be registered with its own plural rule") {
    pat::loc::LoadOptions options;
    // A made-up language with a single form (like Chinese or Japanese).
    options.languages.push_back(
        {"zz", loc::PluralRule{1, 0, [](std::uint64_t) noexcept -> std::uint8_t { return 0; }}});
    MemorySource src("test");
    src.add("loc/zz/ui.pd", "format_version = 1\nui.a = \"{n}{n:plural(x)}\"\nui.b = \"{n:plural(x|y)}\"\n");
    const LocLoadResult r = pat::loc::load_localization(src, options);
    REQUIRE(r.errors.size() == 1);
    CHECK(r.errors[0].code == "LC015");
    CHECK(r.localizer.contains("zz", "ui.a"));
}

TEST_CASE("loading: the content loader leaves the loc/ directory to this module") {
    MemorySource src("test");
    src.add("loc/ru/ui.pd", "format_version = 1\nui.a = \"text\"\nthis is not content\n");
    src.add("loc/en/deep/ui.pd", "format_version = 1\nui.a = \"text\"\n");
    const data::LoadResult loaded = data::load_content(src, data::content_schemas());
    CHECK(loaded.ok());
}

// ---- fallback chain and missing keys -------------------------------------------------------------

namespace {

Localizer two_language_localizer() {
    MemorySource src("test");
    src.add("loc/ru/ui.pd", "format_version = 1\nui.both = \"ру\"\nui.ru_only = \"только ру\"\n");
    src.add("loc/en/ui.pd",
            "format_version = 1\nui.both = \"en\"\nui.en_only = \"only en\"\nui.extra = \"extra\"\n");
    return load_ok(src);
}

}  // namespace

TEST_CASE("fallback chain: ru, then en, then the placeholder") {
    core::ScopedLogCapture capture;
    Localizer loc = two_language_localizer();
    CHECK(fmt(loc, "ui.both") == "ру");
    CHECK(fmt(loc, "ui.ru_only") == "только ру");
    CHECK(fmt(loc, "ui.en_only") == "only en");  // not in ru: falls back to en
    CHECK(fmt(loc, "ui.nowhere") == "[[ui.nowhere]]");
    CHECK(loc.resolves("ui.en_only"));
    CHECK_FALSE(loc.resolves("ui.nowhere"));

    const std::array<std::string_view, 1> only_en = {"en"};
    CHECK(loc.set_chain(only_en) == 1);
    CHECK(fmt(loc, "ui.both") == "en");
    CHECK(fmt(loc, "ui.ru_only") == "[[ui.ru_only]]");

    // Languages without a table are skipped; the chain keeps their names.
    const std::array<std::string_view, 3> with_unknown = {"de", "en", "ru"};
    CHECK(loc.set_chain(with_unknown) == 2);
    CHECK(loc.chain() == std::vector<std::string>{"de", "en", "ru"});
    CHECK(fmt(loc, "ui.both") == "en");

    CHECK(loc.set_chain({}) == 0);
    CHECK(fmt(loc, "ui.both") == "[[ui.both]]");
}

TEST_CASE("missing keys: placeholder, flag, one log line per key, record for tools") {
    core::ScopedLogCapture capture;
    Localizer loc = two_language_localizer();
    std::string out;
    const FormatResult r = loc.format_to(out, "ui.absent.key");
    CHECK(out == "[[ui.absent.key]]");
    CHECK(r.missing_key);
    CHECK_FALSE(r.ok());
    CHECK(r.size == out.size());

    for (int i = 0; i < 10; ++i)
        (void)loc.format("ui.absent.key");
    (void)loc.format("ui.other.absent");
    (void)loc.format("ui.other.absent");

    std::size_t first = 0;
    std::size_t second = 0;
    for (const auto& entry : capture.sink().entries()) {
        if (entry.level != core::LogLevel::Warn)
            continue;
        CHECK(entry.category == "loc");
        first += entry.message.find("'ui.absent.key'") != std::string::npos ? 1 : 0;
        second += entry.message.find("'ui.other.absent'") != std::string::npos ? 1 : 0;
    }
    CHECK(first == 1);
    CHECK(second == 1);
    CHECK(loc.requested_missing_keys() == std::vector<std::string>{"ui.absent.key", "ui.other.absent"});

    // After a reset the key is reported again.
    loc.reset_missing_log();
    CHECK(loc.requested_missing_keys().empty());
    (void)loc.format("ui.absent.key");
    std::size_t again = 0;
    for (const auto& entry : capture.sink().entries())
        again += entry.message.find("'ui.absent.key'") != std::string::npos ? 1 : 0;
    CHECK(again == 2);

    // A fixed buffer shows (a part of) the placeholder as well.
    std::array<char, 8> small{};
    const FormatResult t = loc.format_to(small, "ui.absent.key");
    CHECK(t.missing_key);
    CHECK(t.truncated);
    CHECK(std::string_view(small.data(), t.size) == "[[ui.abs");
}

TEST_CASE("missing keys: coverage and lint helpers") {
    const Localizer loc = two_language_localizer();
    CHECK(loc.missing_keys("en", "ru") == std::vector<std::string>{"ui.ru_only"});
    CHECK(loc.missing_keys("ru", "en") == std::vector<std::string>{"ui.en_only", "ui.extra"});
    CHECK(loc.missing_keys("de", "ru") == std::vector<std::string>{"ui.both", "ui.ru_only"});
    CHECK(loc.missing_keys("ru", "de").empty());

    const auto coverage = loc.coverage();
    REQUIRE(coverage.size() == 1);
    CHECK(coverage[0].language == "en");
    CHECK(coverage[0].total == 3);
    CHECK(coverage[0].missing == std::vector<std::string>{"ui.ru_only"});
    CHECK(coverage[0].extra == std::vector<std::string>{"ui.en_only", "ui.extra"});

    const std::array<std::string_view, 4> used = {"ui.both", "ui.zzz", "ui.aaa", "ui.zzz"};
    CHECK(loc.absent_keys("ru", used) == std::vector<std::string>{"ui.aaa", "ui.zzz"});
    CHECK(loc.unused_keys("ru", used) == std::vector<std::string>{"ui.ru_only"});
    CHECK(loc.unused_keys("en", used) == std::vector<std::string>{"ui.en_only", "ui.extra"});
    CHECK(loc.unused_keys("de", used).empty());
}

// ---- the shipped Russian table -------------------------------------------------------------------

TEST_CASE("the shipped Russian table loads and every key formats") {
    const data::DirectorySource dir{std::string(PATRIARCH_DATA_DIR)};
    LocLoadResult r = pat::loc::load_localization(dir);
    std::string report;
    for (const LocError& e : r.errors)
        report += e.format() + "; ";
    INFO(report);
    REQUIRE(r.ok());
    const Localizer& loc = r.localizer;
    REQUIRE(loc.has_language("ru"));
    const std::vector<std::string> keys = loc.keys("ru");
    CHECK(keys.size() > 60);

    // Every parameter gets an integer: all of {x}, {x:group} and {x:plural(...)} accept one.
    for (const std::string& key : keys) {
        INFO("key: " << key);
        const std::vector<std::string> names = loc.parameters("ru", key);
        std::vector<Arg> args;
        for (const std::string& name : names)
            args.emplace_back(name, 1480);
        std::string out;
        const FormatResult status = loc.format_to(out, key, args);
        CHECK(status.ok());
        CHECK_FALSE(out.empty());
        CHECK(out.find("[[") == std::string::npos);
        CHECK(out.find("{!") == std::string::npos);
    }

    // The shipped tables now include content names as well as UI messages.
    for (const std::string& key : keys) {
        CHECK(key.find('.') != std::string::npos);
        for (const char c : key)
            CHECK(((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.'));
    }

    // US-4.3 through the shipped table.
    const std::pair<std::int64_t, const char*> ages[] = {{1, "1 год"},
                                                         {2, "2 года"},
                                                         {5, "5 лет"},
                                                         {11, "11 лет"},
                                                         {21, "21 год"},
                                                         {22, "22 года"},
                                                         {25, "25 лет"},
                                                         {111, "111 лет"}};
    for (const auto& [n, expected] : ages)
        CHECK(loc.format("ui.age.years", {Arg("n", n)}) == expected);
    CHECK(loc.format("ui.fmt.number", {Arg("n", 12480)}) == "12" + kNbsp + "480");
    CHECK(loc.format("ui.nav.chronicle") == "Журнал");
    CHECK(loc.format("ui.fmt.rouble", {Arg("n", 1248)}) == "1" + kNbsp + "248 ₽");
    CHECK(loc.format("ui.hud.res.capacity", {Arg("n", 1600)}) == "из 1" + kNbsp + "600");
    // The age is composed from ui.age.years (with its plural) and passed to the subtitle as text.
    const std::string age = loc.format("ui.age.years", {Arg("n", 22)});
    CHECK(age == "22 года");
    CHECK(loc.format("ui.character.subtitle",
                     {Arg("role", "стольник"), Arg("age", age), Arg("health", "крепкое")}) ==
          "стольник · 22 года · здоровье: крепкое");
}

// ---- robustness ----------------------------------------------------------------------------------

TEST_CASE("fuzz: random message texts never crash and the output stays bounded") {
    // Characters that matter to the syntax, plus letters, digits and non-ASCII text.
    const std::vector<std::string> alphabet = {"{",  "}",  "|", "(", ")",          ":",     "n",
                                               "x",  "_",  "1", " ", "plural",     "group", "{n}",
                                               "{{", "}}", "é", "я", "{n:plural(", "|b|c)}"};
    for (std::uint64_t seed = 1; seed <= 40; ++seed) {
        core::Pcg32 rng(seed, 7);
        for (int round = 0; round < 250; ++round) {
            std::string text;
            const std::uint32_t pieces = rng.next_u32() % 14;
            for (std::uint32_t i = 0; i < pieces; ++i)
                text += alphabet[rng.next_u32() % alphabet.size()];
            for (const loc::PluralRule* rule : {&loc::kPluralRu, &loc::kPluralEn}) {
                const auto msg = CompiledMessage::compile(text, *rule);
                if (!msg.has_value()) {
                    CHECK(msg.error().offset <= text.size());
                    CHECK_FALSE(msg.error().message.empty());
                    continue;
                }
                std::string out;
                loc::Output output(out);
                FormatResult status;
                const std::array<Arg, 3> args = {
                    Arg("n", static_cast<std::int64_t>(rng.next_u32()) - 2000000000),
                    Arg("x", "текст"),
                    Arg("_", 5)};
                msg.value().format(output, args, status);
                CHECK(out.size() <= 64 * (text.size() + 1));

                std::array<char, 9> tiny{};
                loc::Output fixed{std::span<char>(tiny)};
                FormatResult status2;
                msg.value().format(fixed, args, status2);
                CHECK(fixed.size() <= tiny.size());
            }
        }
    }
}

TEST_CASE("fuzz: random tables load without crashing and positions stay inside the file") {
    const std::vector<std::string> alphabet = {
        "{", "}", "|", "(", ")", ":", "n", "plural", "group", "я", " ", "{n}", "{{", "é"};
    for (std::uint64_t seed = 1; seed <= 25; ++seed) {
        core::Pcg32 rng(seed, 11);
        std::string file = "format_version = 1\n";
        std::uint32_t lines = 1;
        for (int k = 0; k < 40; ++k) {
            std::string text;
            const std::uint32_t pieces = rng.next_u32() % 10;
            for (std::uint32_t i = 0; i < pieces; ++i)
                text += alphabet[rng.next_u32() % alphabet.size()];
            // Mix the three string forms: plain, with a harmless escape, block.
            switch (rng.next_u32() % 3) {
                case 0:
                    file += "k.n" + std::to_string(k) + " = \"" + text + "\"\n";
                    ++lines;
                    break;
                case 1:
                    file += "k.n" + std::to_string(k) + " = \"\\t" + text + "\\u{44F}\"\n";
                    ++lines;
                    break;
                default:
                    file += "k.n" + std::to_string(k) + " = \"\"\"\n" + text + "\n" + text + "\"\"\"\n";
                    lines += 3;
                    break;
            }
        }
        MemorySource src("fuzz");
        src.add("loc/ru/f.pd", file);
        const LocLoadResult r = pat::loc::load_localization(src);
        for (const LocError& e : r.errors) {
            CHECK(e.pos.line >= 2);
            CHECK(e.pos.line <= lines + 1);
            CHECK(e.pos.column >= 1);
            CHECK_FALSE(e.format().empty());
        }
        CHECK(r.localizer.key_count("ru") + r.errors.size() <= 40);
    }
}

TEST_CASE("fuzz: random bytes as a table file never crash the loader") {
    for (std::uint64_t seed = 1; seed <= 60; ++seed) {
        core::Pcg32 rng(seed, 3);
        std::string file = "format_version = 1\n";
        const std::uint32_t length = rng.next_u32() % 200;
        for (std::uint32_t i = 0; i < length; ++i)
            file += static_cast<char>(rng.next_u32() % 128);
        MemorySource src("fuzz");
        src.add("loc/ru/f.pd", file);
        const LocLoadResult r = pat::loc::load_localization(src);
        (void)r;
    }
}
