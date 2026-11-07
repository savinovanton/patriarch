#include "pat/data/content_schemas.hpp"
#include "pat/data/example_schemas.hpp"
#include "pat/data/schema.hpp"
#include "pat/data/schema_doc.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

using namespace pat::data;
using pat::core::Fixed32;
using pat::core::Fixed64;

namespace {

bool contains(const std::string& text, std::string_view needle) {
    return text.find(needle) != std::string::npos;
}

std::string without_cr(std::string text) {
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    return text;
}

/// A schema that uses every field type and every kind of default.
SchemaSet all_types_set() {
    ObjectSchema inner("inner");
    inner.required("weight", Type::fixed32(Fixed64::from_int(-5), std::nullopt)).describe("Inner | weight.");

    ObjectSchema everything("everything");
    everything.describe_kind("Every type.");
    everything.required("title", Type::string())
        .optional("count", Type::integer(1, std::nullopt), TypedValue::of(std::int64_t{3}))
        .optional("level", Type::integer(std::nullopt, 9))
        .optional("ratio32", Type::fixed32(), TypedValue::of(Fixed32::from_int(2)))
        .optional("ratio64", Type::fixed64(Fixed64::zero(), Fixed64::one()))
        .optional("flag", Type::boolean(), TypedValue::of(true))
        .optional("mood", Type::enumeration({"calm", "angry"}), TypedValue::of(EnumValue{"angry", 1}))
        .optional("friend", Type::reference("person"), TypedValue::of(RefValue{"person", "ivan"}))
        .optional("born", Type::date(), TypedValue::of(DateValue{1703, 5, 7}))
        .optional("friends", Type::list(Type::reference("person")))
        .optional("scores", Type::map(Type::integer(0, 100)))
        .repeatable("remark", Type::string())
        .optional("parts", Type::list(Type::block(std::move(inner))));

    ObjectSchema aaa("aaa");
    SchemaSet set;
    set.add(std::move(everything));
    set.add(std::move(aaa));
    return set;
}

const char* const kExampleBody = R"DOC(## Виды объектов

- `trait` — Черта характера (пример схемы; реальные виды контента добавляются в content_schemas).

## Вид `trait`

Черта характера (пример схемы; реальные виды контента добавляются в content_schemas).

Объект: `trait "<id>" { … }`. Метка (id) обязательна и уникальна в пределах вида (`PD109`, `PD112`).

| Поле | Тип | Обязательность | По умолчанию | Ограничения | Описание |
|---|---|---|---|---|---|
| `name_key` | `string` | обязательное | — | — | Ключ строки локализации с названием черты. |
| `cost` | `int` | обязательное | — | диапазон 0..10 | Стоимость черты в очках. |
| `group` | `enum` | необязательное | `neutral` | значения: `vice`, `virtue`, `neutral` | Нравственная группа черты. |
| `opposite` | `ref(trait)` | необязательное | — | вид: `trait` | Взаимоисключающая черта. |
| `weight` | `fixed32` | необязательное | `1` | диапазон 0..100 | Относительный вес при случайном выборе. |
| `inheritable` | `bool` | необязательное | `no` | — | Передаётся ли черта потомкам. |
| `since` | `date` | необязательное | — | — | Дата (юлианская), с которой черта доступна. |
| `tags` | `list of string` | необязательное | — | — | Произвольные метки для фильтров. |
| `skills` | `map of int` | необязательное | — | значения: диапазон -10..10 | Модификаторы навыков: идентификатор навыка = значение. |
| `note` | `string` | необязательное, повторяемое | — | — | Заметка автора; ключ можно повторять. |
| `chance` | `block` (см. `trait.chance`) | необязательное | — | — | Вложенный блок с вероятностями. |

### Блок `trait.chance`

Вложенный блок: `chance { … }` или `chance = { … }`; метка не допускается (`PD109`).

| Поле | Тип | Обязательность | По умолчанию | Ограничения | Описание |
|---|---|---|---|---|---|
| `base` | `fixed64` | обязательное | — | диапазон 0..1 | Базовая вероятность. |
| `max` | `fixed64` | необязательное | `1` | диапазон 0..1 | Верхняя граница вероятности. |
)DOC";

}  // namespace

TEST_CASE("schema_doc: description API is optional and fluent") {
    ObjectSchema s("k");
    s.required("a", Type::string()).optional("b", Type::boolean()).describe("About b.");
    s.describe_kind("About k.");
    CHECK(s.description() == "About k.");
    CHECK(s.fields()[0].description.empty());
    CHECK(s.fields()[1].description == "About b.");
    CHECK(s.fields()[0].required);
    CHECK(!s.fields()[1].required);
}

TEST_CASE("schema_doc: reference body of the example schemas matches the golden text") {
    const std::string body = render_reference_body(make_example_schema_set());
    CHECK(body == kExampleBody);
}

TEST_CASE("schema_doc: output is deterministic, LF only, no tabs, ends with one newline") {
    const SchemaSet set = all_types_set();
    const std::string a = render_reference_document(set);
    const std::string b = render_reference_document(set);
    CHECK(a == b);
    CHECK(!contains(a, "\r"));
    CHECK(!contains(a, "\t"));
    REQUIRE(a.size() > 2);
    CHECK(a.back() == '\n');
    CHECK(a[a.size() - 2] != '\n');
}

TEST_CASE("schema_doc: every field type is documented") {
    const std::string body = render_reference_body(all_types_set());
    CHECK(contains(body, "| `title` | `string` | обязательное | — | — | — |"));
    CHECK(contains(body, "| `count` | `int` | необязательное | `3` | диапазон ≥ 1 | — |"));
    CHECK(contains(body, "| `level` | `int` | необязательное | — | диапазон ≤ 9 | — |"));
    CHECK(contains(body, "| `ratio32` | `fixed32` | необязательное | `2` | — | — |"));
    CHECK(contains(body, "| `ratio64` | `fixed64` | необязательное | — | диапазон 0..1 | — |"));
    CHECK(contains(body, "| `flag` | `bool` | необязательное | `yes` | — | — |"));
    CHECK(contains(body, "| `mood` | `enum` | необязательное | `angry` | значения: `calm`, `angry` | — |"));
    CHECK(
        contains(body, "| `friend` | `ref(person)` | необязательное | `person:ivan` | вид: `person` | — |"));
    CHECK(contains(body, "| `born` | `date` | необязательное | `1703.05.07` | — | — |"));
    CHECK(contains(
        body, "| `friends` | `list of ref(person)` | необязательное | — | элементы: вид: `person` | — |"));
    CHECK(contains(body, "| `scores` | `map of int` | необязательное | — | значения: диапазон 0..100 | — |"));
    CHECK(contains(body, "| `remark` | `string` | необязательное, повторяемое | — | — | — |"));
    CHECK(contains(body,
                   "| `parts` | `list of block` (см. `everything.parts`) | необязательное | — | — | — |"));
    // Nested block section with a pipe escaped in the description and a lower bound only.
    CHECK(contains(body, "### Блок `everything.parts`"));
    CHECK(contains(body, "| `weight` | `fixed32` | обязательное | — | диапазон ≥ -5 | Inner \\| weight. |"));
}

TEST_CASE("schema_doc: kinds are sorted by name, empty set is handled") {
    const std::string body = render_reference_body(all_types_set());
    const std::size_t aaa = body.find("## Вид `aaa`");
    const std::size_t everything = body.find("## Вид `everything`");
    REQUIRE(aaa != std::string::npos);
    REQUIRE(everything != std::string::npos);
    CHECK(aaa < everything);
    CHECK(contains(body, "Полей нет."));

    const std::string empty = render_reference_body(SchemaSet{});
    CHECK(contains(empty, "Схемы не зарегистрированы."));
}

TEST_CASE("schema_doc: fixed-point limits print exactly (no float artefacts)") {
    ObjectSchema s("num");
    s.optional("a", Type::fixed64(Fixed64::from_ratio(1, 20), Fixed64::from_ratio(-1, 2)))
        .optional("b", Type::fixed32(Fixed64::from_ratio(3, 8), Fixed64::from_ratio(1, 3)));
    SchemaSet set;
    set.add(std::move(s));
    const std::string body = render_reference_body(set);
    CHECK(contains(body, "диапазон 0.05..-0.5"));
    CHECK(contains(body, "диапазон 0.375..0.333333"));
}

TEST_CASE("schema_doc: content_schemas() is documented and the committed reference is up to date") {
    const SchemaSet schemas = content_schemas();
    CHECK(schemas.find("trait") != nullptr);

    const std::string path = std::string(PATRIARCH_SOURCE_DIR) + "/docs/dev/data-reference.md";
    std::ifstream in(path, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), "cannot open " << path);
    const std::string committed((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    // Not up to date? Run: schemadoc --write docs/dev/data-reference.md
    CHECK_MESSAGE(without_cr(committed) == render_reference_document(schemas),
                  "docs/dev/data-reference.md is stale; run `schemadoc --write docs/dev/data-reference.md`");
}
