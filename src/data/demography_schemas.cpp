#include "pat/data/demography_schemas.hpp"

#include <cstdint>
#include <string>
#include <utility>

namespace pat::data {

namespace {

// Corridors; the same numbers as pat::sim (demography.hpp, demography.cpp), repeated here because the
// data module must not depend on sim. tests/demography_content_test.cpp pins the two together.
constexpr std::int64_t kPermilleOne = 1000;
constexpr std::int64_t kMaxMultiplier = 10000;
constexpr std::int64_t kMaxHealth = 100;
constexpr std::int64_t kMaxAge = 200;

Type permille() {
    return Type::integer(0, kPermilleOne);
}
Type multiplier() {
    return Type::integer(0, kMaxMultiplier);
}
Type signed_permille() {
    return Type::integer(-kPermilleOne, kPermilleOne);
}
Type age() {
    return Type::integer(0, kMaxAge);
}

/// One step of an age table: `{ from_age = 15, <value_field> = 800 }`.
ObjectSchema bucket_schema(std::string_view value_field, Type value_type, std::string value_text) {
    ObjectSchema s("age_bucket");
    s.describe_kind(
        "Ступень таблицы по возрасту: значение действует с `from_age` до начала следующей ступени.");
    s.required("from_age", age())
        .describe("Первый полный год жизни, с которого действует ступень; у первой ступени 0, дальше строго "
                  "по возрастанию.")
        .required(std::string(value_field), std::move(value_type))
        .describe(std::move(value_text));
    return s;
}

ObjectSchema mortality_block() {
    ObjectSchema s("mortality");
    s.describe_kind(
        "Естественная смертность (GDD 01 §3.2): годовой шанс смерти по возрасту, множители знати и "
        "здоровья. Пороги здоровья — в блоке `health`.");
    s.required("buckets",
               Type::list(Type::block(bucket_schema(
                   "permille", permille(), "Годовой шанс умереть в этом возрасте, промилле (220 = 22 %)."))))
        .describe(
            "Таблица базовой годовой смертности по возрасту: от 1 до 16 ступеней, первая с `from_age = 0`, "
            "возраст растёт строго.")
        .required("noble_child_age_limit", Type::integer(0, kMaxAge))
        .describe("Знатные (персонажи с полной детализацией) моложе этого возраста получают детский "
                  "множитель ниже (по умолчанию 15: детские ступени 0-14).")
        .required("noble_child_multiplier_permille", multiplier())
        .describe("Множитель смертности знатных детей, промилле (800 = ×0,8: уход и лекари, "
                  "решение владельца #134 «мягкий реализм»).")
        .required("low_health_multiplier_permille", multiplier())
        .describe("Множитель смертности при здоровье ниже `health.low_below`, промилле (2000 = ×2,0).")
        .required("high_health_multiplier_permille", multiplier())
        .describe("Множитель смертности при здоровье не ниже `health.high_from`, промилле (750 = ×0,75).");
    return s;
}

ObjectSchema health_block() {
    ObjectSchema s("health");
    s.describe_kind("Пороги здоровья (0...100), общие для смертности, зачатия и родов.");
    s.required("low_below", Type::integer(0, kMaxHealth))
        .describe("Здоровье ниже этого значения считается слабым (в GDD 30).")
        .required("high_from", Type::integer(0, kMaxHealth))
        .describe("Здоровье от этого значения считается крепким (в GDD 80); строго больше `low_below`.");
    return s;
}

ObjectSchema conception_block() {
    ObjectSchema s("conception");
    s.describe_kind("Зачатие (GDD 01 §3.3): месячный шанс = база × возраст жены × здоровье × черты × "
                    "совместное проживание; число детей ограничено потолком.");
    s.required("base_permille", permille())
        .describe("Базовый месячный шанс зачатия пары, промилле (12 = 1,2 % в месяц).")
        .required(
            "age_factor",
            Type::list(Type::block(bucket_schema(
                "factor_permille", multiplier(), "Множитель шанса для жены этого возраста, промилле."))))
        .describe("Таблица множителя шанса по возрасту жены: от 1 до 16 ступеней, первая с `from_age = 0`, "
                  "возраст растёт строго; множитель 0 означает, что зачатие невозможно.")
        .required("low_health_factor_permille", multiplier())
        .describe("Множитель шанса при слабом здоровье жены, промилле (в GDD не задан, заготовка 500).")
        .required("high_health_factor_permille", multiplier())
        .describe("Множитель шанса при крепком здоровье жены, промилле (в GDD не задан, заготовка 1100).")
        .required("max_children_per_woman", Type::integer(1, 100))
        .describe("Потолок числа детей одной женщины (в GDD около 14); после него зачатие невозможно.")
        .required("pregnancy_months", Type::integer(1, 24))
        .describe("Длительность беременности, месяцев.");
    return s;
}

ObjectSchema birth_block() {
    ObjectSchema s("birth");
    s.describe_kind("Роды (GDD 01 §3.3): соотношение полов и риск смерти матери.");
    s.required("boys_per_100_girls", Type::integer(1, 10000))
        .describe("Сколько мальчиков рождается на 100 девочек (в GDD 105).")
        .required("risk_base_permille", permille())
        .describe("Базовый риск смерти матери при родах, промилле (50 = 5 %).")
        .required("risk_high_health_delta_permille", signed_permille())
        .describe("Поправка к риску при крепком здоровье матери, промилле (отрицательная снижает риск).")
        .required("risk_low_health_delta_permille", signed_permille())
        .describe("Поправка к риску при слабом здоровье матери, промилле (положительная повышает риск).")
        .required("risk_physician_delta_permille", signed_permille())
        .describe("Поправка к риску, если при дворе есть лекарь, промилле. База с поправками остаётся в "
                  "пределах 0...1000.");
    return s;
}

ObjectSchema life_stages_block() {
    ObjectSchema s("life_stages");
    s.describe_kind(
        "Возрастные периоды (GDD 01 §3.1): первый год каждого периода и возраст вступления в брак.");
    s.required("adolescent_from", Type::integer(1, kMaxAge))
        .describe("С какого возраста начинается отрочество (в GDD 7); дальше возраст периодов строго растёт.")
        .required("prime_from", Type::integer(1, kMaxAge))
        .describe("С какого возраста начинается расцвет сил (в GDD 16); больше `adolescent_from`.")
        .required("mature_from", Type::integer(1, kMaxAge))
        .describe("С какого возраста начинается зрелость (в GDD 51); больше `prime_from`.")
        .required("elder_from", Type::integer(1, kMaxAge))
        .describe("С какого возраста начинается старость (в GDD 66); больше `mature_from`.")
        .required("marriage_age_male", Type::integer(0, kMaxAge))
        .describe("Наименьший возраст жениха (в GDD 16); не меньше `adolescent_from` и меньше `mature_from`.")
        .required("marriage_age_female", Type::integer(0, kMaxAge))
        .describe("Наименьший возраст невесты (в GDD 15); не меньше `adolescent_from` и меньше "
                  "`mature_from`.");
    return s;
}

ObjectSchema demography_rules_schema() {
    ObjectSchema s{std::string(kDemographyRulesKind)};
    s.describe_kind(
        "Баланс демографии рода (E7, `docs/gdd/01-dynasty.md` §3): смертность, зачатие, роды, возрастные "
        "периоды. В контенте должен быть ровно один объект этого вида; мод заменяет его целиком, повторив "
        "тот "
        "же id. Значения по умолчанию в `data/balance/demography.pd` равны запасным `kDefaultDemography`. "
        "Все "
        "числа — предложения `[предл.]`; калибровка допусков вымирания — задача E7-T19.");
    s.required("mortality", Type::block(mortality_block()))
        .describe("Смертность.")
        .required("health", Type::block(health_block()))
        .describe("Пороги здоровья.")
        .required("conception", Type::block(conception_block()))
        .describe("Зачатие и потолок детей.")
        .required("birth", Type::block(birth_block()))
        .describe("Роды: полы и риск для матери.")
        .required("life_stages", Type::block(life_stages_block()))
        .describe("Возрастные периоды и брачный возраст.");
    return s;
}

}  // namespace

void add_demography_schemas(SchemaSet& set) {
    set.add(demography_rules_schema());
}

}  // namespace pat::data
