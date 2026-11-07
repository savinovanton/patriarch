#include "pat/data/example_schemas.hpp"

#include <utility>

namespace pat::data {

ObjectSchema make_example_trait_schema() {
    ObjectSchema chance("chance");
    chance.required("base", Type::fixed64(core::Fixed64::zero(), core::Fixed64::one()))
        .describe("Базовая вероятность.")
        .optional("max",
                  Type::fixed64(core::Fixed64::zero(), core::Fixed64::one()),
                  TypedValue::of(core::Fixed64::one()))
        .describe("Верхняя граница вероятности.");

    ObjectSchema trait("trait");
    trait.describe_kind(
        "Черта характера (пример схемы; реальные виды контента добавляются в content_schemas).");
    trait.required("name_key", Type::string())
        .describe("Ключ строки локализации с названием черты.")
        .required("cost", Type::integer(0, 10))
        .describe("Стоимость черты в очках.")
        .optional("group",
                  Type::enumeration({"vice", "virtue", "neutral"}),
                  TypedValue::of(EnumValue{"neutral", 2}))
        .describe("Нравственная группа черты.")
        .optional("opposite", Type::reference("trait"))
        .describe("Взаимоисключающая черта.")
        .optional("weight",
                  Type::fixed32(core::Fixed64::zero(), core::Fixed64::from_int(100)),
                  TypedValue::of(core::Fixed32::one()))
        .describe("Относительный вес при случайном выборе.")
        .optional("inheritable", Type::boolean(), TypedValue::of(false))
        .describe("Передаётся ли черта потомкам.")
        .optional("since", Type::date())
        .describe("Дата (юлианская), с которой черта доступна.")
        .optional("tags", Type::list(Type::string()))
        .describe("Произвольные метки для фильтров.")
        .optional("skills", Type::map(Type::integer(-10, 10)))
        .describe("Модификаторы навыков: идентификатор навыка = значение.")
        .repeatable("note", Type::string())
        .describe("Заметка автора; ключ можно повторять.")
        .optional("chance", Type::block(std::move(chance)))
        .describe("Вложенный блок с вероятностями.");
    return trait;
}

SchemaSet make_example_schema_set() {
    SchemaSet set;
    set.add(make_example_trait_schema());
    return set;
}

}  // namespace pat::data
