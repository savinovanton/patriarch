#include "pat/data/court_schemas.hpp"

#include <cstdint>
#include <limits>
#include <string>
#include <utility>

namespace pat::data {

namespace {

// Only representation limits belong here; sim::validate(CourtRules) owns the corridors.
Type i32() {
    return Type::integer(std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max());
}
Type i64() {
    return Type::integer(std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::max());
}

// clang-format off
const std::vector<CourtBalanceField> kBalanceFields{
    {"limits", "max_petitions_per_dynasty", false, "Сколько челобитных на должности род может держать одновременно."},
    {"limits", "max_offices_per_person", false, "Сколько должностей может занимать один человек (фаворит не считается)."},
    {"limits", "gift_max_kop", true, "Наибольший подарок к челобитной, копеек."},
    {"limits", "disgrace_max_days", false, "Наибольший срок опалы, дней."},
    {"limits", "retire_age", false, "Возраст ухода слуг на покой, лет."},
    {"limits", "quit_after_months", false, "Сколько месяцев просрочки жалованья терпит слуга, прежде чем уйти."},
    {"decision", "candidate_prefilter", false, "Сколько лучших кандидатов отбирается для собственного выбора государя."},
    {"decision", "min_utility", false, "Наименьшая полезность победителя; ниже — решение откладывается."},
    {"decision", "max_postponements", false, "Сколько раз можно отложить решение о вакансии."},
    {"decision", "postpone_days", false, "На сколько дней откладывается решение."},
    {"decision", "relax_after_days", false, "Через сколько дней вакансии допускаются мягкие отклонения («исключительный порядок»)."},
    {"decision", "self_nomination_min_favor", false, "Ниже этой милости государь не назначает по собственному выбору человека из рода игрока."},
    {"utility", "favor_weight_permille", false, "Вес милости рода в слагаемом «расположение», промилле."},
    {"utility", "opinion_weight_permille", false, "Вес мнения государя о кандидате, промилле."},
    {"utility", "suspicion_weight_permille", false, "Вес подозрения (E12) в слагаемом «расположение», промилле."},
    {"utility", "honor_per_point", false, "Сколько чести рода даёт одно очко полезности."},
    {"utility", "rank_cap", false, "Потолок слагаемого «честь», очков."},
    {"utility", "party_balance_points", false, "Размах слагаемого «баланс партий», очков."},
    {"utility", "gift_unit_kop", true, "Сколько копеек подарка дают одно очко полезности."},
    {"utility", "gift_cap", false, "Потолок слагаемого «подарок», очков."},
    {"utility", "service_points", false, "Очки полезности за услугу."},
    {"utility", "party_support_points", false, "Очки полезности за поддержку партии (при полной сплочённости)."},
    {"utility", "favourite_points", false, "Очки полезности, если подающий род держит место фаворита."},
    {"utility", "exceptional_penalty", false, "Штраф полезности за назначение в исключительном порядке, очков."},
    {"utility", "exceptional_max_honor_gap", false, "Наибольший недобор чести при исключительном порядке."},
    {"utility", "exceptional_max_skill_gap", false, "Наибольший недобор по каждому навыку при исключительном порядке."},
    {"support", "services_max", false, "Наибольший запас услуг у рода."},
    {"support", "party_support_cooldown_days", false, "Не чаще раза в столько дней род может использовать поддержку партии."},
    {"favor", "favor_base_x100", false, "Милость, к которой род тянется без влияний, сотые доли очка."},
    {"favor", "opinion_to_favor_x100", false, "Сдвиг базы милости за очко мнения государя о главе рода, сотые доли."},
    {"favor", "suspicion_to_favor_x100", false, "Потеря милости за очко подозрения (E12), сотые доли."},
    {"favor", "pull_permille", false, "Доля разрыва до базы, закрываемая за месяц, промилле."},
    {"favor", "service_per_office_x100", false, "Прибавка милости за месяц за каждое держание рода, сотые доли."},
    {"favor", "service_cap_x100", false, "Потолок прибавки за службу, сотые доли."},
    {"favor", "quota_met_x100", false, "Прибавка за месяц при выполненной квоте, сотые доли."},
    {"favor", "quota_short_x100", false, "Потеря за месяц при невыполненной квоте, сотые доли."},
    {"favor", "favourite_monthly_x100", false, "Прибавка за месяц роду фаворита, сотые доли."},
    {"favor", "arrears_favor_x100", false, "Потеря за месяц, пока у рода есть слуги в просрочке жалованья, сотые доли."},
    {"favor", "disgrace_favor_x100", false, "Потеря за месяц за каждого члена рода в опале, сотые доли."},
    {"favor", "favor_month_cap_x100", false, "Предел месячного изменения милости, сотые доли."},
    {"favor", "favor_low_x100", false, "Порог «милость низкая» для событий, сотые доли; строго ниже порога «высокая»."},
    {"favor", "favor_high_x100", false, "Порог «милость высокая» для событий, сотые доли."},
    {"favor", "favor_on_grant_x100", false, "Прибавка милости роду при назначении его кандидата, сотые доли."},
    {"favor", "gift_per_100_rub_x100", false, "Милость за 100 рублей подарка императору, сотые доли."},
    {"favor", "gift_month_cap_x100", false, "Потолок милости от подарков за месяц, сотые доли."},
    {"opinion", "reject_opinion", false, "Обида отвергнутого кандидата на императора, очков мнения."},
    {"opinion", "grant_opinion", false, "Благодарность назначенного кандидата, очков мнения."},
    {"opinion", "dismiss_opinion", false, "Обида снятого с должности, очков мнения."},
    {"opinion", "disgrace_one_off_x100", false, "Единовременная потеря милости рода при опале, сотые доли."},
    {"servants", "loyalty_base", false, "Лояльность, к которой слуга тянется, очков."},
    {"servants", "loyalty_pull_permille", false, "Доля разрыва до базы, закрываемая за месяц, промилле."},
    {"servants", "paid_bonus", false, "Прибавка лояльности за месяц без просрочки жалованья."},
    {"servants", "arrears_loss", false, "Потеря лояльности за месяц просрочки."},
    {"servants", "unreliable_below", false, "Ниже этой лояльности слуга считается ненадёжным."},
    {"servants", "betrayal_base_permille", false, "Базовая вероятность предательства (задел E14), промилле."},
    {"servants", "betrayal_per_gap_permille", false, "Прибавка вероятности за пункт нехватки лояльности до 50, промилле."},
    {"servants", "betrayal_bribe_unit_kop", true, "Единица суммы подкупа для вероятности предательства, копеек."},
    {"servants", "betrayal_per_unit_permille", false, "Прибавка вероятности за единицу подкупа, промилле."},
    {"servants", "betrayal_per_secrecy_permille", false, "Снижение вероятности за пункт скрытности, промилле."},
    {"servants", "steward_control_per_skill_x100", false, "Очки контроля за пункт хозяйственности управляющего выше 10, сотые доли."},
    {"servants", "steward_corruption_per_skill_permille", false, "Снижение коррупции за пункт хозяйственности управляющего выше 10, промилле."},
    {"parties", "party_base_weight", false, "Вес рода в партии без учёта чести."},
    {"parties", "party_honor_cap", false, "Честь рода, выше которой вес не растёт."},
    {"parties", "party_honor_div", false, "Делитель чести в весе рода."},
    {"parties", "cohesion_pair_cap", false, "Наибольшее число пар родов в расчёте сплочённости партии."},
    {"ai", "ai_min_utility", false, "Наименьшая полезность, при которой род ИИ подаёт челобитную."},
    {"ai", "ai_gift_permille", false, "Доля казны, которую ИИ готов потратить на подарок, промилле."},
    {"ai", "ai_gift_cap_kop", true, "Потолок подарка ИИ, копеек."},
    {"money", "refund_void_permille", false, "Доля подарка, возвращаемая при отмене вакансии, промилле."},
    {"money", "refund_withdraw_permille", false, "Доля подарка, возвращаемая при отзыве челобитной, промилле."},
};
// clang-format on

struct GroupText {
    std::string_view group;
    std::string_view text;
};

const GroupText kGroupTexts[] = {
    {"limits", "Пределы: челобитные, должности человека, подарок, опала, возраст ухода."},
    {"decision", "Решение государя о вакансии (court-model §8.2, §8.3): отбор, порог, отсрочки."},
    {"utility", "Слагаемые полезности назначения (court-model §8.2): веса, потолки, исключительный порядок."},
    {"support", "Подкрепления челобитной (court-model §8.4): услуги и поддержка партии."},
    {"favor", "Милость императора (court-model §8.5, §8.6): месячный пересчёт, пороги, подарки."},
    {"opinion", "Мнения и разовые потери после решений о назначении, снятии и опале."},
    {"servants", "Слуги (court-model §8.7): лояльность, просрочка, предательство, эффект управляющего."},
    {"parties", "Статические партии двора (court-model §8.10): вес и сплочённость."},
    {"ai", "Челобитные ИИ (court-model §8.9) до утилити-ИИ E17."},
    {"money", "Возвраты денег при отмене вакансии и отзыве челобитной."},
};

ObjectSchema group_schema(std::string_view group) {
    ObjectSchema s{std::string(group)};
    for (const GroupText& entry : kGroupTexts) {
        if (entry.group == group) {
            s.describe_kind(std::string(entry.text));
        }
    }
    for (const CourtBalanceField& field : kBalanceFields) {
        if (field.group == group) {
            s.required(std::string(field.name), field.wide ? i64() : i32())
                .describe(std::string(field.description));
        }
    }
    return s;
}

ObjectSchema seats_schema() {
    ObjectSchema s("seats");
    s.describe_kind("Какие места должности существуют (court-model §4.2).");
    s.required("rule", Type::enumeration({"fixed", "per_crown_province", "listed"}))
        .describe("`fixed` — постоянные места номерами 0...slots-1; `per_crown_province` — по месту в каждой "
                  "государевой провинции подходящего вида; `listed` — провинции, названные сценарием.")
        .optional("slots", Type::integer(1, 64))
        .describe("Число мест при `fixed`; при остальных правилах не пишется.")
        .optional("max", Type::integer(1, 4096))
        .describe("Наибольшее число мест при `per_crown_province` и `listed`.")
        .optional("kinds", Type::list(Type::reference("province_kind")))
        .describe(
            "При `per_crown_province`: виды провинций (id объектов `province_kind`), в которых есть место.");
    return s;
}

ObjectSchema requirements_schema() {
    ObjectSchema s("requirements");
    s.describe_kind(
        "Требования к кандидату (court-model §8.1): возраст, пол, честь, навыки, запретные черты.");
    s.required("min_age", Type::integer(0, 100))
        .describe("Наименьший возраст, лет (жёсткое требование).")
        .required("sex_allowed", Type::enumeration({"male", "female", "any"}))
        .describe("Допустимый пол; в M1 все должности мужские.")
        .required("min_honor", i32())
        .describe("Наименьшая честь рода кандидата (мягкое требование; «ранг» M1).")
        .optional("skills", Type::map(Type::integer(1, 20)))
        .describe("Наименьшие навыки: `навык = число` (id объекта `skill`); мягкое требование.")
        .optional("not_traits", Type::list(Type::reference("trait")))
        .describe("Черты, исключающие кандидата (жёсткое требование).");
    return s;
}

ObjectSchema office_schema() {
    ObjectSchema s{std::string(kOfficeKind)};
    s.intern_last();
    s.describe_kind("Тип должности двора (court-model §11.1). Стабильный `slot` записывается в сохранения; "
                    "порядок файлов на него не влияет. Типов не больше 63: 12 должностей M1 и фаворит.");
    s.required("slot", Type::integer(1, 63))
        .describe("Стабильный номер типа 1...63; уникален.")
        .required("group",
                  Type::enumeration({"court", "regional", "military", "chancery", "council", "favourite"}))
        .describe("Группа: придворные, региональные, военные, приказные, Совет, фаворит.")
        .required("name_key", Type::string())
        .describe("Ключ строки локализации с названием должности.")
        .required("desc_key", Type::string())
        .describe("Ключ строки локализации с описанием должности.")
        .required("appointer", Type::enumeration({"emperor", "council", "dynasty_head", "church"}))
        .describe("Кто назначает; в M1 реализован только `emperor`.")
        .required("seats", Type::block(seats_schema()))
        .describe("Правило мест.")
        .required("requirements", Type::block(requirements_schema()))
        .describe("Требования к кандидату.")
        .optional("skill_weights", Type::map(Type::integer(0, 100)))
        .describe(
            "Очки полезности за уровень навыка: `навык = число` (id объекта `skill`); пропуск — без вклада.")
        .required("salary_kop", Type::integer(0, 100'000'000))
        .describe("Жалованье, копеек в месяц.")
        .required("honor_on_assign", Type::integer(0, 10'000))
        .describe("Честь рода при назначении.")
        .required("honor_monthly", Type::integer(0, 10'000))
        .describe("Честь рода за месяц службы.")
        .required("decision_delay_days", Type::integer(1, 365))
        .describe("Дней от открытия вакансии до решения государя (30...90).")
        .optional("from_date", Type::date())
        .describe("Первый день, когда места существуют; пропуск — без ограничения.")
        .optional("to_date", Type::date())
        .describe("Последний день, когда места существуют; пропуск — без ограничения.")
        .optional("administrator_priority", Type::integer(0, 100))
        .describe("Региональные: при нескольких держателях провинции управляет тот, у кого число больше.")
        .optional("grants_trait", Type::reference("trait"))
        .describe("Черта, которую получает держатель (фаворит); пропуск — без черты.")
        .optional("council_vote_weight", Type::integer(0, 100))
        .describe("Зарезервированный голос в Совете (E34); пропуск — 0.");
    return s;
}

ObjectSchema servant_role_schema() {
    ObjectSchema s{std::string(kServantRoleKind)};
    s.intern_last();
    s.describe_kind(
        "Роль слуги двора (court-model §11.3, §8.7). Стабильный `slot` записывается в сохранения.");
    s.required("slot", Type::integer(1, 15))
        .describe(
            "Стабильный номер роли 1...15; уникален (1 управляющий, 2 шпион, 3 секретарь, 4 телохранитель).")
        .required("name_key", Type::string())
        .describe("Ключ строки локализации с названием роли.")
        .required("effect_key", Type::string())
        .describe("Ключ строки локализации с описанием эффекта роли.")
        .required("skill", Type::reference("skill"))
        .describe("Навык роли (id объекта `skill`).")
        .required("slots", Type::integer(1, 16))
        .describe("Слуг этой роли у одного рода (в M1 — 1).")
        .required("skill_min", Type::integer(0, 20))
        .describe("Наименьший навык кандидата на рынке.")
        .required("skill_max", Type::integer(0, 20))
        .describe("Наибольший навык кандидата на рынке; не меньше `skill_min`.")
        .required("wage_base_kop", Type::integer(0, 10'000'000))
        .describe("Основа жалованья, копеек в месяц.")
        .required("wage_per_skill_kop", Type::integer(0, 1'000'000))
        .describe("Добавка к жалованью за уровень навыка, копеек в месяц.")
        .required("fee_months", Type::integer(0, 120))
        .describe("Плата за найм в месячных жалованьях.");
    return s;
}

ObjectSchema court_party_schema() {
    ObjectSchema s{std::string(kCourtPartyKind)};
    s.intern_last();
    s.describe_kind("Статическая партия двора (court-model §8.10); членство задаёт сценарий.");
    s.required("slot", Type::integer(1, 7))
        .describe("Стабильный номер партии 1...7 (0 — вне партий); уникален.")
        .required("name_key", Type::string())
        .describe("Ключ строки локализации с названием партии.")
        .required("agenda_key", Type::string())
        .describe("Ключ строки локализации с повесткой партии.");
    return s;
}

ObjectSchema court_balance_schema() {
    ObjectSchema s{std::string(kCourtBalanceKind)};
    s.intern_last();
    s.describe_kind("Баланс двора (court-model §11.4): веса, лимиты и пороги. Ровно один объект; все числа "
                    "[предл.]. Диапазоны проверяет sim::validate(CourtRules).");
    for (const GroupText& entry : kGroupTexts) {
        s.required(std::string(entry.group), Type::block(group_schema(entry.group)));
    }
    return s;
}

}  // namespace

const std::vector<CourtBalanceField>& court_balance_fields() {
    return kBalanceFields;
}

void add_court_schemas(SchemaSet& set) {
    set.add(office_schema());
    set.add(servant_role_schema());
    set.add(court_party_schema());
    set.add(court_balance_schema());
}

}  // namespace pat::data
