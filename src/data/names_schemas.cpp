#include "pat/data/names_schemas.hpp"

#include <cstdint>

namespace pat::data {

namespace {

// Corridors; the same numbers as pat::sim (names.hpp), repeated here because the data module must not
// depend on sim. tests/names_content_test.cpp pins the two together.
constexpr std::int64_t kMaxNameWeight = 1000;
constexpr std::int64_t kMaxBonus = 100000;
constexpr std::int64_t kMaxPortraitId = 65535;

Type sex_type() {
    return Type::enumeration({"male", "female"});
}

ObjectSchema given_name_schema() {
    ObjectSchema s("given_name");
    s.describe_kind(
        "Имя из пула имён (GDD 01 §2.1). Id имени записывается в колонку `name` персонажа. Формы отчества "
        "записаны явно, без морфологического алгоритма: у мужских имён обе формы обязательны, у женских "
        "их нет (проверяет слой контента).");
    s.required("sex", sex_type())
        .describe("Пол носителя имени.")
        .required("weight", Type::integer(1, kMaxNameWeight))
        .describe("Относительная частота имени в пуле: вероятность выбора равна весу, делённому на сумму "
                  "весов имён этого пола.")
        .required("name_key", Type::string())
        .describe("Ключ строки локализации с самим именем («Пётр»).")
        .optional("patronymic_son_key", Type::string())
        .describe("Ключ отчества для сына человека с этим именем («Петрович»); только для мужских имён.")
        .optional("patronymic_daughter_key", Type::string())
        .describe("Ключ отчества для дочери человека с этим именем («Петровна»); только для мужских имён.");
    return s;
}

ObjectSchema surname_schema() {
    ObjectSchema s("surname");
    s.describe_kind(
        "Написание фамилии рода в мужской и женской форме. Не больше одного объекта на род; у каждого рода "
        "должен быть свой (проверяет слой контента).");
    s.required("dynasty", Type::reference("dynasty"))
        .describe("Род, чью фамилию описывает объект.")
        .required("male_key", Type::string())
        .describe("Ключ строки локализации с мужской формой («Меншиков»).")
        .required("female_key", Type::string())
        .describe("Ключ строки локализации с женской формой («Меншикова»).");
    return s;
}

ObjectSchema portrait_schema() {
    ObjectSchema s("portrait");
    s.describe_kind(
        "Портрет из пула: id, под которым гравюра хранится в колонке `portrait` персонажа, для пола и "
        "возрастной полосы. Сами изображения подключает эпик E26; здесь только распределение id.");
    s.required("sex", sex_type())
        .describe("Пол изображённого.")
        .required("stage", Type::enumeration({"child", "adolescent", "prime", "mature", "elder"}))
        .describe("Возрастная полоса (GDD 01 §3.1): ребёнок, подросток, зрелый возраст, средний возраст, "
                  "старость. Границы полос — в `demography_rules.life_stages`.")
        .required("portrait_id", Type::integer(1, kMaxPortraitId))
        .describe("Id портрета (колонка `portrait`, u16); 0 зарезервирован для «без портрета». "
                  "Уникальность проверяет слой контента.");
    return s;
}

ObjectSchema naming_rules_schema() {
    ObjectSchema s("naming_rules");
    s.describe_kind(
        "Правила выбора имени новорождённому. Вес имени отца и деда — прибавка к его обычному весу в пуле "
        "(в тех же единицах, что `given_name.weight`). Ровно один объект.");
    s.required("parent_bonus", Type::integer(0, kMaxBonus))
        .describe("Прибавка к весу имени родителя того же пола (отца для сына, матери для дочери); "
                  "0 отключает повторение имён.")
        .required("grandparent_bonus", Type::integer(0, kMaxBonus))
        .describe("Прибавка к весу имени деда или бабки по той же линии (отец отца, мать матери).");
    return s;
}

}  // namespace

void add_names_schemas(SchemaSet& set) {
    set.add(given_name_schema());
    set.add(surname_schema());
    set.add(portrait_schema());
    set.add(naming_rules_schema());
}

}  // namespace pat::data
