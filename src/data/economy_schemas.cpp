#include "pat/data/economy_schemas.hpp"

#include <cstdint>
#include <utility>

namespace pat::data {

namespace {

// Corridors; the same numbers as pat::sim::economy (money.hpp, rules.hpp), repeated here because the
// data module must not depend on sim. tests/economy_content_test.cpp pins the two together.
constexpr std::int64_t kPermilleOne = 1000;
constexpr std::int64_t kMaxMultiplier = 10000;
constexpr std::int64_t kMaxRatePpm = 1'000'000;
constexpr std::int64_t kMaxPostingKop = 10'000'000'000'000;
constexpr std::int64_t kMaxTreasuryKop = 1'000'000'000'000'000;

Type permille() {
    return Type::integer(0, kPermilleOne);
}
Type multiplier() {
    return Type::integer(0, kMaxMultiplier);
}
Type rate_ppm() {
    return Type::integer(0, kMaxRatePpm);
}
Type kopecks() {
    return Type::integer(0, kMaxPostingKop);
}
Type treasury_kopecks() {
    return Type::integer(0, kMaxTreasuryKop);
}
/// Mood or wealth points, -100...100, exact decimals allowed.
Type points() {
    return Type::fixed32(core::Fixed64::from_int(-100), core::Fixed64::from_int(100));
}

ObjectSchema population_block() {
    ObjectSchema s("population");
    s.describe_kind("Рост населения (формула F1, GDD 03 §4.2); ставки по группам — в `population_group`.");
    s.required("wealth_growth_ppm_per_point", Type::integer(0, 100000))
        .describe("Прибавка к годовому приросту (ppm в год) за каждый пункт достатка выше 100; ниже 100 "
                  "вычитается.")
        .required("capacity_per_development", Type::integer(1, 10'000'000))
        .describe("Вместимость провинции: людей на пункт развития; сверх неё прирост замедляется.");
    return s;
}

ObjectSchema production_block() {
    ObjectSchema s("production");
    s.describe_kind(
        "Труд и выпуск (формулы F2-F4, GDD 03 §5.2); цены — в `good`, доли владельца — в `province_kind`.");
    s.required("labour_min_permille", multiplier())
        .describe("Нижняя граница множителя труда, промилле (1000 = ×1,0).")
        .required("labour_max_permille", multiplier())
        .describe("Верхняя граница множителя труда, промилле; не меньше нижней.")
        .required("labour_mood_slope_permille", multiplier())
        .describe("Потеря труда за пункт отрицательного настроения крестьян, промилле на пункт.")
        .required("losses_permille", permille())
        .describe("Доля выпуска, теряемая на растраты и воровство (множитель `1 - losses`), промилле.");
    return s;
}

ObjectSchema consumption_block() {
    ObjectSchema s("consumption");
    s.describe_kind("Потребление, запас и достаток (формула F5, GDD 03 §5.3); нормы — в `good`, целевой "
                    "достаток — в `population_group`.");
    s.required("stock_cap_months", Type::integer(1, 120))
        .describe("Запас хлеба в провинции ограничен столькими месяцами потребности.")
        .required("wealth_inertia_permille", permille())
        .describe("Доля разрыва до целевого достатка, закрываемая за месяц, промилле.");
    return s;
}

ObjectSchema tax_block() {
    ObjectSchema s("tax");
    s.describe_kind("Налоги (формула F6, GDD 03 §3.2, §3.2a); ставки с головы — в `population_group`, уровни "
                    "— в `tax_level`.");
    s.required("law_permille", multiplier())
        .describe("Законодательный множитель сбора (реформы подушной подати и т. п.); в M1 равен 1000.")
        .required("low_control_tax_permille", permille())
        .describe("Множитель сбора, когда контроль ниже `control.low_threshold`, промилле.")
        .required("city_crown_share_permille", permille())
        .describe(
            "Доля налога посада в городе, уходящая короне, а не владельцу, промилле (вопрос Q6, #141).");
    return s;
}

ObjectSchema control_block() {
    ObjectSchema s("control");
    s.describe_kind("Контроль и коррупция (формула F7, GDD 03 §3.2, §7.4).");
    s.required("low_threshold", Type::integer(1, 100))
        .describe("Ниже этого контроля владелец собирает меньше, а коррупция растёт (в GDD 40).")
        .required("anarchy_threshold", Type::integer(0, 99))
        .describe("Ниже этого контроля провинция в смуте и владелец не получает ничего (в GDD 15); строго "
                  "меньше `low_threshold`.")
        .required("administrator_neutral", Type::integer(0, 100))
        .describe("Хозяйственность и честность управителя (0...100), при которых поправок нет.")
        .required("control_base", Type::integer(0, 100))
        .describe("Контроль, к которому провинция стремится без управителя и беспорядков.")
        .required("steward_gain_permille", multiplier())
        .describe("Очки контроля за пункт хозяйственности выше нейтральной, промилле очка.")
        .required("unrest_penalty_permille", multiplier())
        .describe("Очки контроля, теряемые за пункт беспорядков, промилле очка.")
        .required("control_inertia_permille", permille())
        .describe("Доля разрыва до целевого контроля, закрываемая за месяц, промилле.")
        .required("corruption_base_permille", permille())
        .describe("Базовая коррупция как доля собранного налога, промилле.")
        .required("integrity_gain_permille", multiplier())
        .describe("Промилле коррупции, снимаемые за пункт честности выше нейтральной.")
        .required("corruption_inertia_permille", permille())
        .describe("Доля разрыва до целевой коррупции, закрываемая за месяц, промилле.");
    return s;
}

ObjectSchema mood_block() {
    ObjectSchema s("mood");
    s.describe_kind(
        "Настроение групп (формула F8, GDD 03 §4.2); врождённое смещение — в `population_group`.");
    s.required("wealth_slope_permille", multiplier())
        .describe("Очки настроения за пункт достатка выше 100, промилле очка.")
        .required("admin_slope_permille", multiplier())
        .describe("Очки настроения за пункт контроля выше 50, промилле очка.")
        .required("inertia_permille", permille())
        .describe("Доля разрыва до целевого настроения, закрываемая за месяц, промилле.");
    return s;
}

ObjectSchema unrest_block() {
    ObjectSchema s("unrest");
    s.describe_kind("Беспорядки и бунты (формулы F9-F10, GDD 03 §4.3); вклад групп — в `population_group`.");
    s.required("slope_permille", multiplier())
        .describe("Очки беспорядков за пункт отрицательного настроения, промилле.")
        .required("control_slope_permille", multiplier())
        .describe("Очки беспорядков за пункт контроля ниже `control.low_threshold`, промилле.")
        .required("inertia_permille", permille())
        .describe("Доля разрыва до целевых беспорядков, закрываемая за месяц, промилле.")
        .required("calm_below", Type::integer(1, 100))
        .describe("Ниже этого значения провинция спокойна (в GDD 25).")
        .required("riot_from", Type::integer(1, 100))
        .describe("С этого значения начинаются бунты (в GDD 50); больше `calm_below`.")
        .required("uprising_from", Type::integer(1, 100))
        .describe("С этого значения начинается восстание (в GDD 75); больше `riot_from`, не больше 100.")
        .required("riot_chance_min_permille", permille())
        .describe("Месячный шанс бунта при `riot_from`, промилле.")
        .required("riot_chance_max_permille", permille())
        .describe(
            "Месячный шанс бунта при 100 беспорядков (между — линейно), промилле; не меньше минимального.")
        .required("riot_loot_permille", permille())
        .describe("Доля дохода прошлого месяца, которую разграбят при бунте, промилле.")
        .required("riot_control_loss", Type::integer(0, 100))
        .describe("Контроль, теряемый при бунте, в пунктах.");
    return s;
}

ObjectSchema recruit_block() {
    ObjectSchema s("recruit");
    s.describe_kind("Рекруты (формула F11, GDD 03 §10); доли по группам — в `population_group`, уровни — в "
                    "`recruit_level`.");
    s.required("regen_permille", permille())
        .describe("Доля разрыва до потенциала рекрутов, закрываемая за месяц, промилле.")
        .required("quota_penalty_honor", Type::integer(0, 1000))
        .describe("Потеря чести за невыполненную рекрутскую квоту (формула F14).");
    return s;
}

ObjectSchema loans_block() {
    ObjectSchema s("loans");
    s.describe_kind("Займы у казны (формулы F12-F14, GDD 03 §7.3). Принято на время (вопрос Q3, #138): "
                    "простой заём, лимит от чести, фиксированный процент.");
    s.required("rate_ppm_year", rate_ppm())
        .describe("Годовая ставка займа, ppm (100000 = 10 %).")
        .required("term_months", Type::integer(1, 600))
        .describe("Срок займа, месяцев.")
        .required("base_limit_kop", treasury_kopecks())
        .describe("Лимит займа рода без чести, копеек.")
        .required("per_honor_kop", kopecks())
        .describe("Прибавка к лимиту за пункт чести, копеек.")
        .required("bankruptcy_threshold_kop", Type::integer(1, kMaxTreasuryKop))
        .describe("Долг, с которого род считается банкротом, копеек.")
        .required("bankruptcy_exit_kop", treasury_kopecks())
        .describe("Долг, ниже которого банкротство снимается, копеек; строго меньше порога банкротства.")
        .required("max_loans_per_dynasty", Type::integer(0, 64))
        .describe("Наибольшее число одновременных займов рода.");
    return s;
}

ObjectSchema buildings_block() {
    ObjectSchema s("buildings");
    s.describe_kind("Общие правила строительства (формулы F15-F16); сами здания — вид `building`.");
    s.required("refund_permille", permille())
        .describe("Доля уплаченной цены, возвращаемая при отмене стройки, промилле.");
    return s;
}

ObjectSchema economy_rules_schema() {
    ObjectSchema s{std::string(kEconomyRulesKind)};
    s.describe_kind(
        "Константы правил экономики (E9, `docs/dev/economy-model.md` §9, §15). В контенте должен быть ровно "
        "один "
        "объект этого вида; мод заменяет его целиком, повторив тот же id. Все числа — предложения `[предл.]` "
        "(вопрос Q1, #136).");
    s.required("crown_account_mode", Type::enumeration({"separate", "emperor_dynasty"}))
        .describe("Как хранятся деньги короны (вопрос Q7, #142): `separate` — отдельный счёт, "
                  "`emperor_dynasty` — счёт рода императора.")
        .required("build_foreign_allowed", Type::boolean())
        .describe("Можно ли роду строить в чужой провинции (вопрос Q4, #139); `no` — только в своих.")
        .required("default_tax_level", Type::reference(std::string(kTaxLevelKind)))
        .describe("Уровень налога, с которого начинают провинции.")
        .required("default_recruit_level", Type::reference(std::string(kRecruitLevelKind)))
        .describe("Уровень набора, с которого начинают провинции.")
        .required("population", Type::block(population_block()))
        .describe("Рост населения.")
        .required("production", Type::block(production_block()))
        .describe("Труд и выпуск.")
        .required("consumption", Type::block(consumption_block()))
        .describe("Потребление, запас, достаток.")
        .required("tax", Type::block(tax_block()))
        .describe("Налоги.")
        .required("control", Type::block(control_block()))
        .describe("Контроль и коррупция.")
        .required("mood", Type::block(mood_block()))
        .describe("Настроение.")
        .required("unrest", Type::block(unrest_block()))
        .describe("Беспорядки и бунты.")
        .required("recruit", Type::block(recruit_block()))
        .describe("Рекруты.")
        .required("loans", Type::block(loans_block()))
        .describe("Займы.")
        .required("buildings", Type::block(buildings_block()))
        .describe("Общие правила строительства.");
    return s;
}

ObjectSchema good_schema() {
    ObjectSchema s{std::string(kGoodKind)};
    s.describe_kind("Товар (GDD 03 §5.1). Индекс `slot` стабилен и записывается в сохранения; порядок файлов "
                    "на него не влияет. "
                    "Слоты без объекта остаются нулевыми.");
    s.required("slot", Type::integer(0, 9))
        .describe("Стабильный индекс товара 0...9 (0 хлеб, 1 соль, 2 пушнина, 3 лес, 4 пенька/лён, 5 железо, "
                  "6 медь/серебро, 7 сукно, 8 рыба, 9 оружие); уникален.")
        .required("base_price_kop", kopecks())
        .describe("Статичная цена единицы товара в копейках (до цен торговых узлов, E19).")
        .optional("shortage_wealth_points", Type::integer(0, 300), TypedValue::of(std::int64_t{0}))
        .describe("Очки достатка, теряемые при полной нехватке товара.")
        .optional("need_per_head_milli", Type::map(Type::integer(0, 1'000'000)))
        .describe("Потребность одного человека группы в месяц, тысячные доли единицы: `группа = число`; "
                  "пропущенные группы не потребляют.");
    return s;
}

ObjectSchema population_group_schema() {
    ObjectSchema s{std::string(kPopulationGroupKind)};
    s.describe_kind("Группа населения (GDD 03 §4.1). Id задаёт группу и должен быть одним из: `nobility`, "
                    "`clergy`, `townsfolk`, "
                    "`peasants`, `cossacks`, `natives`; описаны должны быть все шесть.");
    s.required("birth_ppm_year", rate_ppm())
        .describe("Рождаемость, ppm размера группы в год.")
        .required("death_ppm_year", rate_ppm())
        .describe("Смертность, ppm размера группы в год.")
        .required("head_tax_kop_year", kopecks())
        .describe("Подать с головы в год, копеек; дворянство и духовенство платят службой, не деньгами (0).")
        .required("wealth_base", Type::integer(0, 300))
        .describe("Достаток, к которому группа стремится, когда ничего не нехватает (100 — норма).")
        .required("base_mood", points())
        .describe("Врождённое смещение настроения группы, пункты.")
        .required("unrest_influence_permille", multiplier())
        .describe("Вес группы в беспорядках провинции (умножается на её численность); хотя бы у одной группы "
                  "больше нуля.")
        .required("recruit_share_permille", permille())
        .describe("Доля численности группы, образующая потенциал рекрутов, промилле.");
    return s;
}

ObjectSchema province_kind_schema() {
    ObjectSchema s{std::string(kProvinceKindKind)};
    s.describe_kind("Тип провинции (GDD 03 §2). Id задаёт тип и должен быть одним из: `rural`, `city`, "
                    "`border`, `industrial`, "
                    "`port`, `steppe`, `northern`, `simplified`; описаны должны быть все восемь.");
    s.required("owner_share_permille", permille())
        .describe("Доля дохода владения, доходящая до владельца, промилле.");
    return s;
}

ObjectSchema level_schema(std::string_view kind, std::string_view multiplier_field, std::string_view what) {
    ObjectSchema s{std::string(kind)};
    s.describe_kind(std::string("Уровень политики: ") + std::string(what) +
                    " (GDD 03 §9). Индексы уровней идут подряд с 0 без пропусков, не более 8.");
    s.required("index", Type::integer(0, 7))
        .describe("Порядковый номер уровня (0 — самый мягкий); уникален.")
        .required(std::string(multiplier_field), multiplier())
        .describe("Множитель, который уровень даёт сбору, промилле (1000 = ×1,0).")
        .optional("mood_pressure", Type::map(points()))
        .describe("Давление на настроение групп, пункты: `группа = число`; пропущенные группы не затронуты.");
    return s;
}

ObjectSchema building_schema() {
    ObjectSchema s{std::string(kBuildingKind)};
    s.describe_kind("Тип здания (GDD 03 §8). Индекс `slot` стабилен и записывается в сохранения; порядок "
                    "файлов на него не "
                    "влияет. Зданий не больше 16.");
    s.required("slot", Type::integer(1, 255))
        .describe("Стабильный индекс здания 1...255 (0 значит «нет здания»); уникален.")
        .required("name_key", Type::string())
        .describe("Ключ строки локализации с названием здания.")
        .required("cost_kop", kopecks())
        .describe("Цена постройки, копеек.")
        .required("build_days", Type::integer(1, 3650))
        .describe("Срок постройки, дней.")
        .required("upkeep_kop_month", kopecks())
        .describe("Содержание в месяц, копеек.")
        .optional("output_permille", Type::map(Type::integer(0, kMaxMultiplier)))
        .describe("Множитель выпуска по товарам, промилле: `товар = число` (id объекта `good`); пропущенные "
                  "товары — 1000, то есть без эффекта.");
    return s;
}

}  // namespace

void add_economy_schemas(SchemaSet& set) {
    set.add(economy_rules_schema());
    set.add(good_schema());
    set.add(population_group_schema());
    set.add(province_kind_schema());
    set.add(level_schema(kTaxLevelKind, "tax_permille", "налог"));
    set.add(level_schema(kRecruitLevelKind, "recruit_permille", "набор рекрутов"));
    set.add(building_schema());
}

}  // namespace pat::data
