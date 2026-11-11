#include "pat/data/m1_schemas.hpp"

#include <string>

namespace pat::data {

namespace {

ObjectSchema province_schema() {
    ObjectSchema schema("province");
    schema.describe_kind(
        "Определение провинции M1. Id совпадает со стабильным ключом провинции карты; для PMD1 "
        "используются ключи p0001…pNNNN (решение #201). Владение и начальное состояние задаёт сценарий.");
    schema.required("name_key", Type::string())
        .describe("Ключ строки локализации с названием провинции.")
        .required("kind", Type::reference("province_kind"))
        .describe("Тип провинции из экономических определений.")
        .optional("owner", Type::reference("dynasty"))
        .describe("Необязательная ссылка на род-владельца для сценария; базовые определения владельцев "
                  "не назначают.");
    return schema;
}

ObjectSchema dynasty_schema() {
    ObjectSchema schema("dynasty");
    schema.describe_kind(
        "Определение рода: стабильный идентификатор и ключи локализации. Персонажи, глава, честь и "
        "начальные владения задаются сценарием, а не определением.");
    schema.required("name_key", Type::string())
        .describe("Ключ строки локализации с названием рода.")
        .optional("desc_key", Type::string())
        .describe("Необязательный ключ строки локализации с описанием рода.");
    return schema;
}

ObjectSchema skill_schema() {
    ObjectSchema schema("skill");
    schema.describe_kind(
        "Определение одного из пяти навыков (GDD 01 §2.1). Слот соответствует колонке skills[5] "
        "симуляции; уникальность слотов проверяет слой контента.");
    schema.required("name_key", Type::string())
        .describe("Ключ строки локализации с названием навыка.")
        .required("desc_key", Type::string())
        .describe("Ключ строки локализации с описанием навыка.")
        .required("slot", Type::integer(0, 4))
        .describe("Стабильный слот: 0 — stewardship, 1 — diplomacy, 2 — martial, 3 — intrigue, "
                  "4 — learning.");
    return schema;
}

ObjectSchema trait_schema() {
    ObjectSchema schema("trait");
    schema.describe_kind(
        "Черта персонажа (GDD 01 §4): категория, модификаторы и подсказки ИИ. Это определения; "
        "исполнение эффектов и правила назначения черт относятся к игровым системам.");
    schema.required("name_key", Type::string())
        .describe("Ключ строки локализации с названием черты.")
        .required("desc_key", Type::string())
        .describe("Ключ строки локализации с описанием черты.")
        .required("slot", Type::integer(1, 65535))
        .describe("Стабильный индекс черты в слоте u16 персонажа; 0 зарезервирован для пустого "
                  "слота. Уникальность индексов проверяет слой контента.")
        .required(
            "category",
            Type::enumeration({"personality", "education", "health", "life", "congenital", "religious"}))
        .describe("Категория: характер, образование, здоровье, жизненные обстоятельства, "
                  "врождённые или религиозные черты.")
        .optional("opposite", Type::reference("trait"))
        .describe("Взаимоисключающая черта. Согласованность противоположностей проверяет слой "
                  "контента.")
        .optional("skills", Type::map(Type::integer(-10, 10)))
        .describe("Изменения навыков в пунктах: ключ — id определения skill. Ключи проверяет слой "
                  "контента.")
        .optional("modifiers",
                  Type::map(Type::fixed32(core::Fixed64::from_int(-100), core::Fixed64::from_int(100))))
        .describe("Изменения шкал в пунктах; значения Fixed32. Применение модификаторов реализуют "
                  "игровые системы.")
        .optional("ai_hint", Type::map(Type::integer(-100, 100)))
        .describe("Целочисленные веса поведенческих подсказок ИИ; это данные, а не исполняемые "
                  "условия или команды.")
        .optional("inherit_chance",
                  Type::fixed32(core::Fixed64::zero(), core::Fixed64::one()),
                  TypedValue::of(core::Fixed32::zero()))
        .describe("Вероятность наследования в диапазоне 0…1; по умолчанию 0. Правила наследования "
                  "применяет система династии.");
    return schema;
}

ObjectSchema honor_rank_schema() {
    ObjectSchema schema{std::string(kHonorRankKind)};
    schema.describe_kind(
        "Ранг чести рода (E7-T14). Действует от своего порога включительно до следующего порога. "
        "Пороги уникальны, нижний порог равен −1000. В M1 ранг используется для отображения.");
    schema.required("min_honor", Type::integer(-1000, 10000))
        .describe("Минимальная честь для ранга, включительно.")
        .required("name_key", Type::string())
        .describe("Ключ строки локализации с названием ранга.");
    return schema;
}

}  // namespace

void add_m1_schemas(SchemaSet& schemas) {
    schemas.add(province_schema());
    schemas.add(dynasty_schema());
    schemas.add(trait_schema());
    schemas.add(skill_schema());
    schemas.add(honor_rank_schema());
}

}  // namespace pat::data
