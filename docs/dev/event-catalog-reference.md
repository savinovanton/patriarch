<!-- GENERATED FILE. Do not edit by hand: run `schemadoc --catalog --write docs/dev/event-catalog-reference.md` (source of truth: the registrations in src/sim/events/catalog_*.cpp). -->

# Справочник каталога движка событий

> **Файл сгенерирован, не редактируйте его вручную:** `schemadoc --catalog --write docs/dev/event-catalog-reference.md`. `ctest` (`schemadoc_catalog_check`) сравнивает файл с выводом генератора. Спецификация и правила нумерации — [event-engine.md](event-engine.md) §8; язык правил — §6.

Столбцы: **Код** — стабильный числовой код записи (не меняется и не переиспользуется); **Области** — где запись допустима без перехода; **Аргументы** — подписи аргументов в блоке `имя = { аргумент = … }` (главный аргумент можно писать как `имя = значение`); **Ключи тултипа** — шаблоны на русском в `data/loc/ru/events/` (`.not` — предикат с `= no`, `.fail` — причина недоступности варианта); **Пример** — запись в файле события. Запись с пометкой «заглушка» уже допустима в файлах событий, но система за ней ещё не создана: вычислитель отвечает «нет данных» (значение 0).

## Предикаты (триггеры) (27)

| Код | Имя | Области | Аргументы | Ключи тултипа | Пример | Примечание |
|---:|---|---|---|---|---|---|
| 1001 | `is_adult` | character | — | `tooltip.trigger.is_adult`, `tooltip.trigger.is_adult.not`, `tooltip.trigger.is_adult.fail` | `is_adult = yes` | — |
| 1002 | `is_female` | character | — | `tooltip.trigger.is_female`, `tooltip.trigger.is_female.not`, `tooltip.trigger.is_female.fail` | `is_female = no` | — |
| 1003 | `is_married` | character | — | `tooltip.trigger.is_married`, `tooltip.trigger.is_married.not`, `tooltip.trigger.is_married.fail` | `is_married = yes` | — |
| 1004 | `is_pregnant` | character | — | `tooltip.trigger.is_pregnant`, `tooltip.trigger.is_pregnant.not`, `tooltip.trigger.is_pregnant.fail` | `is_pregnant = yes` | — |
| 1005 | `is_head_of_dynasty` | character | — | `tooltip.trigger.is_head_of_dynasty`, `tooltip.trigger.is_head_of_dynasty.not`, `tooltip.trigger.is_head_of_dynasty.fail` | `is_head_of_dynasty = yes` | — |
| 1006 | `is_heir` | character | — | `tooltip.trigger.is_heir`, `tooltip.trigger.is_heir.not`, `tooltip.trigger.is_heir.fail` | `is_heir = yes` | — |
| 1007 | `is_tonsured` | character | — | `tooltip.trigger.is_tonsured`, `tooltip.trigger.is_tonsured.not`, `tooltip.trigger.is_tonsured.fail` | `is_tonsured = no` | — |
| 1008 | `is_widowed` | character | — | `tooltip.trigger.is_widowed`, `tooltip.trigger.is_widowed.not`, `tooltip.trigger.is_widowed.fail` | `is_widowed = yes` | — |
| 1009 | `has_trait` | character | `trait`: ссылка `trait:` | `tooltip.trigger.has_trait`, `tooltip.trigger.has_trait.not`, `tooltip.trigger.has_trait.fail` | `has_trait = greedy` | — |
| 1010 | `has_education` | character | `focus`: stewardship \| diplomacy \| martial \| intrigue \| learning | `tooltip.trigger.has_education`, `tooltip.trigger.has_education.not`, `tooltip.trigger.has_education.fail` | `has_education = martial` | — |
| 1011 | `same_dynasty` | character, dynasty | `target`: область | `tooltip.trigger.same_dynasty`, `tooltip.trigger.same_dynasty.not`, `tooltip.trigger.same_dynasty.fail` | `same_dynasty = scope:rival` | — |
| 1012 | `is_alive` | character | — | `tooltip.trigger.is_alive`, `tooltip.trigger.is_alive.not`, `tooltip.trigger.is_alive.fail` | `is_alive = yes` | — |
| 1013 | `has_living_child` | character | — | `tooltip.trigger.has_living_child`, `tooltip.trigger.has_living_child.not`, `tooltip.trigger.has_living_child.fail` | `has_living_child = yes` | — |
| 1014 | `is_regent` | character | — | `tooltip.trigger.is_regent`, `tooltip.trigger.is_regent.not`, `tooltip.trigger.is_regent.fail` | `is_regent = yes` | — |
| 1015 | `is_betrothed` | character | — | `tooltip.trigger.is_betrothed`, `tooltip.trigger.is_betrothed.not`, `tooltip.trigger.is_betrothed.fail` | `is_betrothed = yes` | — |
| 1016 | `is_disowned` | character | — | `tooltip.trigger.is_disowned`, `tooltip.trigger.is_disowned.not`, `tooltip.trigger.is_disowned.fail` | `is_disowned = no` | — |
| 1035 | `has_succession_law` | dynasty | `law`: majorat \| minorat \| partition \| family_council \| appointment | `tooltip.trigger.has_succession_law`, `tooltip.trigger.has_succession_law.not`, `tooltip.trigger.has_succession_law.fail` | `has_succession_law = majorat` | — |
| 1101 | `has_flag` | любая | `flag`: имя | `tooltip.trigger.has_flag`, `tooltip.trigger.has_flag.not`, `tooltip.trigger.has_flag.fail` | `has_flag = hushed_theft` | — |
| 1102 | `has_global_flag` | любая | `flag`: имя | `tooltip.trigger.has_global_flag`, `tooltip.trigger.has_global_flag.not`, `tooltip.trigger.has_global_flag.fail` | `has_global_flag = poltava_won` | — |
| 1103 | `has_modifier` | любая | `modifier`: имя (event_modifier) | `tooltip.trigger.has_modifier`, `tooltip.trigger.has_modifier.not`, `tooltip.trigger.has_modifier.fail` | `has_modifier = estate_audit` | — |
| 1104 | `has_fired` | любая | `name`: имя | `tooltip.trigger.has_fired`, `tooltip.trigger.has_fired.not`, `tooltip.trigger.has_fired.fail` | `has_fired = theft_found` | — |
| 1108 | `random_chance` | любая | `percent`: число | `tooltip.trigger.random_chance`, `tooltip.trigger.random_chance.not`, `tooltip.trigger.random_chance.fail` | `random_chance = 12.5` | — |
| 1117 | `has_building` | province | `building`: ссылка `building:` | `tooltip.trigger.has_building`, `tooltip.trigger.has_building.not`, `tooltip.trigger.has_building.fail` | `has_building = mill` | — |
| 1120 | `is_player_controlled` | character, dynasty, province | — | `tooltip.trigger.is_player_controlled`, `tooltip.trigger.is_player_controlled.not`, `tooltip.trigger.is_player_controlled.fail` | `is_player_controlled = no` | — |
| 1121 | `has_cooldown` | любая | `name`: имя | `tooltip.trigger.has_cooldown`, `tooltip.trigger.has_cooldown.not`, `tooltip.trigger.has_cooldown.fail` | `has_cooldown = theft_found` | — |
| 1125 | `has_quota` | dynasty | `kind`: any \| money \| recruits (необяз.) | `tooltip.trigger.has_quota`, `tooltip.trigger.has_quota.not`, `tooltip.trigger.has_quota.fail` | `has_quota = recruits` | — |
| 1200 | `has_office` | character | `type`: ссылка `office:` (необяз.) | `tooltip.trigger.has_office`, `tooltip.trigger.has_office.not`, `tooltip.trigger.has_office.fail` | `has_office = yes` | заглушка до E10: «нет данных» |

## Значения (31)

| Код | Имя | Области | Аргументы | Ключи тултипа | Пример | Примечание |
|---:|---|---|---|---|---|---|
| 1021 | `age` | character | — | `tooltip.trigger.age`, `tooltip.trigger.age.fail` | `age >= 16` | — |
| 1022 | `skill` | character | вид: stewardship \| diplomacy \| martial \| intrigue \| learning | `tooltip.trigger.skill`, `tooltip.trigger.skill.fail` | `skill.martial >= 8` | — |
| 1023 | `health` | character | — | `tooltip.trigger.health`, `tooltip.trigger.health.fail` | `health < 30` | — |
| 1024 | `stress` | character | — | `tooltip.trigger.stress`, `tooltip.trigger.stress.fail` | `stress >= 60` | — |
| 1025 | `piety` | character | — | `tooltip.trigger.piety`, `tooltip.trigger.piety.fail` | `piety >= 50` | — |
| 1026 | `opinion_of` | character | `target`: область character | `tooltip.trigger.opinion_of`, `tooltip.trigger.opinion_of.fail` | `opinion_of = { target = scope:governor  value >= 10 }` | — |
| 1027 | `kinship_degree` | character | `target`: область character | `tooltip.trigger.kinship_degree`, `tooltip.trigger.kinship_degree.fail` | `kinship_degree = { target = scope:rival  value <= 3 }` | — |
| 1028 | `fertility` | character | — | `tooltip.trigger.fertility`, `tooltip.trigger.fertility.fail` | `fertility >= 40` | — |
| 1029 | `ambition` | character | — | `tooltip.trigger.ambition`, `tooltip.trigger.ambition.fail` | `ambition >= 30` | — |
| 1030 | `loyalty_head` | character | — | `tooltip.trigger.loyalty_head`, `tooltip.trigger.loyalty_head.fail` | `loyalty_head < 0` | — |
| 1031 | `honor` | dynasty | — | `tooltip.trigger.honor`, `tooltip.trigger.honor.fail` | `honor >= 30` | — |
| 1032 | `unity` | dynasty | — | `tooltip.trigger.unity`, `tooltip.trigger.unity.fail` | `unity < 40` | — |
| 1033 | `prestige` | dynasty | — | `tooltip.trigger.prestige`, `tooltip.trigger.prestige.fail` | `prestige >= 50` | — |
| 1034 | `treasury` | dynasty | — | `tooltip.trigger.treasury`, `tooltip.trigger.treasury.fail` | `treasury >= 500` | — |
| 1036 | `child_count` | character | — | `tooltip.trigger.child_count`, `tooltip.trigger.child_count.fail` | `child_count >= 3` | — |
| 1105 | `chain_step` | любая | `chain`: имя | `tooltip.trigger.chain_step`, `tooltip.trigger.chain_step.fail` | `chain_step = { chain = theft_story  value >= 2 }` | — |
| 1106 | `variable` | любая | `name`: имя | `tooltip.trigger.variable`, `tooltip.trigger.variable.fail` | `variable.debt_count >= 3` | — |
| 1107 | `global_variable` | любая | `name`: имя | `tooltip.trigger.global_variable`, `tooltip.trigger.global_variable.fail` | `global_variable.levy_year >= 1` | — |
| 1109 | `current_year` | любая | — | `tooltip.trigger.current_year`, `tooltip.trigger.current_year.fail` | `current_year >= 1709` | — |
| 1110 | `current_month` | любая | — | `tooltip.trigger.current_month`, `tooltip.trigger.current_month.fail` | `current_month <= 3` | — |
| 1111 | `date` | любая | — | `tooltip.trigger.date`, `tooltip.trigger.date.fail` | `date >= 1703.05.16` | — |
| 1112 | `unrest` | province | — | `tooltip.trigger.unrest`, `tooltip.trigger.unrest.fail` | `unrest >= 40` | — |
| 1113 | `control` | province | — | `tooltip.trigger.control`, `tooltip.trigger.control.fail` | `control < 30` | — |
| 1114 | `corruption` | province | — | `tooltip.trigger.corruption`, `tooltip.trigger.corruption.fail` | `corruption > 0.25` | — |
| 1115 | `population` | province | — | `tooltip.trigger.population`, `tooltip.trigger.population.fail` | `population >= 50000` | — |
| 1116 | `recruit_pool` | province | — | `tooltip.trigger.recruit_pool`, `tooltip.trigger.recruit_pool.fail` | `recruit_pool >= 500` | — |
| 1118 | `unrest_state` | province | — | `tooltip.trigger.unrest_state`, `tooltip.trigger.unrest_state.fail` | `unrest_state >= 2` | — |
| 1119 | `development` | province | — | `tooltip.trigger.development`, `tooltip.trigger.development.fail` | `development >= 3` | — |
| 1122 | `group_size` | province | вид: nobility \| clergy \| townsfolk \| peasants \| cossacks \| natives | `tooltip.trigger.group_size`, `tooltip.trigger.group_size.fail` | `group_size.peasants >= 20000` | — |
| 1123 | `group_mood` | province | вид: nobility \| clergy \| townsfolk \| peasants \| cossacks \| natives | `tooltip.trigger.group_mood`, `tooltip.trigger.group_mood.fail` | `group_mood.nobility < -20` | — |
| 1124 | `debt` | dynasty | — | `tooltip.trigger.debt`, `tooltip.trigger.debt.fail` | `debt > 1000` | — |

## Эффекты (33)

| Код | Имя | Области | Аргументы | Ключи тултипа | Пример | Примечание |
|---:|---|---|---|---|---|---|
| 2001 | `change_honor` | dynasty | `value`: число | `tooltip.effect.change_honor` | `change_honor = -5` | — |
| 2002 | `change_unity` | dynasty | `value`: число | `tooltip.effect.change_unity` | `change_unity = 2.5` | — |
| 2003 | `change_prestige` | dynasty | `value`: число | `tooltip.effect.change_prestige` | `change_prestige = 10` | — |
| 2004 | `change_opinion` | character | `target`: область character; `value`: число; `reason`: число (необяз.) | `tooltip.effect.change_opinion` | `change_opinion = { target = scope:rival  value = -20 }` | — |
| 2005 | `add_trait` | character | `trait`: ссылка `trait:` | `tooltip.effect.add_trait` | `add_trait = greedy` | — |
| 2006 | `remove_trait` | character | `trait`: ссылка `trait:` | `tooltip.effect.remove_trait` | `remove_trait = greedy` | — |
| 2007 | `die` | character | `cause`: old_age \| illness \| childbirth \| war \| execution \| murder \| accident \| wounds \| grief | `tooltip.effect.die` | `die = illness` | — |
| 2008 | `plan_death` | character | `date`: дата (необяз.); `years`: число (необяз.) | `tooltip.effect.plan_death` | `plan_death = { years = 2 }` | — |
| 2009 | `set_education` | character | `focus`: stewardship \| diplomacy \| martial \| intrigue \| learning; `tutor`: область character (необяз.) | `tooltip.effect.set_education` | `set_education = { focus = martial  tutor = scope:tutor }` | — |
| 2010 | `conceive` | character | `father`: область character | `tooltip.effect.conceive` | `conceive = { father = scope:lover }` | — |
| 2011 | `create_person` | dynasty | `sex`: male \| female; `age`: число; `father`: область character (необяз.); `mother`: область character (необяз.) | `tooltip.effect.create_person` | `create_person = { sex = female  age = 16 }` | — |
| 2012 | `end_marriage` | character | `reason`: annulment \| tonsure | `tooltip.effect.end_marriage` | `end_marriage = annulment` | — |
| 2013 | `change_skill` | character | `skill`: stewardship \| diplomacy \| martial \| intrigue \| learning; `delta`: число | `tooltip.effect.change_skill` | `change_skill = { skill = martial  delta = 2 }` | — |
| 2014 | `change_health` | character | `delta`: число | `tooltip.effect.change_health` | `change_health = -10` | — |
| 2015 | `change_stress` | character | `delta`: число | `tooltip.effect.change_stress` | `change_stress = 15` | — |
| 2016 | `record_chronicle` | dynasty | `key`: имя; `a`: область character (необяз.); `b`: область character (необяз.) | `tooltip.effect.record_chronicle` | `record_chronicle = { key = duel  a = root  b = scope:rival }` | — |
| 2017 | `betroth` | dynasty | `groom`: область character; `bride`: область character; `dowry`: число (необяз.); `dispensation`: yes/no (необяз.) | `tooltip.effect.betroth` | `betroth = { groom = scope:groom  bride = scope:bride  dowry = 300 }` | — |
| 2018 | `tonsure` | character | — | `tooltip.effect.tonsure` | `tonsure = yes` | — |
| 2019 | `set_heir` | dynasty | `heir`: область character | `tooltip.effect.set_heir` | `set_heir = scope:son` | — |
| 2020 | `disown` | character | — | `tooltip.effect.disown` | `disown = yes` | — |
| 2021 | `grant_land` | dynasty | `character`: область character; `province`: область province | `tooltip.effect.grant_land` | `grant_land = { character = scope:son  province = scope:estate }` | заглушка до E8: «нет данных» |
| 2022 | `assign_regent` | dynasty | `regent`: область character; `kind`: mother \| guardian \| uncle \| council \| imperial | `tooltip.effect.assign_regent` | `assign_regent = { regent = scope:uncle  kind = uncle }` | заглушка до E8: «нет данных» |
| 2023 | `dismiss_regent` | dynasty | `reason`: majority \| dismissed \| regent_died | `tooltip.effect.dismiss_regent` | `dismiss_regent = dismissed` | заглушка до E8: «нет данных» |
| 2024 | `resolve_succession_dispute` | dynasty | `winner`: область character | `tooltip.effect.resolve_succession_dispute` | `resolve_succession_dispute = { winner = scope:claimant }` | заглушка до E8: «нет данных» |
| 2101 | `set_province_owner` | province | `owner`: область dynasty | `tooltip.effect.set_province_owner` | `set_province_owner = scope:rival_house` | — |
| 2102 | `assign_administrator` | province | `administrator`: область character (необяз.); `dismiss`: yes/no (необяз.) | `tooltip.effect.assign_administrator` | `assign_administrator = scope:steward` | — |
| 2103 | `change_unrest` | province | `value`: число | `tooltip.effect.change_unrest` | `change_unrest = 10` | — |
| 2104 | `change_control` | province | `value`: число | `tooltip.effect.change_control` | `change_control = -5` | — |
| 2105 | `change_group_mood` | province | `group`: nobility \| clergy \| townsfolk \| peasants \| cossacks \| natives; `delta`: число | `tooltip.effect.change_group_mood` | `change_group_mood = { group = peasants  delta = -8 }` | — |
| 2106 | `pay` | dynasty | `value`: число | `tooltip.effect.pay` | `pay = 120` | — |
| 2107 | `receive` | dynasty | `value`: число | `tooltip.effect.receive` | `receive = 50.5` | — |
| 2108 | `start_construction` | province | `building`: ссылка `building:` | `tooltip.effect.start_construction` | `start_construction = mill` | — |
| 2109 | `conscript` | province | `count`: число | `tooltip.effect.conscript` | `conscript = 200` | — |

## Списки итераторов (6)

Список перебирают `any_<список>`, `all_<список>`, `count_<список>` в условиях и значениях, `every_`, `random_`, `ordered_` в эффектах. **Граница** — лимит fan-out: итератор получает не больше min(граница, 256) элементов в порядке §5.2 (по `id` ↑, дети и братья-сёстры — по дате рождения); длиннее — список усекается, растёт счётчик `truncated`. Перебора всего мира нет (`EV107`).

| Код | Имя | Из | Элементы | Граница | Пример |
|---:|---|---|---|---:|---|
| 1 | `living_member` | dynasty | character | 256 | `any_living_member = { count >= 2 … }` |
| 2 | `child` | character | character | 32 | `any_child = { count >= 2 … }` |
| 3 | `sibling` | character | character | 32 | `any_sibling = { count >= 2 … }` |
| 4 | `owned_province` | dynasty | province | 128 | `any_owned_province = { count >= 2 … }` |
| 5 | `realm_dynasty` | realm | dynasty | 128 | `any_realm_dynasty = { count >= 2 … }` |
| 6 | `claimant` | dynasty | character | 4 | `any_claimant = { count >= 2 … }` |

