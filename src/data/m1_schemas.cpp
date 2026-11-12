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
        .describe("Изменения шкал в пунктах; значения Fixed32. Ключи: health, fertility, stress, piety, "
                  "ambition, loyalty_head, loyalty_emperor.")
        .optional("health_multiplier_permille", Type::integer(0, 10000))
        .describe("Множитель эффективного здоровья в промилле; при отсутствии 1000.")
        .optional("fertility_multiplier_permille", Type::integer(0, 10000))
        .describe("Множитель эффективной фертильности в промилле; при отсутствии 1000.")
        .optional("mortality_multiplier_permille", Type::integer(0, 10000))
        .describe("Множитель риска смерти в промилле; при отсутствии 1000.")
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

ObjectSchema holding_schema() {
    ObjectSchema schema{std::string(kHoldingKind)};
    schema.describe_kind("Начальное хозяйство сценария M1; баланс и владение — тестовые значения [предл.].");
    schema.required("province", Type::reference("province"))
        .describe("Провинция, для которой задаётся начальное хозяйство.")
        .required("kind", Type::reference("province_kind"))
        .describe("Вид провинции в этом сценарии; переопределяет базовое определение.")
        .required("owner_kind", Type::enumeration({"unowned", "dynasty", "crown", "church", "free"}))
        .describe("Вид начального владельца; dynasty требует поле owner.")
        .optional("owner", Type::reference("dynasty"))
        .describe("Род-владелец; допустим только для owner_kind = dynasty.")
        .required("trade_node", Type::reference(std::string(kTradeNodeKind)))
        .describe("Торговый узел; пока задаёт только группировку провинций для E19.")
        .required("development", Type::integer(1, 10))
        .describe("Начальное развитие провинции, 1…10.")
        .required("slots_total", Type::integer(0, 6))
        .describe("Число строительных слотов, 0…6.")
        .required("population", Type::map(Type::integer(0, 32'000'000)))
        .describe("Люди по ключам population_group; общая численность не больше 32 000 000.")
        .required("capacity", Type::map(Type::fixed32(core::Fixed64::zero(), core::Fixed64::from_int(32767))))
        .describe("Потенциал выпуска по ключам good, 0…32767.")
        .optional("mood",
                  Type::map(Type::fixed32(core::Fixed64::from_int(-100), core::Fixed64::from_int(100))))
        .describe("Начальное настроение по группам; пропущенная группа равна 0.")
        .optional("wealth", Type::map(Type::fixed32(core::Fixed64::zero(), core::Fixed64::from_int(300))))
        .describe("Начальный достаток по группам; пропущенная группа равна 100.")
        .optional("control",
                  Type::fixed32(core::Fixed64::zero(), core::Fixed64::from_int(100)),
                  TypedValue::of(core::Fixed32::from_int(100)))
        .describe("Начальный контроль; при отсутствии 100.")
        .optional("corruption",
                  Type::fixed32(core::Fixed64::zero(), core::Fixed64::one()),
                  TypedValue::of(core::Fixed32::zero()))
        .describe("Начальная коррупция, 0…1.")
        .optional("unrest",
                  Type::fixed32(core::Fixed64::zero(), core::Fixed64::from_int(100)),
                  TypedValue::of(core::Fixed32::zero()))
        .describe("Начальная смута, 0…100.")
        .optional("grain_stock",
                  Type::fixed32(core::Fixed64::zero(), core::Fixed64::from_int(32767)),
                  TypedValue::of(core::Fixed32::zero()))
        .describe("Начальный запас хлеба, единиц товара.")
        .optional("buildings", Type::list(Type::reference("building")))
        .describe("Готовые здания в порядке строительных слотов; соблюдают требования к месту.");
    return schema;
}

ObjectSchema trade_node_schema() {
    ObjectSchema schema{std::string(kTradeNodeKind)};
    schema.describe_kind("Группа провинций M1; торговая симуляция относится к E19.");
    schema.required("slot", Type::integer(1, 65535))
        .describe("Стабильный числовой id узла в состоянии провинции; уникален, 0 зарезервирован.")
        .required("name_key", Type::string())
        .describe("Ключ названия узла в локализации.");
    return schema;
}

ObjectSchema economy_start_schema() {
    ObjectSchema schema{std::string(kEconomyStartKind)};
    schema.describe_kind("Начальные казны сценария M1. Один объект; владельцы и числа тестовые [предл.].");
    schema.required("crown_treasury_kop", Type::integer(0, 1'000'000'000'000'000))
        .describe("Начальная казна короны, копеек.")
        .required("dynasty_treasuries", Type::map(Type::integer(0, 1'000'000'000'000'000)))
        .describe("Начальные казны по id родов; не перечисленные роды начинают с нулём.");
    return schema;
}

}  // namespace

void add_m1_schemas(SchemaSet& schemas) {
    schemas.add(province_schema());
    schemas.add(dynasty_schema());
    schemas.add(trait_schema());
    schemas.add(skill_schema());
    schemas.add(honor_rank_schema());
    schemas.add(holding_schema());
    schemas.add(trade_node_schema());
    schemas.add(economy_start_schema());
}

}  // namespace pat::data
