#include "pat/data/event_schemas.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace pat::data {

namespace {

const std::vector<std::string> kTypeNames{
    "personal", "family", "provincial", "court", "imperial", "church", "military", "background", "hidden"};

Type ranged(std::int64_t lo, std::int64_t hi) {
    return Type::integer(lo, hi);
}

ObjectSchema window_schema() {
    ObjectSchema s("window");
    s.describe_kind("Окно дат события (юлианский календарь) и признак исторического якоря.");
    s.optional("from", Type::date())
        .describe("Первый день, с которого событие может сработать.")
        .optional("to", Type::date())
        .describe("Последний день, когда событие ещё может сработать; не раньше `from`.")
        .optional("anchor", Type::boolean())
        .describe("`yes` — исторический якорь (срабатывает в окне обязательно, если подходит).");
    return s;
}

ObjectSchema option_schema() {
    ObjectSchema s("option");
    s.describe_kind("Вариант выбора (event-engine.md §4.3). Метка — имя варианта, уникальное в событии; "
                    "ключ текста `evt.<id>.opt.<имя>`.");
    s.optional("trigger", Type::rules())
        .describe("Доступность: ложь — вариант виден, но недоступен, показывается причина.")
        .optional("show_if", Type::rules())
        .describe("Видимость: ложь — вариант скрыт совсем.")
        .optional("ai_chance", Type::rules())
        .describe("Вес выбора ИИ (число или аккумулятор `base`/`add`/`multiply`/`if`); по умолчанию 10.")
        .optional("effect", Type::rules())
        .describe("Эффекты варианта, по порядку.")
        .optional("default", Type::boolean())
        .describe("`yes` — вариант, подсвеченный в окне по умолчанию.")
        .optional("neutral", Type::boolean())
        .describe("`yes` — вариант без эффектов допустим.")
        .optional("text", Type::string())
        .describe("Ключ локализации подписи (по умолчанию `evt.<id>.opt.<имя>`).")
        .optional("detail", Type::string())
        .describe("Ключ локализации пояснения под подписью.");
    return s;
}

ObjectSchema event_schema() {
    ObjectSchema s{std::string(kEventKind)};
    s.intern_last();
    s.describe_kind(
        "Событие (event-engine.md §4.2, GDD 08 §4): область, тип, условия запуска, тексты, "
        "варианты. Правила — блоки записей `ключ оператор значение`, компилируются при загрузке.");
    s.required("scope", Type::enumeration({"character", "dynasty", "province", "realm"}))
        .describe("Корневая область: на ком срабатывает событие.")
        .required("type", Type::enumeration(kTypeNames))
        .describe("Тип события (GDD 08 §3): подпись окна и правила показа по умолчанию.")
        .required("category", Type::identifier())
        .describe("Категория: пул иллюстраций, звук и подпись внизу окна.")
        .optional("image", Type::identifier())
        .describe("Ключ пула гравюр; по умолчанию — категория.")
        .optional("tags", Type::list(Type::identifier()))
        .describe("Метки для фильтров, отчётов и ИИ.")
        .optional("on", Type::rules())
        .describe("On-action или список (`on = monthly_pulse`, `on = [on_death, yearly_pulse]`); роль после "
                  "точки (`on_marriage.bride`). Без `on` событие запускается только цепочкой.")
        .optional("trigger", Type::rules())
        .describe("Условия запуска (блок = И).")
        .optional("mean_time_to_happen", Type::rules())
        .describe("Среднее время до события: `{ months = 24 }` и модификаторы частоты "
                  "`modifier = { factor = 0.5  trigger = { … } }`; несовместимо с `chance`.")
        .optional("chance", Type::rules())
        .describe("Вероятность на действие или пульс, проценты (число или аккумулятор).")
        .optional("cooldown", Type::rules())
        .describe(
            "Пауза после срабатывания на этой области: `{ days = … }`, `{ months = … }`, `{ years = … }`.")
        .optional("cooldown_group", Type::identifier())
        .describe("Общая пауза для группы событий на области.")
        .optional("once", Type::enumeration({"scope", "global"}))
        .describe("Один раз на область или один раз за партию.")
        .optional("window", Type::block(window_schema()))
        .describe("Окно дат и признак исторического якоря.")
        .optional("pool", Type::identifier())
        .describe("Взвешенный пул: из прошедших ворота событий пула на области срабатывает одно.")
        .optional("weight", Type::rules())
        .describe("Вес в пуле (число или аккумулятор); по умолчанию 100.")
        .optional("priority", ranged(0, 100))
        .describe("Порядок индекса и очереди, 0...100 (больше — раньше); по умолчанию 50.")
        .optional("urgency", Type::enumeration({"critical", "important", "info"}))
        .describe("Класс очереди и автопаузы; по умолчанию по `display`.")
        .optional("display", Type::enumeration({"modal", "notification", "silent"}))
        .describe("Форма показа игроку-получателю; по умолчанию по `type`.")
        .optional("autopause", Type::enumeration({"auto", "always", "never"}))
        .describe("Автопауза: `auto` — да для `modal` и `critical`; `always`, `never` — всегда или никогда "
                  "(yes и no — булевы значения, не идентификаторы).")
        .optional("demotable", Type::boolean())
        .describe("Модальное событие сверх лимита окон понижается до уведомления; по умолчанию `yes`.")
        .optional("rumor", Type::boolean())
        .describe("Фоновое событие, о котором может узнать игрок; по умолчанию `no`.")
        .optional("survives_death", Type::boolean())
        .describe("Экземпляр не отменяется со смертью области-корня; по умолчанию `no`.")
        .optional("group_key", Type::identifier())
        .describe("Группировка однотипных уведомлений.")
        .optional("chain_scopes", Type::rules())
        .describe("Сохранённые области, которые приносит цепочка (`trigger_event` другого события): "
                  "`{ governor = character }`; доступны как `scope:имя`.")
        .optional("immediate", Type::rules())
        .describe("Эффекты при срабатывании до показа (сохранение областей, флаги).")
        .optional("after", Type::rules())
        .describe("Эффекты после эффектов выбранного варианта.")
        .optional("speaker", Type::rules())
        .describe("Область «говорящего» окна (`scope:имя` или путь переходов).")
        .optional("title", Type::string())
        .describe("Ключ локализации заголовка (по умолчанию `evt.<id>.title`).")
        .optional("subtitle", Type::string())
        .describe("Ключ локализации подзаголовка.")
        .optional("desc", Type::string())
        .describe("Ключ локализации текста (по умолчанию `evt.<id>.desc`).")
        .optional("quote", Type::string())
        .describe("Ключ локализации цитаты окна.")
        .repeatable_labeled("option", Type::block(option_schema()))
        .describe("Вариант выбора с меткой-именем; от 1 до 4.")
        .optional("single_option", Type::boolean())
        .describe("У модального события допускается один вариант «Понятно».")
        .optional("review", Type::enumeration({"draft", "reviewed"}))
        .describe("Статус текстов (#294): `reviewed` ставит только владелец; по умолчанию `draft`.")
        .optional("history", Type::enumeration({"unchecked", "checked"}))
        .describe("Историчность исторического события (#294): `checked` — проверено.")
        .optional("sources", Type::list(Type::string()))
        .describe("Источники для `history`.")
        .optional("notes", Type::string())
        .describe("Заметка автора; игнорируется.");
    return s;
}

ObjectSchema modifier_schema() {
    ObjectSchema s{std::string(kEventModifierKind)};
    s.intern_last();
    s.describe_kind("Модификатор события (event-engine.md §4.4): метка с длительностью и числами для "
                    "выражений событий (`has_modifier`, `modifier_value`). Статов не меняет.");
    s.required("scope", Type::enumeration({"character", "dynasty", "province", "realm"}))
        .describe("На какую область можно навесить.")
        .optional("duration", Type::rules())
        .describe("Длительность по умолчанию: `{ days = … }`, `{ months = … }`, `{ years = … }`; "
                  "без неё — бессрочно.")
        .optional("stack", Type::enumeration({"refresh", "replace", "ignore"}))
        .describe("Повторное добавление: продлить, заменить или игнорировать; по умолчанию `refresh`.")
        .optional("values", Type::map(Type::fixed64()))
        .describe("Числа для выражений событий: `имя = число`.")
        .optional("tooltip", Type::string())
        .describe("Ключ локализации названия и пояснения (по умолчанию `modifier.<id>`).");
    return s;
}

ObjectSchema limits_schema() {
    ObjectSchema s("limits");
    s.describe_kind("Лимиты ВМ событий (event-engine.md §7.4); переопределяются только вниз.");
    s.optional("max_trigger_instr", ranged(1, 4000))
        .describe("Инструкций на запуск триггера.")
        .optional("max_effect_instr", ranged(1, 40000))
        .describe("Инструкций на запуск эффекта.")
        .optional("max_commands", ranged(1, 64))
        .describe("Команд на запуск.")
        .optional("max_state_ops", ranged(1, 128))
        .describe("Операций состояния на запуск.")
        .optional("max_fanout", ranged(1, 256))
        .describe("Элементов на вход итератора.");
    return s;
}

ObjectSchema settings_schema() {
    ObjectSchema s{std::string(kEventSettingsKind)};
    s.intern_last();
    s.describe_kind("Настройки движка событий (event-engine.md §4.5); один объект `\"main\"` в "
                    "`data/balance/events.pd`.");
    s.optional("modal_per_month", ranged(1, 100))
        .describe("Лимит модальных окон в календарный месяц при низкой фазе кризиса (по умолчанию 6).")
        .optional("modal_per_month_crisis", ranged(1, 100))
        .describe("Лимит при фазе кризиса не ниже `crisis_phase_from` (по умолчанию 12).")
        .optional("crisis_phase_from", ranged(0, 10))
        .describe("С какой фазы кризиса действует повышенный лимит (по умолчанию 2).")
        .optional("defer_max", ranged(0, 10))
        .describe("Сколько раз игрок может отложить событие (по умолчанию 2).")
        .optional("defer_days", ranged(1, 365))
        .describe("На сколько дней откладывается событие (по умолчанию 14).")
        .optional("frequency_choices", Type::list(ranged(1, 10000)))
        .describe("Допустимые множители частоты, промилле (по умолчанию 500, 1000, 2000).")
        .optional("frequency_applies_to", Type::enumeration({"player", "all"}))
        .describe("К кому применяется множитель частоты (по умолчанию `player`).")
        .optional("max_chain_depth", ranged(1, 32))
        .describe("Глубина мгновенного `trigger_event` в одном тике (по умолчанию 8).")
        .optional("max_timer_chain", ranged(1, 256))
        .describe("Глубина цепочки через таймеры (по умолчанию 32).")
        .optional("abort_disable_months", ranged(0, 120))
        .describe("На сколько месяцев событие отключается после отката по лимитам (по умолчанию 12).")
        .optional("fire_cap_per_pulse", ranged(1, 16))
        .describe("Сколько пульсовых событий на сущность за пульс (по умолчанию 1).")
        .optional("pulse_month_days", ranged(1, 60))
        .describe("Длина месяца для ppm MTTH, дней (по умолчанию 30).")
        .optional("pulse_year_days", ranged(1, 400))
        .describe("Длина года для ppm MTTH, дней (по умолчанию 365).")
        .optional("rumor_delay_days", Type::list(ranged(0, 365)))
        .describe("Границы задержки слухов, дней: два числа (по умолчанию 7, 30).")
        .optional("notification_ring", ranged(16, 4096))
        .describe("Размер кольца журнала уведомлений (по умолчанию 256).")
        .optional("history_ring", ranged(16, 8192))
        .describe("Размер кольца истории (по умолчанию 512).")
        .optional("limits", Type::block(limits_schema()))
        .describe("Лимиты ВМ, только вниз.");
    return s;
}

}  // namespace

const std::vector<std::string>& event_type_names() {
    return kTypeNames;
}

void add_event_schemas(SchemaSet& set) {
    set.add(event_schema());
    set.add(modifier_schema());
    set.add(settings_schema());
}

}  // namespace pat::data
