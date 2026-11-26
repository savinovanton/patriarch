#include "pat/core/json_reader.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <limits>
#include <string>

using pat::core::JsonValue;
using pat::core::parse_json;

TEST_CASE("json: values, nesting and member order") {
    const auto r = parse_json(R"( {"a": 1, "b": [true, false, null, "x"], "c": {"d": -12}, "e": 1.5e3} )");
    REQUIRE(r.has_value());
    const JsonValue& v = r.value();
    REQUIRE(v.is_object());
    REQUIRE(v.members.size() == 4);
    CHECK(v.members[0].key == "a");
    CHECK(v.members[3].key == "e");
    CHECK(v.find("a")->as_i64() == 1);
    const JsonValue* b = v.find("b");
    REQUIRE(b != nullptr);
    REQUIRE(b->items.size() == 4);
    CHECK(b->items[0].boolean);
    CHECK_FALSE(b->items[1].boolean);
    CHECK(b->items[2].is_null());
    CHECK(b->items[3].text == "x");
    CHECK(v.find("c")->find("d")->as_i64() == -12);
    // Non-integers keep their text and are not integers.
    CHECK(v.find("e")->text == "1.5e3");
    CHECK_FALSE(v.find("e")->as_i64().has_value());
    CHECK(v.find("missing") == nullptr);
    CHECK(b->find("a") == nullptr);  // not an object
}

TEST_CASE("json: integers are exact over the whole i64 range") {
    CHECK(parse_json("9223372036854775807").value().as_i64() == std::numeric_limits<std::int64_t>::max());
    CHECK(parse_json("-9223372036854775808").value().as_i64() == std::numeric_limits<std::int64_t>::min());
    CHECK_FALSE(parse_json("9223372036854775808").value().as_i64().has_value());
    CHECK_FALSE(parse_json("-9223372036854775809").value().as_i64().has_value());
    CHECK_FALSE(parse_json("99999999999999999999999").value().as_i64().has_value());
    CHECK(parse_json("0").value().as_i64() == 0);
    CHECK(parse_json("-0").value().as_i64() == 0);
}

TEST_CASE("json: string escapes decode to UTF-8") {
    const auto r = parse_json(R"("a\"b\\c\/d\n\t\u0041\u00e9\u20ac\ud83d\ude00")");
    REQUIRE(r.has_value());
    CHECK(r.value().text == "a\"b\\c/d\n\tA\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80");
    // Raw UTF-8 passes through.
    CHECK(parse_json("\"\xD0\x9C\xD0\xBE\xD1\x81\xD0\xBA\xD0\xB2\xD0\xB0\"").value().text ==
          "\xD0\x9C\xD0\xBE\xD1\x81\xD0\xBA\xD0\xB2\xD0\xB0");
}

TEST_CASE("json: malformed input is rejected") {
    const char* bad[] = {
        "",
        "{",
        "[1,]",
        "{\"a\":1,}",
        "{\"a\" 1}",
        "{a:1}",
        "{\"a\":1,\"a\":2}",  // duplicate member
        "01",
        "1.",
        "1e",
        "-",
        "+1",
        "tru",
        "nul",
        "\"abc",
        "\"a\x01b\"",          // control character
        "\"\\x\"",             // unknown escape
        "\"\\u12\"",           // truncated escape
        "\"\\ud83d\"",         // lone high surrogate
        "\"\\ude00\"",         // lone low surrogate
        "\"\\ud83d\\u0041\"",  // high surrogate + non-surrogate
        "\"\xC3\"",            // truncated UTF-8
        "\"\xC0\xAF\"",        // overlong UTF-8
        "[1] 2",               // trailing data
        "NaN",
    };
    for (const char* text : bad) {
        CAPTURE(text);
        const auto r = parse_json(text);
        CHECK_FALSE(r.has_value());
        if (!r.has_value()) {
            CHECK(r.error().message.find("at byte") != std::string::npos);
        }
    }
}

TEST_CASE("json: nesting depth is limited") {
    // kMaxJsonDepth nested containers are fine, one more is not.
    std::string ok(pat::core::kMaxJsonDepth, '[');
    ok += std::string(pat::core::kMaxJsonDepth, ']');
    CHECK(parse_json(ok).has_value());
    std::string deep(pat::core::kMaxJsonDepth + 1, '[');
    deep += std::string(pat::core::kMaxJsonDepth + 1, ']');
    CHECK_FALSE(parse_json(deep).has_value());
}
