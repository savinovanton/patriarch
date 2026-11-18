<!-- GENERATED FILE. Do not edit by hand: run `schemadoc --catalog --write docs/dev/event-catalog-reference.md` (source of truth: the registrations in src/sim/events/catalog_*.cpp). -->

# Справочник каталога движка событий

> **Файл сгенерирован, не редактируйте его вручную:** `schemadoc --catalog --write docs/dev/event-catalog-reference.md`. `ctest` (`schemadoc_catalog_check`) сравнивает файл с выводом генератора. Спецификация и правила нумерации — [event-engine.md](event-engine.md) §8; язык правил — §6.

Столбцы: **Код** — стабильный числовой код записи (не меняется и не переиспользуется); **Области** — где запись допустима без перехода; **Аргументы** — подписи аргументов в блоке `имя = { аргумент = … }` (главный аргумент можно писать как `имя = значение`); **Ключи тултипа** — шаблоны на русском в `data/loc/ru/events/` (`.not` — предикат с `= no`, `.fail` — причина недоступности варианта); **Пример** — запись в файле события. Запись с пометкой «заглушка» уже допустима в файлах событий, но система за ней ещё не создана: вычислитель отвечает «нет данных» (значение 0).

## Предикаты (триггеры) (18)

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
| 1200 | `has_office` | character | `type`: ссылка `office:` (необяз.) | `tooltip.trigger.has_office`, `tooltip.trigger.has_office.not`, `tooltip.trigger.has_office.fail` | `has_office = yes` | заглушка до E10: «нет данных» |

## Значения (15)

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

