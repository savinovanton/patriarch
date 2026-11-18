#include "pat/data/crisis_schemas.hpp"

#include <cstdint>
#include <limits>
#include <string>
#include <utility>

namespace pat::data {

namespace {

// Only representation limits belong here; sim::crisis::validate(CrisisRules) owns the corridors.
Type i32() {
    return Type::integer(std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max());
}
Type i64() {
    return Type::integer(std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::max());
}

// clang-format off
const std::vector<CrisisBalanceField> kBalanceFields{
    {"stability", "base", false, "Основа стабильности: итог = основа + вклады компонентов, 0...100."},
    {"stability", "step_cap_x100", false, "Наибольшее изменение суммы вкладов за месяц, сотые доли пункта (инерция)."},
    {"stability", "band_up_margin", false, "Запас для перехода полосы вверх: вверх — при пороге + запас, вниз — при пересечении."},
    {"stability", "legacy_decay_pm", false, "Доля «наследия смуты», затухающая за месяц, промилле."},
    {"stability", "legacy_snap_x100", false, "Наследие меньше этого модуля обнуляется, сотые доли пункта."},
    {"bands", "order_min", false, "Порог полосы «порядок»: стабильность не ниже."},
    {"bands", "tension_min", false, "Порог полосы «напряжённость»: не ниже, но ниже порядка."},
    {"bands", "crisis_min", false, "Порог полосы «кризис»: не ниже, но ниже напряжённости; ниже — «развал»."},
    {"bands", "plot_ready_order_pm", false, "Множитель готовности заговоров при порядке, промилле."},
    {"bands", "plot_ready_tension_pm", false, "Множитель готовности заговоров при напряжённости, промилле."},
    {"bands", "plot_ready_crisis_pm", false, "Множитель готовности заговоров при кризисе, промилле."},
    {"bands", "plot_ready_collapse_pm", false, "Множитель готовности заговоров при развале, промилле."},
    {"bands", "detect_order_pm", false, "Множитель шанса раскрытия заговоров при порядке, промилле."},
    {"components", "weight_fiscal", false, "Вес компонента «фискальное давление», промилле; все восемь весов дают 1000."},
    {"components", "weight_war", false, "Вес компонента «военная усталость», промилле."},
    {"components", "weight_recruit", false, "Вес компонента «рекрутская тяжесть», промилле."},
    {"components", "weight_mood", false, "Вес компонента «настроения сословий», промилле."},
    {"components", "weight_court", false, "Вес компонента «сплочённость двора», промилле."},
    {"components", "weight_church", false, "Вес компонента «церковный конфликт», промилле."},
    {"components", "weight_clarity", false, "Вес компонента «ясность престолонаследия», промилле."},
    {"components", "weight_authority", false, "Вес компонента «авторитет императора», промилле."},
    {"components", "fiscal_tax_per_level", false, "Пункты давления за уровень налога выше уровня по умолчанию."},
    {"components", "fiscal_arrears_share_k", false, "Пункты давления за долю банкротов (доля × k)."},
    {"components", "fiscal_short_quota_k", false, "Пункты давления за недобор квоты за последние три месяца."},
    {"components", "fiscal_short_quota_cap", false, "Потолок слагаемого недоборов квот, пунктов."},
    {"components", "fiscal_deficit", false, "Пункты давления при дефиците казны короны."},
    {"components", "recruit_level_k", false, "Пункты тяжести за уровень набора выше уровня по умолчанию."},
    {"components", "recruit_short_share_k", false, "Пункты тяжести за долю недобранных рекрутских квот."},
    {"components", "mood_gain_pm", false, "Усиление средневзвешенного настроения сословий, промилле."},
    {"components", "estate_nobles", false, "Вес дворян в средневзвешенном настроении, промилле."},
    {"components", "estate_clergy", false, "Вес духовенства в настроении, промилле."},
    {"components", "estate_townsfolk", false, "Вес посада в настроении, промилле."},
    {"components", "estate_peasants", false, "Вес крестьян в настроении, промилле."},
    {"components", "estate_cossacks", false, "Вес казаков в настроении, промилле."},
    {"components", "estate_old_believers", false, "Вес старообрядцев в настроении, промилле."},
    {"components", "court_base", false, "Основа сплочённости двора, пунктов."},
    {"components", "court_opposition_k", false, "Потеря за долю оппозиционной партии (доля × k)."},
    {"components", "court_known_plot_k", false, "Потеря за каждый известный заговор."},
    {"components", "court_known_plot_cap", false, "Сколько известных заговоров учитывается."},
    {"components", "court_purge_heat_k_pm", false, "Потеря за накал чисток, промилле на пункт."},
    {"components", "clarity_vacant", false, "Ясность при вакантном престоле."},
    {"components", "clarity_dispute", false, "Ясность при споре дома о престоле."},
    {"components", "clarity_appointed", false, "Ясность при законе «назначение» и живом взрослом назначенном."},
    {"components", "clarity_lawful_adult", false, "Ясность при законе порядка и взрослом первом в порядке."},
    {"components", "clarity_minor", false, "Ясность при несовершеннолетнем наследнике."},
    {"components", "clarity_none", false, "Ясность, если наследников нет."},
    {"components", "clarity_rival_k", false, "Потеря за каждого сильного соперника наследника."},
    {"components", "clarity_rival_cap", false, "Сколько соперников учитывается."},
    {"components", "clarity_rival_margin", false, "Соперник считается сильным, если его claim_strength не ниже силы наследника плюс это число."},
    {"components", "authority_k", false, "Множитель отклонения авторитета императора от 50."},
    {"phases", "p1_enter", false, "Фаза 1 «брожение» начинается, если стабильность ниже этого порога."},
    {"phases", "p1_exit", false, "Фаза 1 заканчивается при стабильности не ниже порога (гистерезис)."},
    {"phases", "p1_opposition_share", false, "Доля оппозиционной партии, с которой партия считается оппозицией, процентов."},
    {"phases", "p1_opposition_opinion", false, "Среднее мнение глав партии к государю не выше — признак оппозиции."},
    {"phases", "p2_members", false, "Сколько участников делает заговор основанием фазы 2."},
    {"phases", "p2_susp_enter", false, "Подозрение (пункты), с которого начинается фаза 2."},
    {"phases", "p2_susp_exit", false, "Подозрение (пункты), ниже которого фаза 2 может закончиться."},
    {"phases", "p3_enter", false, "Стабильность ниже этого порога открывает окно переворота (фаза 3)."},
    {"phases", "p3_exit", false, "Стабильность не ниже этого порога закрывает окно переворота."},
    {"phases", "p3_split_court", false, "Вход компонента двора не выше этого значения — открытый раскол, фаза 3."},
    {"phases", "after_throne", false, "Фаза после коронации; затем обычное снижение."},
    {"protect", "years", false, "Защитное окно: сколько лет от старта государь не умирает случайно."},
    {"protect", "health_floor", false, "Пол здоровья государя в защитном окне."},
    {"emperor", "ill_enter", false, "Здоровье ниже — стадия «нездоров»."},
    {"emperor", "ill_exit", false, "Здоровье не ниже — выход из стадии «нездоров»."},
    {"emperor", "heavy_enter", false, "Здоровье ниже — стадия «тяжело болен»."},
    {"emperor", "heavy_exit", false, "Здоровье не ниже — выход из стадии «тяжело болен»."},
    {"emperor", "authority_base", false, "Авторитет, к которому тянется императорский, пунктов."},
    {"emperor", "authority_pull_pm", false, "Доля разрыва до основы, закрываемая за месяц, промилле."},
    {"emperor", "authority_ill_x100", false, "Месячный сдвиг авторитета при стадии «нездоров», сотые доли."},
    {"emperor", "authority_heavy_x100", false, "Месячный сдвиг авторитета при стадии «тяжело болен», сотые доли."},
    {"emperor", "authority_riot_x100", false, "Сдвиг авторитета за каждый бунт, сотые доли."},
    {"emperor", "authority_riot_cap_x100", false, "Предел месячного сдвига от бунтов, сотые доли."},
    {"emperor", "authority_minor", false, "Авторитет несовершеннолетнего государя."},
    {"emperor", "calm_min_stability", false, "Наименьшая стабильность для спокойной передачи престола."},
    {"emperor", "challenge_margin", false, "Претендент оспаривает передачу, если не слабее наследника больше чем на это число."},
    {"emperor", "interregnum_days", false, "Длительность междуцарствия, дней."},
    {"emperor", "auto_claimants", false, "Сколько первых по порядку получают претензию автоматически."},
    {"emperor", "interregnum_legacy_x100", false, "Вклад междуцарствия в наследие смуты, сотые доли."},
    {"emperor", "interregnum_loser_opinion", false, "Сдвиг мнения поддержавших проигравшего к победителю."},
    {"projects", "noise_max", false, "Наибольший шум счёта проектов, пунктов."},
    {"projects", "war_need", false, "Потребность проекта «война» при идущей войне."},
    {"projects", "taxes_need_max", false, "Наибольшая потребность проекта «налоги»."},
    {"projects", "taxes_need_min", false, "Наименьшая потребность проекта «налоги» (избыток казны)."},
    {"projects", "taxes_low_kop", true, "Казна короны, ниже которой растёт потребность в налогах, копеек."},
    {"projects", "purges_susp_pm", false, "Вклад наибольшего подозрения в потребность чисток, промилле."},
    {"projects", "purges_plot_k", false, "Вклад известного заговора в потребность чисток."},
    {"projects", "purges_plot_cap", false, "Сколько известных заговоров учитывается чистками."},
    {"projects", "recruit_quota_from_year", false, "Год, с которого ставятся рекрутские квоты (решение #140)."},
    {"suspicion", "offices_cap", false, "Потолок месячной прибавки подозрения за должности родственников, пунктов."},
    {"suspicion", "quota_short_x100", false, "Прибавка подозрения за недобор квоты, сотые доли."},
    {"suspicion", "danger_x100", false, "Прибавка за гвардейскую должность при слабом контроле столицы, сотые доли."},
    {"suspicion", "danger_guard_below", false, "Контроль столицы ниже этого числа делает гвардейскую должность опасной."},
    {"suspicion", "favor_k_pm", false, "Снижение подозрения за пункт милости выше опорной, промилле."},
    {"suspicion", "favor_pivot", false, "Милость, выше которой подозрение снижается."},
    {"suspicion", "decay_pct", false, "Затухание подозрения за месяц, процентов."},
    {"suspicion", "decay_min_x100", false, "Наименьшее затухание при положительном подозрении, сотые доли."},
    {"suspicion", "expose_organizer", false, "Подозрение организатору при раскрытии заговора."},
    {"suspicion", "expose_member", false, "Подозрение участнику при раскрытии заговора."},
    {"suspicion", "confirmed_accused", false, "Подозрение обвиняемому при подтверждённом доносе."},
    {"suspicion", "confirmed_informer", false, "Сдвиг подозрения к доносчику при подтверждённом доносе."},
    {"suspicion", "false_informer", false, "Подозрение доносчику при ложном доносе."},
    {"suspicion", "open_claim", false, "Подозрение за открытую претензию при живом здоровом государе."},
    {"suspicion", "band_low", false, "Нижняя граница полосы подозрения."},
    {"suspicion", "band_mid", false, "Средняя граница полосы подозрения."},
    {"suspicion", "band_high", false, "Верхняя граница полосы подозрения."},
    {"suspicion", "band_hysteresis", false, "Гистерезис полос подозрения, пунктов."},
    {"suspicion", "denounce_cooldown_days", false, "Перезарядка доноса для дома, дней."},
    {"suspicion", "investigation_days", false, "Срок расследования доноса, дней."},
    {"suspicion", "credibility_favor_k", false, "Вес милости доносчика в доверии."},
    {"suspicion", "credibility_honor_div", false, "Делитель чести доносчика в доверии."},
    {"suspicion", "credibility_cap", false, "Предел доверия по модулю, промилле."},
    {"suspicion", "confirm_base", false, "Основа шанса подтверждения доноса, промилле."},
    {"suspicion", "confirm_secrecy_k", false, "Прибавка шанса за недостаток скрытности заговора."},
    {"suspicion", "confirm_min", false, "Наименьший шанс подтверждения, промилле."},
    {"suspicion", "confirm_max", false, "Наибольший шанс подтверждения, промилле."},
    {"suspicion", "false_expose_base", false, "Основа шанса разоблачить ложный донос, промилле."},
    {"suspicion", "false_expose_min", false, "Наименьший шанс разоблачения ложного доноса, промилле."},
    {"suspicion", "false_expose_max", false, "Наибольший шанс разоблачения ложного доноса, промилле."},
    {"suspicion", "informer_leniency_pm", false, "Доля наказания доносчика-участника заговора, промилле."},
    {"suspicion", "informer_honor", false, "Честь доносчику за подтверждённый донос."},
    {"suspicion", "false_honor", false, "Сдвиг чести доносчика за ложный донос."},
    {"suspicion", "false_opinion", false, "Сдвиг мнения обвиняемого к доносчику при ложном доносе."},
    {"guard", "base", false, "Основа контроля столицы, пунктов."},
    {"guard", "empty_seat", false, "Сдвиг за пустое место полковника гвардии."},
    {"guard", "loyalty_pm", false, "Вклад лояльности держателя места, промилле."},
    {"guard", "unpaid", false, "Сдвиг, если жалованье гвардии не плачено."},
    {"guard", "shift_decay", false, "Затухание сдвига контроля за месяц, пунктов."},
    {"guard", "access_to_guard_pm", false, "Вклад взносов «доступ» в перетянутую долю гвардии, промилле."},
    {"plot", "max_members", false, "Наибольшее число участников заговора."},
    {"plot", "prep_per_week_x100", false, "Прирост готовности в неделю, сотые доли пункта."},
    {"plot", "strength_lo_pm", false, "Нижний множитель силы заговора для готовности, промилле."},
    {"plot", "strength_hi_pm", false, "Верхний множитель силы заговора для готовности, промилле."},
    {"plot", "secrecy_loss_x100", false, "Потеря скрытности в неделю, сотые доли."},
    {"plot", "secrecy_member_loss_x100", false, "Потеря скрытности в неделю за участника сверх второго, сотые доли."},
    {"plot", "secrecy_info_gain_x100", false, "Прибавка скрытности за очко взносов «сведения», сотые доли."},
    {"plot", "secrecy_info_cap", false, "Сколько очков «сведений» учитывается."},
    {"plot", "detect_base", false, "Основа шанса раскрытия в неделю, промилле."},
    {"plot", "detect_member", false, "Прибавка шанса за участника сверх второго."},
    {"plot", "detect_age", false, "Прибавка шанса за неделю возраста заговора."},
    {"plot", "detect_age_cap", false, "Сколько недель возраста учитывается."},
    {"plot", "detect_susp_div", false, "Делитель подозрения организатора в шансе раскрытия."},
    {"plot", "detect_secrecy_div", false, "Делитель скрытности в шансе раскрытия."},
    {"plot", "detect_cap", false, "Потолок шанса раскрытия в неделю, промилле."},
    {"plot", "exposure_base", false, "Прирост заметности заговора в неделю."},
    {"plot", "exposure_member_div", false, "Делитель числа участников в приросте заметности."},
    {"plot", "idle_days", false, "Через сколько дней заговор без двух участников отменяется."},
    {"plot", "money_per_point_kop", true, "Сколько копеек взноса дают одно очко силы."},
    {"plot", "forces_div", false, "Делитель веса дома в ёмкости взноса «силы»."},
    {"plot", "honor_div", false, "Делитель чести дома в ёмкости взноса «влияние»."},
    {"plot", "access_per_guard_seat", false, "Ёмкость «доступа» за каждое гвардейское место дома."},
    {"plot", "access_other", false, "Ёмкость «доступа» дома без гвардейских мест."},
    {"plot", "accept_threshold", false, "Наименьший счёт, при котором ИИ-род принимает приглашение в заговор."},
    {"plot", "stance_plotter", false, "Слагаемое приглашения для рода с позицией «заговорщик»."},
    {"plot", "stance_loyalist", false, "Слагаемое приглашения для рода с позицией «лоялист»."},
    {"plot", "loyalty_k_pm", false, "Вес лояльности в счёте приглашения, промилле."},
    {"plot", "disgraced_member", false, "Слагаемое приглашения за опального в доме."},
    {"plot", "party_candidate", false, "Слагаемое приглашения, если кандидат из партии дома."},
    {"plot", "order_penalty", false, "Штраф приглашения при стабильности порядка."},
    {"coup", "w_guard", false, "Вес перетянутой гвардии в силе переворота, промилле."},
    {"coup", "w_strength", false, "Вес силы заговора в силе переворота, промилле."},
    {"coup", "w_legit", false, "Вес claim_strength кандидата в силе переворота, промилле."},
    {"coup", "w_authority", false, "Вес авторитета императора в сопротивлении, промилле."},
    {"coup", "w_factions", false, "Вес родов-лоялистов в сопротивлении, промилле."},
    {"coup", "surprise_max", false, "Наибольший бонус внезапности нераскрытого заговора, пунктов."},
    {"coup", "alert_known", false, "Тревога, если государь знает о заговоре."},
    {"coup", "alert_susp", false, "Добавка тревоги при высоком подозрении организатора."},
    {"coup", "alert_susp_from", false, "Подозрение организатора, с которого добавляется тревога."},
    {"coup", "success_min", false, "Наименьший шанс успеха, промилле."},
    {"coup", "success_max", false, "Наибольший шанс успеха, промилле."},
    {"coup", "partial_base", false, "Основа доли частичного успеха от остатка, промилле."},
    {"coup", "partial_slope", false, "Наклон доли частичного успеха на пункт разницы."},
    {"coup", "partial_min", false, "Наименьшая доля частичного успеха, промилле."},
    {"coup", "partial_max", false, "Наибольшая доля частичного успеха, промилле."},
    {"coup", "hard_margin", false, "Провал тяжёлый, если разница сил не ниже минус этого числа."},
    {"coup", "ai_coup_min_pm", false, "Наименьший шанс успеха, при котором ИИ решается на переворот, промилле."},
    {"coup", "sigmoid_step", false, "Шаг таблицы сигмоиды по разнице сил, пунктов."},
    {"stance", "decay_pm", false, "Затухание лояльности рода за месяц, промилле."},
    {"stance", "favor_div", false, "Делитель отклонения милости в сдвиге лояльности."},
    {"stance", "offices_cap_x100", false, "Потолок прибавки за держания рода, сотые доли."},
    {"stance", "offices_k_x100", false, "Прибавка за каждое держание, сотые доли."},
    {"stance", "disgrace_loss", false, "Потеря за члена рода в опале."},
    {"stance", "disgrace_cap", false, "Сколько опальных учитывается."},
    {"stance", "quota_loss", false, "Потеря за недобор квоты."},
    {"stance", "suspicion_div", false, "Делитель подозрения в сдвиге лояльности."},
    {"stance", "party_cap", false, "Предел слагаемого партии."},
    {"stance", "authority_div", false, "Делитель отклонения авторитета императора в сдвиге лояльности."},
    {"stance", "loyalist_enter", false, "Лояльность не ниже — позиция «лоялист»."},
    {"stance", "loyalist_exit", false, "Лояльность ниже — выход из позиции «лоялист»."},
    {"stance", "plotter_enter", false, "Лояльность не выше — позиция «заговорщик»."},
    {"stance", "plotter_exit", false, "Лояльность не ниже — выход из позиции «заговорщик»."},
    {"stance", "plotter_ambition", false, "Наименьшая амбиция главы для позиции «заговорщик»."},
    {"stance", "min_months", false, "Позиция меняется не чаще, чем раз в столько месяцев."},
    {"stance", "ai_candidate_min", false, "Наименьший claim_strength кандидата заговора ИИ."},
    {"stance", "ai_contrib_share", false, "Доля ёмкости, вносимая ИИ в месяц, процентов."},
    {"stance", "loyalist_denounce_delay_months", false, "Через сколько месяцев заметности заговора лоялист доносит."},
    {"consequences", "ok_emperor_honor", false, "Честь дома свергнутого государя при успехе."},
    {"consequences", "ok_legacy_x100", false, "Вклад успеха в наследие смуты, сотые доли."},
    {"consequences", "ok_candidate_honor", false, "Честь кандидата-победителя."},
    {"consequences", "ok_authority", false, "Авторитет нового государя."},
    {"consequences", "ok_organizer_honor", false, "Честь организатора при успехе."},
    {"consequences", "ok_member_honor", false, "Честь участника при успехе."},
    {"consequences", "ok_reward_offices", false, "Сколько должностей достаётся участникам."},
    {"consequences", "ok_loyalist_susp", false, "Подозрение к крупным родам-лоялистам от нового государя."},
    {"consequences", "ok_disgrace_days", false, "Срок опалы приближённых прежнего государя, дней."},
    {"consequences", "part_authority", false, "Сдвиг авторитета при частичном успехе."},
    {"consequences", "part_legacy_x100", false, "Вклад частичного успеха в наследие смуты, сотые доли."},
    {"consequences", "part_organizer_honor", false, "Сдвиг чести организатора при частичном успехе."},
    {"consequences", "part_organizer_susp", false, "Подозрение организатору при частичном успехе."},
    {"consequences", "part_member_susp", false, "Подозрение участникам при частичном успехе."},
    {"consequences", "part_concession_offices", false, "Сколько назначенных императором уступает частичный успех."},
    {"consequences", "fail_authority", false, "Сдвиг авторитета при раскрытом провале."},
    {"consequences", "fail_legacy_x100", false, "Вклад раскрытого провала в наследие смуты, сотые доли."},
    {"consequences", "fail_candidate_honor", false, "Честь кандидата при провале."},
    {"consequences", "fail_organizer_disgrace_days", false, "Срок опалы организатора, дней."},
    {"consequences", "fail_organizer_honor", false, "Честь организатора при провале."},
    {"consequences", "fail_organizer_confiscate_pm", false, "Доля казны организатора, конфискуемая при провале, промилле."},
    {"consequences", "fail_organizer_unity", false, "Единство рода организатора при провале."},
    {"consequences", "fail_organizer_susp", false, "Подозрение организатору при провале."},
    {"consequences", "fail_member_disgrace_days", false, "Срок опалы участника, дней."},
    {"consequences", "fail_member_honor", false, "Честь участника при провале."},
    {"consequences", "fail_member_confiscate_pm", false, "Доля казны участника, конфискуемая при провале, промилле."},
    {"consequences", "fail_member_susp", false, "Подозрение участнику при провале."},
    {"consequences", "fail_house_opinion", false, "Мнение родов к организатору при провале."},
    {"consequences", "hard_authority", false, "Сдвиг авторитета при тяжёлом провале."},
    {"consequences", "hard_legacy_x100", false, "Вклад тяжёлого провала в наследие смуты, сотые доли."},
    {"consequences", "hard_candidate_honor", false, "Честь кандидата при тяжёлом провале."},
    {"consequences", "hard_organizer_confiscate_pm", false, "Доля казны организатора при тяжёлом провале, промилле."},
    {"consequences", "hard_organizer_unity", false, "Единство рода организатора при тяжёлом провале."},
    {"consequences", "exposed_authority", false, "Сдвиг авторитета при заговоре, раскрытом до переворота."},
    {"consequences", "exposed_legacy_x100", false, "Вклад раскрытия до переворота в наследие смуты, сотые доли."},
    {"consequences", "exposed_candidate_honor", false, "Честь кандидата при раскрытии до переворота."},
    {"consequences", "exposed_mild_pm", false, "Доля наказания при раскрытии до переворота, промилле."},
    {"consequences", "purge_honor", false, "Сдвиг чести дома, наказанного чисткой."},
    {"consequences", "purge_after_susp", false, "Сдвиг подозрения после наказания чисткой (насыщение)."},
};

const std::vector<CrisisBalanceField> kClaimFields{
    {"basis", "marriage", false, "Очки основания «по браку»."},
    {"basis", "designation_appointment", false, "Очки назначенного наследника при законе «назначение»."},
    {"basis", "designation_other", false, "Очки назначенного наследника при другом законе."},
    {"basis", "kin_max_degree", false, "Наибольшая степень родства для основания «кровная»."},
    {"basis", "force_min", false, "Контроль столицы, с которого допустимо основание «по праву силы»."},
    {"basis", "force_supporters", false, "Сколько домов должно поддержать претензию «по праву силы»."},
    {"basis", "max_per_house", false, "Сколько претензий может заявить один дом."},
    {"support", "guard_k_pm", false, "Вклад перетянутой гвардии заговора, промилле."},
    {"support", "guard_cap", false, "Потолок вклада гвардии, очков."},
    {"support", "houses_max", false, "Размах поддержки родов, очков."},
    {"support", "council_max", false, "Размах поддержки Думы, очков."},
    {"support", "prestige_div", false, "Делитель престижа дома."},
    {"support", "prestige_cap", false, "Потолок вклада престижа, очков."},
    {"penalty", "outside_line", false, "Штраф претендента вне линии престола."},
    {"penalty", "relief_marriage", false, "Смягчение: брак с линией."},
    {"penalty", "relief_appointment", false, "Смягчение: назначение (снимает штраф целиком)."},
    {"penalty", "relief_council_max", false, "Смягчение: наибольшая поддержка Думы."},
    {"penalty", "relief_church", false, "Смягчение: поддержка Церкви."},
    {"penalty", "relief_guard", false, "Смягчение: контроль столицы."},
    {"penalty", "relief_guard_from", false, "Контроль столицы, с которого действует смягчение."},
    {"penalty", "phase4_pm", false, "Доля штрафа в фазе 4 (резерв), промилле."},
    {"penalty", "illegitimate", false, "Сдвиг claim_strength незаконнорождённого."},
    {"penalty", "disgraced", false, "Сдвиг claim_strength опального."},
};
// clang-format on

struct GroupText {
    std::string_view group;
    std::string_view text;
};

// clang-format off
const std::vector<GroupText> kBalanceGroups{
    {"stability", "Стабильность (throne-crisis §8.1): основа, инерция, гистерезис полос."},
    {"bands", "Полосы стабильности и их влияние на заговоры (§8.1)."},
    {"components", "Компоненты стабильности: веса, входы, сословия (§8.1)."},
    {"phases", "Фазы кризиса 0-3: пороги входа и выхода (§8.2)."},
    {"protect", "Защитное окно императора (§8.3, D17)."},
    {"emperor", "Здоровье, авторитет, передача престола и междуцарствие (§8.3)."},
    {"projects", "Общие числа проектов императора (§8.5)."},
    {"suspicion", "Подозрение и донос (§8.6)."},
    {"guard", "Гвардия и контроль столицы (§8.7)."},
    {"plot", "Заговор: сила, скрытность, раскрытие, приглашения (§8.8)."},
    {"coup", "Окно переворота: сила, сопротивление, сигмоида (§8.9)."},
    {"stance", "Позиции ИИ-родов (§8.11)."},
    {"consequences", "Последствия переворота, раскрытия и чисток (§8.10)."},
};

const std::vector<GroupText> kClaimGroups{
    {"basis", "Основания претензии (§8.4)."},
    {"support", "Слагаемые поддержки претензии (§8.4)."},
    {"penalty", "Штраф вне линии, смягчения и прочее (§8.4)."},
};
// clang-format on

ObjectSchema group_schema(const std::vector<CrisisBalanceField>& fields,
                          const std::vector<GroupText>& texts,
                          std::string_view group) {
    ObjectSchema s{std::string(group)};
    for (const GroupText& entry : texts) {
        if (entry.group == group) {
            s.describe_kind(std::string(entry.text));
        }
    }
    for (const CrisisBalanceField& field : fields) {
        if (field.group == group) {
            s.required(std::string(field.name), field.wide ? i64() : i32())
                .describe(std::string(field.description));
        }
    }
    return s;
}

void add_groups(ObjectSchema& s,
                const std::vector<CrisisBalanceField>& fields,
                const std::vector<GroupText>& texts) {
    for (const GroupText& entry : texts) {
        s.required(std::string(entry.group), Type::block(group_schema(fields, texts, entry.group)));
    }
}

ObjectSchema crisis_balance_schema() {
    ObjectSchema s{std::string(kCrisisBalanceKind)};
    s.intern_last();
    s.describe_kind("Баланс кризиса и престола (throne-crisis-model §8, §11): веса, пороги, гистерезис, "
                    "защитное окно, заговор, окно переворота, последствия. Ровно один объект; все числа "
                    "[предл.]. Диапазоны проверяет sim::crisis::validate(CrisisRules).");
    add_groups(s, kBalanceFields, kBalanceGroups);
    s.required("sigmoid", Type::list(i32()))
        .describe("Таблица вероятности успеха переворота в промилле: 21 число по разнице сил от -100 до 100 "
                  "с шагом coup.sigmoid_step; не убывает, p(d) + p(-d) = 1000 (§8.9).");
    return s;
}

ObjectSchema claim_rules_schema() {
    ObjectSchema s{std::string(kClaimRulesKind)};
    s.intern_last();
    s.describe_kind("Правила претензий на престол (throne-crisis-model §8.4): слагаемые claim_strength. "
                    "Ровно один объект; все числа [предл.].");
    add_groups(s, kClaimFields, kClaimGroups);
    s.required("blood", Type::list(i32()))
        .describe(
            "Очки кровного родства по степени 1...5 (пятая — «пять и далее»); с ростом степени не растут.")
        .required("legal", Type::list(i32()))
        .describe("Очки по месту в порядке престолонаследия E8: первое, второе, третье; не растут.");
    return s;
}

ObjectSchema emperor_project_schema() {
    ObjectSchema s{std::string(kEmperorProjectKind)};
    s.intern_last();
    s.describe_kind(
        "Проект императора (throne-crisis-model §8.5): уровни, пороги счёта и эффекты по уровням. "
        "Ровно три объекта: война (slot 1), налоги (2), расследования и чистки (3).");
    s.required("slot", Type::integer(1, 3))
        .describe("Стабильный номер проекта: 1 война, 2 налоги, 3 чистки; уникален.")
        .required("name_key", Type::string())
        .describe("Ключ строки локализации с названием проекта.")
        .required("desc_key", Type::string())
        .describe("Ключ строки локализации с описанием проекта.")
        .required("level_min", Type::integer(-4, 0))
        .describe("Наименьший уровень проекта (налоги: -2, остальные 0).")
        .required("level_max", Type::integer(1, 7))
        .describe("Наибольший уровень проекта.")
        .required("thresholds", Type::list(i32()))
        .describe("Пороги счёта между соседними уровнями: level_max - level_min возрастающих чисел; "
                  "уровень = level_min + число порогов, которые счёт достиг.")
        .required("response_pivot", Type::integer(0, 100))
        .describe("Стабильность, ниже которой растёт реакция императора на давление.")
        .required("response_k_pm", i32())
        .describe("Реакция = response_k_pm * max(0, pivot - стабильность) / 1000; отрицательная смягчает "
                  "(война, налоги), положительная ужесточает (чистки).")
        .required("effects", Type::map(Type::list(i64())))
        .describe("Эффекты по уровням: имя = список чисел по одному на уровень от level_min. Имена "
                  "фиксированы для проекта (война: war_fatigue_x100, recruit_quota_mult, army_upkeep_kop; "
                  "налоги: per_weight_kop; чистки: purge_heat_x100, court_upkeep_kop, punish_tier).");
    return s;
}

}  // namespace

const std::vector<CrisisBalanceField>& crisis_balance_fields() {
    return kBalanceFields;
}

const std::vector<CrisisBalanceField>& claim_rule_fields() {
    return kClaimFields;
}

void add_crisis_schemas(SchemaSet& set) {
    set.add(crisis_balance_schema());
    set.add(claim_rules_schema());
    set.add(emperor_project_schema());
}

}  // namespace pat::data
