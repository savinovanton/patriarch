// Shared typed accessors of the content builders (src/content/typed_accessors.hpp, D10).

#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <utility>

#include "typed_accessors.hpp"

using namespace pat;
using pat::data::ContentObject;
using pat::data::SourcePos;
using pat::data::TypedField;
using pat::data::TypedValue;

namespace {

void add(ContentObject& object, const char* name, SourcePos pos, TypedValue value) {
    TypedField field;
    field.name = name;
    field.pos = pos;
    field.value = std::move(value);
    object.object.body.fields.push_back(std::move(field));
}

ContentObject make_object() {
    ContentObject object;
    object.object.pos = SourcePos{1, 1};
    add(object, "text", SourcePos{2, 3}, TypedValue::of(std::string("hello")));
    add(object, "count", SourcePos{3, 3}, TypedValue::of(std::int64_t{-7}));
    add(object, "kind", SourcePos{4, 3}, TypedValue::of(data::EnumValue{"female", 1}));
    return object;
}

}  // namespace

TEST_CASE("typed accessors read present fields of a content object") {
    const ContentObject object = make_object();
    CHECK(content::detail::text_of(object, "text") == "hello");
    CHECK(content::detail::i32_of(object, "count") == -7);
    CHECK(content::detail::i32_of(object, "count", 99) == -7);
    CHECK(content::detail::enum_index_of(object, "kind") == 1);
    CHECK(content::detail::has_field(object, "text"));
    CHECK(content::detail::field_pos(object, "count") == SourcePos{3, 3});
}

TEST_CASE("typed accessors fall back for absent or mistyped fields") {
    const ContentObject object = make_object();
    CHECK(content::detail::text_of(object, "missing").empty());
    CHECK(content::detail::text_of(object, "count").empty());
    CHECK(content::detail::i32_of(object, "missing") == 0);
    CHECK(content::detail::i32_of(object, "missing", 100) == 100);
    CHECK(content::detail::i32_of(object, "text", 5) == 5);
    CHECK(content::detail::enum_index_of(object, "missing") == 0);
    CHECK(content::detail::enum_index_of(object, "text") == 0);
    CHECK_FALSE(content::detail::has_field(object, "missing"));
    CHECK(content::detail::field_pos(object, "missing") == SourcePos{1, 1});
}

TEST_CASE("typed block accessors keep their behaviour") {
    const ContentObject object = make_object();
    CHECK(content::detail::i32_of(object.object.body, "count") == -7);
    CHECK(content::detail::i32_of(object.object.body, "text") == 0);
    CHECK(content::detail::key_pos(object.object.body, "kind", SourcePos{9, 9}) == SourcePos{4, 3});
    CHECK(content::detail::key_pos(object.object.body, "none", SourcePos{9, 9}) == SourcePos{9, 9});
}
