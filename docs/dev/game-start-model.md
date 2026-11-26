# Модель старта партии, сценариев и целей (E30-T01)

> Статус: **спецификация v1** (2026-11-26), задача E30-T01 ([#425](https://github.com/savinovanton/patriarch/issues/425)), [ADR-0016](../adr/0016-game-start.md). Таблицы мира — E30-T02, сейв и хеш — T03, формат сценария и загрузчик — T04, люди в данных — T05, стартовое хозяйство и двор — T06, данные короны и родов — T07, T08, свой род — T09, запуск партии — T10, цели — T11, счёт — T12, конец партии — T13, ключевые решения и итоги — T14, мост и окна — T15…T17, баланс и приёмка — T18…T20. Документ — контракт для E30-T02…T20 и точка согласования с E7, E8, E9, E10, E11, E12, E13, E15, E16, E17, E31 и меню UI-D12.
> Опирается на: [эпик E30](../epics/E30-game-start-goals.md) (решения §3, истории US-30.1…30.8), ТЗ FR-DYN-1, FR-DYN-8, FR-GOAL-1…3, FR-TIME-3, FR-SAVE-1, NFR-DET-1, NFR-PERF-6; [GDD 08](../gdd/08-events-content.md) §8 (сценарии), [GDD 01](../gdd/01-dynasty.md) §2.2, §13.2; [ADR-0001](../adr/0001-stack.md), [ADR-0006](../adr/0006-character-storage.md) (D5), [ADR-0011](../adr/0011-saves.md) и [save-format.md](save-format.md) (реестры), [event-engine.md](event-engine.md) (§8 каталоги, §10.3 `player_dynasties`), [court-model.md](court-model.md) (§7 команды, `listed`), [throne-crisis-model.md](throne-crisis-model.md) (§4.1 `Realm`, §10), [army-model.md](army-model.md) §11.7, [ai-model.md](ai-model.md) (`SetAiOptions`, `ConfigureAgent`, `ai_agents.pd`), [diplomacy-model.md](diplomacy-model.md) (`SignTreaty`, `relations.pd`), [religion-model.md](religion-model.md) (`faiths.pd`), [influence-model.md](influence-model.md), код `src/sim`, `src/content` (`session_assembly.hpp`, `m1_scenario.hpp`), `src/game` (`save_loader.hpp`), `src/ui` (`menu.hpp`) на 2026-11-26; решения владельца [#362](https://github.com/savinovanton/patriarch/issues/362), [#363](https://github.com/savinovanton/patriarch/issues/363), [#364](https://github.com/savinovanton/patriarch/issues/364), [#668](https://github.com/savinovanton/patriarch/issues/668) A, [#670](https://github.com/savinovanton/patriarch/issues/670) B, рейтинг 16+ ([#583](https://github.com/savinovanton/patriarch/issues/583)), упрощённые E15/E16/E17/E31 в M1 (2026-11-22/25).
> Числа с пометкой `[предл.]` — предложения: они живут в данных (`data/balance/goals.pd`, `data/scenarios/m1/*.pd`), а не в коде и не в этом документе как «истина».

## 1. Что здесь решено и что нет

Документ фиксирует:
1. слои и поток «новая партия → подготовка мира → игра» (§3, §10);
2. формат сценария: главный файл, включаемые файлы, компактное описание поколений, проверки (§4, §5);
3. порядок построения мира и команды дня 0 (§5.4, §10.2);
4. `GameSetup` и таблицы мира: игрок, сценарий, цели, статус партии, журнал ключевых решений, годовые снимки счёта (§6);
5. основание своего рода (§7), цели и их проверку (§8), счёт династии (§9), конец партии (§11), итоговую сводку (§12);
6. команды 168…175 и коды отказа (§13), каталог E11 (§14), `Breakdown` (§15), сейв и хеш (§16), golden (§17);
7. контракты с соседями и список их узких правок (§18), границу M1/M2 (§19), открытые вопросы (§20), поправки задач и оценку (§21).

Не решается: исторические даты Романовых (`[проверить]`, T07, ревью владельца), состав семей вымышленных родов (T08, ревью владельца), калибровка весов счёта и баланс своего рода (T12, T18), тексты локализации (`scen.*`, `goal.*`, `game.*`), вид окон (борды доски 24–27, 45–55, 73, 74, UI-D11 [#468](https://github.com/savinovanton/patriarch/issues/468)).

### 1.1 Сводка решений

| # | Решение | Где |
|---|---|---|
| G1 | Слои: `src/sim/game` (таблицы, правила, системы, команды, `dynasty_score`), `src/content/scenario_*` (формат, загрузчик, построитель людей, сборка мира), `src/game/start_game` и `src/game/final_summary` (запуск, итоги, мосты); публичный `pat/sim/game/query.hpp` | §3 |
| G2 | Ключ и каталог сценария M1 — **`m1`** (`data/scenarios/m1/`), имя «Боярский заговор» — ключ `scen.m1.name`; сценарии M2 получают говорящие ключи | §4.1, F1 |
| G3 | Сценарий — `scenario.pd` + включаемые файлы по родам и доменам; люди — компактные поколения с локальными ключами; никаких людей в коде | §4, §5 |
| G4 | Мир сценария строится детерминированно из `(сценарий, seed)` **без** игрока; выбор игрока — **внешние команды дня 0** (`SetAiOptions`, `SetPlayerDynasty` или `FoundDynasty`, `SetGoal`), они пишутся в реплей | §5.4, §10.2 |
| G5 | `GameSetup` создаётся только командой дня 0; мир без него — «наблюдатель» и хешируется бит-в-бит как раньше | §6.1 |
| G6 | Единственный источник «кто игрок» — `GameSetup.player`; список `player_dynasties` раздела 48 E11 мигрирует в T03 и дальше пишется пустым | §6.5, F3 |
| G7 | Пять целей — типы в коде с параметрами в данных; основная + дополнительная; смена раз в 60 мес., множитель счёта 900‰ за смену; детали — E30-Q4 ([#776](https://github.com/savinovanton/patriarch/issues/776)) | §8 |
| G8 | Счёт династии — чистая функция с `Breakdown` (богатство, титулы и должности, потомство, влияние, вера, бонусы целей, множитель смен) и годовыми снимками | §9 |
| G9 | Свой род: `FoundDynasty` (168) с текстами игрока, именем из пула, «свободной» провинцией (E30-Q5, [#777](https://github.com/savinovanton/patriarch/issues/777)) и режимом семьи «с женой и детьми» / «один» (#363) | §7 |
| G10 | Конец партии: `Lost` (вымирание, FR-DYN-8), `Won` (основная цель), `TimeUp` (1780-01-01, FR-TIME-3), `Sandbox`; ворота команд игрока после конца | §11 |
| G11 | `KeyDecisionLog`: 64 записи с вытеснением наименее весомой, снимок «до» и «через 12 мес.»; источники — журнал команд и эффект события `record_key_decision` | §12.1 |
| G12 | Теги 168…174 поимённо (175 резерв); разделы 84…87 (88…91 резерв); `ExtPart` **64 `Game`** (65 резерв); ключи сводки 28 `game_hash`, 29 `game_status` | §13, §16 |
| G13 | on-action 160…163 (164…169 резерв), триггеры 1430…1437 (1438…1449 резерв), эффект 2520 (2521…2539 резерв), `Breakdown` 22000…22999, `GameDenial` | §13.3, §14, §15 |
| G14 | `kExtPartCount` 64 → 128 поднимает **первая** из E30-T03 и E16-T03 (по очереди W это E30-T03); формат реплея не меняется | §16.2 |
| G15 | Сложность ИИ (#670 B) — поле `MenuStart`, команда E17 `SetAiOptions` (196) дня 0, хранится в `AiMeta`, не в `GameSetup` | §10.2, §18 |
| G16 | ИИ ведёт все роды, кроме игрока (#668 A): агенты настраиваются **после** команд дня 0 по `GameSetup.player`; `ai_agents.pd` перечисляет и играбельные роды | §18 |
| G17 | Одна сборка сессии `content::assemble_session` (E13-T07) для новой партии и загрузки; E30-T10 расширяет `SessionContent` правилами партии, а не пишет вторую сборку | §10.3 |

## 2. Что есть в коде и находки

### 2.1 Что E30 берёт готовым

| Что | Где | Как используется |
|---|---|---|
| `World::create_dynasty`, `dynasty_extinct`, `ChronicleKind::Extinction`, `CreatePerson` (9), `Marry` (7), `SetDynastyHead` (4), `SetProvinceOwner` (1), `AdjustHonor` (5), `AdjustPrestige` (20), `AdjustUnity` (17), `RecordChronicle` (18) | E1, E7 (`world.hpp`, `command.hpp`) | построитель людей, свой род, вымирание, летопись |
| `validate`/`apply(World&, const Command&, const CommandContent&)`, `Command::actor` (невалидный — система), `CommandError{reason, code, detail}` | `command.hpp` | команды установки сценария и дня 0, коды `GameDenial` |
| Пулы имён и отчеств, выбор имени (E7-T09), `CharacterInit` | E7, `data/names` | имена людей сценария и семьи своего рода |
| `M1Scenario`, `load_m1_scenario`, `data/scenarios/m1/{holdings,economy_start,trade_nodes}.pd` | E9-T19, `src/content/m1_scenario.*` | переносится в формат сценария (T04) без смены хешей |
| `SessionContent`, `assemble_session` | E13-T07, `session_assembly.hpp` | общая сборка новой партии и загрузки (§10.3) |
| `load_game`, `continue_game`, `SaveSummary` (ключи 3, 5, 6, 11, 12) | E13-T02/T07, `save_loader.hpp` | сводка из `GameSetup` (§16.3) |
| `MenuStart`, `MenuCustomHouse`, `MenuNewGame`, `MenuActions::start_game` | UI-D12 #284, `menu.hpp` | вход `start_new_game` (§10.1) |
| `events_meta.player_dynasties`, `on_game_start` (1), `is_player_controlled` (1120), `SetEventSettings` (99), `FireEvent` (100) | E11 ([event-engine.md](event-engine.md) §9.1, §10.3) | миграция списка игроков (§6.5), запалы сценария |
| `Realm.throne_house`, `line_house`, `reign_start`, `phase`, `stability_x100`; `SetRealmState` (144), `SetStance` (163) | E12 | цели «Пережить», «Престол»; стартовый кризис |
| `AssignOffice` (128), `AssignParty` (140), держания должностей | E10 | стартовые назначения, цель «Должность», слагаемое счёта |
| Казна, владения, развитие провинций, кредиты (E9); влияние (E7-T38); благосклонность Церкви (E31) | E9, E7, E31 | слагаемые счёта |
| `SetAiOptions` (196), `ConfigureAgent` (192) | E17 ([ai-model.md](ai-model.md) §7.1) | сложность и агенты при старте |
| `SignTreaty` (206) | E15 | стартовые договоры (`relations.pd`) |
| `economy::Breakdown` (≤ 32 узлов, глубина ≤ 3) | E9 | разложения счёта и прогресса целей |

### 2.2 Находки (расхождения документов и кода; решения — в разделах)

| # | Находка | Решение |
|---|---|---|
| F1 | Ключ сценария расходится: GDD 08 §8.1 и E12 (`throne-crisis-model.md`, эпик E12) пишут `boyar_plot`, а данные (`data/scenarios/m1/`), сводка сейва (`scenario = m1`), `simcli --scenario m1`, ai-model и religion-model — `m1` | Ключ и каталог — **`m1`** (меньше всего правок, без переименования файлов и строк в сейвах); отображаемое имя «Боярский заговор» — локализация. Пути в E12 и пример GDD 08 исправлены этим PR |
| F2 | Эпик E30 §2 пишет «поле `difficulty` не вводится в M1» и «провинция своего рода — по сиду» | Устарело: сложность выбирается в меню (#670 B, хранится в E17), провинцию выбирает игрок (#363, E30-Q5); эпик исправлен |
| F3 | Две правды о роде игрока: `events_meta.player_dynasties` (E11) и будущий `GameSetup` | T03 мигрирует список в `GameSetup` и дальше пишет его пустым; `SetEventSettings` с непустым списком отклоняется (§6.5) |
| F4 | `ai_agents.pd` (ai-model §20) «не перечисляет род игрока», но игрок выбирается после построения мира | Файл перечисляет все роды под ИИ, включая играбельные; род игрока исключается при старте по `GameSetup.player` (§18); строка ai-model исправлена |
| F5 | `ExtPart` для E30 не выдан: court-model F4 называл блок 48…55 (занят E12/E16), E16 предлагал резерв 57 | E30 берёт **64 `Game`** (65 резерв) и сам поднимает `kExtPartCount` до 128, если идёт раньше E16-T03 (§16.2); резерв E16 не трогается (save-format §5.2 п. 2) |
| F6 | Команды не умеют нести строки, а свой род требует фамилию, название рода и девиз от игрока | Новое тело `FoundDynasty` с ограниченными UTF-8 полями (§13.1); теги < 128 и прежние тела не меняются, версии `PRP1`/`PCL1` прежние |
| F7 | Цели и счёт зависят от систем, которые придут позже (E31 вера, E7-T38 влияние, E10 должности) | Слагаемое без своего домена равно 0 с узлом `Breakdown` «нет данных»; веса — данные (§9) |
| F8 | В M1 «свободных» провинций нет: все 20 имеют владельца, 4 — государевы | E30-Q5 ([#777](https://github.com/savinovanton/patriarch/issues/777)); до ответа — государевы провинции с пометкой сценария `foundable` (§7.2) |
| F9 | #364 не задаёт, какие должности «высшие», когда засчитываются «очки» и что с просроченной целью | E30-Q4 ([#776](https://github.com/savinovanton/patriarch/issues/776)); до ответа — вариант A в данных (§8) |

## 3. Слои

```
data/scenarios/m1/*.pd, data/balance/goals.pd
        │  (data → content)
src/content/scenario_*      ScenarioDefinition, load_scenario, build_people, build_scenario_world, GameRules
        │                    SessionContent::from_scenario (+ game_rules)  → assemble_session (E13-T07)
src/sim/game                GameSetup, GoalRow, KeyDecisionLog, ScoreHistory, GameRules, dynasty_score,
        │                    команды 168…174, системы game.*, query.hpp
src/game/start_game         start_new_game(MenuStart, seed, …) → Simulation (стадии подготовки)
src/game/final_summary      build_final_summary(world, rules) → FinalSummary
src/game/game_view          MenuNewGame из сценария, GoalView, FinalScreenView (мосты; читают World)
src/ui                      окна меню, «Цель партии», «Итоги партии»/«Род угас» (без зависимости от sim)
```

- `src/sim/game` знает только `sim`; его правила (`GameRules`) приходят из `content`, как `EconomyRules`.
- Публичный `pat/sim/game/query.hpp` (T02): `player_dynasty(world)`, `is_player(world, d)`, `game_status(world)`, `goal(world, slot)`, `goal_progress_ppm(world, slot)`, `founded_house(world)`, `custom_texts(world)`. Его читают E11 (`is_player_controlled`), E13 (сводка, E13-T08 [#373](https://github.com/savinovanton/patriarch/issues/373)), E17 (исключение рода игрока), мосты.

## 4. Формат сценария

### 4.1 Файлы

```
data/scenarios/m1/
  scenario.pd        главный файл (§4.2)
  romanov.pd         корона: Пётр I и ближний круг (T07, реальные люди, [проверить])
  golitsyn.pd  sheremetev.pd  menshikov.pd       играбельные роды (T08)
  naryshkin.pd  <малые роды>.pd                  ИИ-роды (T08)
  holdings.pd  economy_start.pd  trade_nodes.pd  хозяйство (E9-T19 → формат сценария, T04/T06)
  court.pd           места `listed`, стартовые назначения и партии (E10, T06)
  crisis.pd          стартовое состояние кризиса, позиции, запалы (E12-T16)
  army.pd            армии, крепости, фронт (E16, army-model §11.7)
  relations.pd       стартовые договоры и модификаторы (E15, diplomacy-model §15)
  faiths.pd          веры родов и доли провинций (E31, religion-model §17)
  ai_agents.pd       архетипы и режимы агентов (E17, ai-model §20)
```

Ключ сценария — имя каталога (`m1`); он пишется в сводку сейва (ключ 3) и в `replay_id` через `SessionContent`. Каждый включаемый файл имеет `format_version`; неизвестный ключ верхнего уровня — ошибка загрузки с `файл:строка:столбец` (общий загрузчик `.pd`, E4).

### 4.2 `scenario.pd`

```
format_version = 1
scenario "m1" {
    name_key = "scen.m1.name"          # «Боярский заговор»
    desc_key = "scen.m1.desc"
    start_date = 1703.01.01
    crown = dynasty:romanov            # род государя; не играбелен в M1
    emperor = person:romanov.peter_i
    playable = [ dynasty:golitsyn dynasty:sheremetev dynasty:menshikov ]   # #362
    custom_house = yes                 # «Основать свой род»
    foundable_provinces = [ province:p0017 province:p0018 province:p0019 ]  # E30-Q5, [предл.]
    goal_presets = [ goal:survive goal:office goal:score goal:throne goal:sandbox ]
    default_goal = goal:survive
    scored = [ dynasty:golitsyn dynasty:sheremetev dynasty:menshikov dynasty:naryshkin ]  # счёт и цель «Очки»
    includes = [ "romanov.pd" "golitsyn.pd" "sheremetev.pd" "menshikov.pd" "naryshkin.pd"
                 "holdings.pd" "economy_start.pd" "trade_nodes.pd" "court.pd" "crisis.pd"
                 "army.pd" "relations.pd" "faiths.pd" "ai_agents.pd" ]
    flags = { fuse_boyar_plot = 1 }    # глобальные флаги E11 до on_game_start
    start_events = [ event:plot.fuse_start ]   # FireEvent дня старта (E11), необязательно
}
```

Отсутствующий доменный файл допустим, если домен выключен в сборке (тогда его системы не ставятся). Номера провинций — ключи `p0001…` (#201); в M2 E21 меняет только данные.

### 4.3 Файл рода

```
format_version = 1
house "golitsyn" {
    dynasty = dynasty:golitsyn                # запись data/dynasties (имя, цвета, герб)
    honor = 300  prestige = 120  unity = 60  influence = 30          # [предл.]
    succession_law = primogeniture            # E8
    treasury_rub = 1800                       # E9 (до T06 — в economy_start.pd)
    party = party:old_nobility                # E10 AssignParty
    card = { blurb_key = "scen.m1.house.golitsyn.blurb" }            # карточка меню
    people {
        p "vasily"  { name = name:vasily  sex = male  born = 1643.02.11  head = yes
                      father_name = name:vasily  skills = { diplomacy = 14 stewardship = 11 }
                      traits = [ trait:cautious trait:ambitious ] }
        p "anna"    { name = auto  sex = female  born = 1655  spouse = vasily  married = 1672 }
        p "mikhail" { name = name:mikhail  born = 1674  father = vasily  mother = anna
                      skills = auto  heir = yes }
        p "ivan"    { born = 1699  father = mikhail  mother = golitsyn.mikhail_wife }
    }
}
```

**Компактное описание поколений (T05):**
- `p "<ключ>"` — локальный ключ, уникален в файле; ссылка из другого файла — `<род>.<ключ>` (браки между родами).
- `born` — дата или год; при одном годе день выводится чистой функцией `day_of_year = 1 + hash32(scenario, house, key) % 365` (без ГСЧ, не зависит от порядка).
- `father`/`mother` — живые люди сценария; мёртвых предков сценарий не создаёт: отчество задаёт `father_name` (имя отца из пула), иначе берётся из `father`.
- `spouse` + `married` — брак (`Marry`, дата брака — в запись E8); `name = auto` / `skills = auto` / отсутствие `traits` — выбор E7 из потока `scenario.people`, индекс — `hash32(house, key)` (порядок файла не влияет).
- `head = yes` — глава (ровно один на род), `heir = yes` — наследник (E8 `SetHeir`), `sex` по умолчанию `male`.
- Пётр I и ближний круг (`romanov.pd`) — тот же формат, все поля явные, даты `[проверить]`.

### 4.4 Проверки при загрузке (T04, T05; `contentlint`)

| Код | Проверка |
|---|---|
| SC01 | ключи файлов и ссылок существуют (`dynasty:`, `province:`, `name:`, `trait:`, `party:`, `office:`, `goal:`) |
| SC02 | каждая провинция имеет владельца (род сценария или корона) |
| SC03 | у каждого рода ровно один `head = yes`, глава жив и совершеннолетен; ≥ 1 живой член |
| SC04 | родитель старше ребёнка ≥ 15 лет (`min_parent_age` из `demography.pd`); мать моложе 50 на дату рождения |
| SC05 | брак: оба живы, совершеннолетни на дату брака, не родственники ближе, чем запрещает E8; один супруг на человека |
| SC06 | `playable` ⊆ родов сценария, корона не играбельна; `foundable_provinces` — государевы; `default_goal` ∈ `goal_presets` |
| SC07 | стартовые назначения удовлетворяют жёстким требованиям E10 (возраст, пол, черты) |
| SC08 | имена — из пулов (E7-T09) или явные id; отчество выводимо |
| SC09 | даты внутри `[1600-01-01, start_date]`; `married ≥ born + 15` |

Все ошибки собираются сразу (не первая), с `файл:строка:столбец`; корпус неверных сценариев — `tests/data/scenarios_bad/` (T04/T05).

## 5. Построение мира

### 5.1 Принцип

`content::build_scenario_world(const ScenarioDefinition&, const SessionContent&, std::uint64_t seed) → core::Result<sim::World, ScenarioError>` — чистая функция от данных и seed. Игрок в ней не участвует: тот же мир строится для меню, для `simcli` (наблюдатель) и для новой партии; выбор игрока — команды дня 0 (§10.2).

### 5.2 Порядок (фиксирован; меняется только с golden-задачей)

1. Контент: правила доменов, пулы имён, черты (готовая загрузка E4/E7/E9).
2. Роды: `create_dynasty` по `includes` в порядке списка (id детерминированы порядком).
3. Люди: `CreatePerson` (9) по родам, внутри рода — по возрастанию года рождения, затем по ключу.
4. Браки и родство: `Marry` (7), главы `SetDynastyHead` (4), наследники `SetHeir` (70), законы `ChangeSuccessionLaw` (71).
5. Честь, престиж, единство, влияние: `AdjustHonor`/`AdjustPrestige`/`AdjustUnity`/`AdjustInfluence`.
6. Хозяйство: владения, казны, население, здания — прямой API мира E9 (как сейчас `economy_start.pd`).
7. Веры (E31 `faiths.pd`) — прямой API мира E31 (религия: «строка контента — стартовое значение»).
8. Армии, крепости, фронт (E16 `army.pd`) — прямой API мира E16 (army-model §11.7).
9. Двор: `OpenVacancy`/`AssignOffice` (128), `AssignParty` (140) с системным актором (court-model §10).
10. Кризис: `SetRealmState{Init}` (144), `SetStance` (163), входы (E12-T16 `crisis.pd`).
11. Дипломатия: `SignTreaty` (206), стартовые модификаторы (E15 `relations.pd`).
12. События: глобальные флаги сценария, `SetEventSettings` (99) без списка игроков; `start_events` ставятся `FireEvent` (100) в очередь дня старта.

Агенты ИИ (E17) и `on_game_start` (E11) **не** часть построения: первые настраиваются после команд дня 0 (§18), второе выпускает диспетчер E11 в первом тике.

### 5.3 Журнал установки

Команды шагов 3–5 и 9–12 проходят `validate` → `apply` с системным актором; отказ любой из них — ошибка загрузки сценария с позицией записи данных (`ScenarioError{file, line, column, rejection, code}`), а не `PAT_ASSERT`. Принятые команды складываются в `ScenarioBuildLog` (в памяти, для `simcli --dump-setup` и тестов); в сейв и реплей он не идёт — мир установки воспроизводится из `(сценарий, seed)`.

### 5.4 Что воспроизводит реплей

`PRP1` хранит `seed`, `scenario_id` и внешние команды. Мир сценария строится заново из данных; **команды дня 0** (§10.2) — обычные внешние команды с `day = start_day`, поэтому реплей новой партии полон без нового поля в заголовке (US-30.6).

## 6. Состояние в `World` (E30-T02)

### 6.1 `GameSetup` (синглтон, раздел 84)

| Колонка | Тип | Смысл |
|---|---|---|
| `present` | `u8` 0/1 | 0 — мир-наблюдатель (нет строки в сейве, часть хеша 0) |
| `scenario_key` | UTF-8 ≤ 32 Б | ключ сценария (`m1`) |
| `start_date` | `Date` | дата команды дня 0 |
| `player` | `DynastyId` | род игрока |
| `status` | `u8` | 1 `Running`, 2 `Won`, 3 `Lost`, 4 `TimeUp`, 5 `Sandbox` (0 — нет партии) |
| `end_reason` | `u8` | 1 вымирание, 2 основная цель, 3 1780 год |
| `end_date`, `won_date` | `Date` | дата конца/победы; `kNoDate`, если нет |
| `flags` | `u8` | бит 0 род основан игроком, бит 1 итог 1780 показан, бит 2 победа была (сохраняется в песочнице) |
| `goal_changes` | `u8` | число платных смен цели (множитель счёта) |
| `last_goal_change` | `Date` | замок смены (§8.3) |
| `next_decision_seq` | `u32` ≥ 1 | счётчик `KeyDecisionLog` |
| `custom` | `{surname ≤ 48 Б, house_genitive ≤ 64 Б, motto ≤ 160 Б, coat u8, look u8}` | тексты своего рода (только при бите 0) |

Строка создаётся только `SetPlayerDynasty` или `FoundDynasty` (§13). Сложность ИИ здесь **не** хранится (она в `AiMeta`, E17).

### 6.2 Цели (раздел 85)

До двух строк `GoalRow` (24 Б): `slot` (`u8`: 0 основная, 1 дополнительная), `goal` (`u8` номер цели из данных, §8.1), `state` (`u8`: 0 активна, 1 выполнена, 2 не выполнена, 3 снята), `chosen` (`Date`), `hold_since` (`Date`; удержание должности/престола, `kNoDate` — не держит), `streak` (`u8`; подряд идущие снимки на первом месте), `progress_ppm` (`u32` 0…10⁶; пересчитывается системой `game.goals`), `resolved` (`Date`).

### 6.3 `KeyDecisionLog` (раздел 86)

До 64 записей (≤ 64 Б): `seq` (`u32`), `date`, `tag` (`u16`), `arg` (`u32`: цель команды — род/персонаж/провинция), `event` (`u32` ключ события E11 или 0), `option` (`u8`), `weight` (`u8` 1…9), `before` и `after` — снимки `{score i32, treasury_rub i32, honor i32, prestige i32, members u16, offices u8}`, `after_due` (`Date`), `flags` (`u8`: бит 0 «после» заполнено). Вытеснение — §12.1.

### 6.4 Снимки счёта (раздел 87)

Строка `{year i16, dynasty u32, score i32}` — каждый год 1 января для родов `scored` сценария и рода игрока; 1703…1780 × ≤ 12 родов ≈ 9,4 КБ. Отсортировано по `(year, dynasty)`.

### 6.5 Миграция `player_dynasties` (T03)

Шаг схемы `N → N+1`: если в разделе 48 список игроков не пуст, создаётся `GameSetup{present = 1, player = min(id), status = Running, scenario_key из сводки, start_date = дата мира}`, остальные id отбрасываются с записью в лог загрузки; список в разделе 48 пишется пустым (раскладка раздела 48 не меняется). После T03 `is_player_controlled` (1120) и E17 читают `game::is_player`; `SetEventSettings` с непустым списком отклоняется `GameDenial::PlayerListMoved` (запись в чек-лист E11, event-engine §24).

### 6.6 Инварианты (`check_invariants`, T02)

GS-1 `present = 0` ⇒ все поля по умолчанию, разделы 85…87 пусты. GS-2 `status ∈ {Running, Won, TimeUp, Sandbox}` ⇒ род `player` существует. GS-3 `status = Lost` ⇔ `end_reason = 1` и род вымер. GS-4 ≤ 1 строки цели на слот; основная есть при `present = 1`. GS-5 `Sandbox` ⇒ основная цель `sandbox`. GS-6 `KeyDecisionLog` ≤ 64, `seq` уникальны и < `next_decision_seq`. GS-7 снимки упорядочены, `year` ≤ текущего. GS-8 бит 0 `flags` ⇔ непустой `custom.surname`.

## 7. Свой род (E30-T09)

### 7.1 Что вводит игрок

Фамилия (2…24 символа), название рода в родительном падеже (2…32), девиз (0…80), имя основателя и отчество — **из пулов** (E7-T09; меню предлагает список, свободного текста в имени нет), облик (`look`), вера (`orthodox` / `old_believers`), герб из набора (`coat`), провинция из `foundable_provinces`, режим семьи. Тексты: UTF-8, буквы кириллицы и латиницы, пробел, дефис, апостроф; без управляющих символов; обрезки нет — неверное поле даёт отказ с причиной рядом с «Далее» (US-30.2).

### 7.2 Что создаёт `FoundDynasty` (#363)

| Что | Значение `[предл.]` (`goals.pd`, блок `found`) |
|---|---|
| Род | `create_dynasty(dynasty.custom)`; имя показывается из `GameSetup.custom` |
| Основатель | мужчина 30 лет, навыки `auto` (поток `game.found`), черты по «Образу» (`look` → набор черт в данных) |
| Семья | режим 1 (по умолчанию): жена 24 лет и `children = 2` ребёнка (6 и 3 года); режим 0 («с нуля»): только основатель |
| Провинция | одна из `foundable_provinces` (E30-Q5, [#777](https://github.com/savinovanton/patriarch/issues/777)), `SetProvinceOwner` от короны |
| Деньги | казна 42 000 коп. (420 ₽), без кредитов |
| Знатность | честь 50, престиж 10, единство 70, влияние 10 |
| Должности | нет |
| Вера | выбранная (E31 `DynastyFaithRow`; при выключенной религии — строка контента) |
| Игрок | `GameSetup.player` = новый род, бит 0 `flags` |

Имена жены и детей — пул E7, поток `game.found`, индекс — порядковый номер члена семьи. Баланс обоих режимов семьи — T18 (US-30.8).

## 8. Цели (E30-T11)

### 8.1 Данные `data/balance/goals.pd`

```
format_version = 1
goal "survive" { slot = 1 kind = survive deadline = 1723.01.01 max_phase = 1 min_stability = 55
                 lawful_line = yes bonus = 500 }
goal "office"  { slot = 2 kind = office offices = [ office:ambassadorial_chief office:treasurer office:general ]
                 hold_months = 12 bonus = 500 }                                    # E30-Q4 (1)
goal "score"   { slot = 3 kind = score rank = 1 snapshots = 2 not_before = 1713 bonus = 500 }  # E30-Q4 (2)
goal "throne"  { slot = 4 kind = throne hold_months = 6 bonus = 1000 }
goal "sandbox" { slot = 5 kind = sandbox }
rules { change_lock_months = 60 change_factor_pm = 900 secondary_bonus_pm = 400
        failed_free_change = yes end_date = 1780.01.01 consequence_months = 12 }   # E30-Q4 (3)
```

`slot` — стабильный номер цели (пишется в сейв); `kind` — тип, реализованный в коде (пять типов M1). Все числа `[предл.]`, рабочие значения по #364.

### 8.2 Условия (система `game.goals`, Monthly, 1-е число; плюс 1 января после снимка счёта)

| Тип | Выполнена, когда | Не выполнена, когда | Прогресс ppm (`Breakdown` 22100…22499) |
|---|---|---|---|
| `survive` | на `deadline`: `Realm.phase ≤ max_phase`, `stability ≥ min_stability`, `throne_house == line_house` (законная линия на троне) | на `deadline` условие ложно | доля прошедшего срока × (3 условия выполнены / 3) |
| `office` | живой член рода игрока держит одну из `offices` непрерывно `hold_months` (`hold_since` сбрасывается при потере) | никогда (до 1780) | месяцы удержания / `hold_months` |
| `score` | на снимке 1 января (год ≥ `not_before`) род игрока на месте ≤ `rank` среди `scored` + игрока `snapshots` раз подряд (`streak`) | никогда (до 1780) | `streak / snapshots`, при `streak = 0` — счёт / счёт лидера × ½ |
| `throne` | `Realm.throne_house == player` и `today ≥ reign_start + hold_months` | никогда (до 1780) | месяцы правления / `hold_months` |
| `sandbox` | никогда | никогда | 0 |

Ничьи в рейтинге — по `(счёт ↓, id ↑)`. Результат — внутренняя команда `ResolveGoal` (174); выполнение основной цели → `EndGame{Won}` (§11); дополнительной — бонус `bonus × secondary_bonus_pm/1000`, летопись. Невыполнение (E30-Q4 (3), до ответа вариант A): запись в летописи, партия идёт дальше, следующая смена цели бесплатна и без замка.

### 8.3 Смена и выбор

- При старте — `SetGoal{slot 0}` (обязательна; меню по умолчанию `default_goal`) и необязательная `SetGoal{slot 1}`. Дополнительную можно впервые поставить и позже, бесплатно.
- `ChangeGoal{slot, goal}` в игре: не раньше `last_goal_change + change_lock_months` (замок отсчитывается и от старта); цена — `goal_changes += 1`, множитель счёта `change_factor_pm^goal_changes` (§9); прогресс и удержание новой цели — с нуля. Исключения без цены и замка: смена после невыполненной цели (`failed_free_change`) и переход в песочницу после `Won`/`TimeUp`.
- Одна цель не может стоять в обоих слотах; `sandbox` — только основная.

## 9. Счёт династии (E30-T12)

`dynasty_score(const World&, const GameRules&, DynastyId) → Breakdown` — чистая функция, целые очки, корень 22000:

| Узел | Слагаемое | Формула `[предл.]` (веса — `goals.pd`, блок `score`) | Источник; без домена |
|---|---|---|---|
| 22001 | богатство | `(казна − долг) / 100 ₽` + `Σ development` владений × 20 | E9; 0 |
| 22002 | титулы и должности | `honor / 10` + `prestige / 5` + Σ держаний × вес должности (по умолчанию 20) | E7, E10; должности 0 |
| 22003 | потомство | живые члены × 10 + 20 при наличии наследника | E7, E8 |
| 22004 | влияние | `influence` (очки) × 1 | E7-T38; 0 |
| 22005 | вера | `(church_favor − 50) / 2` | E31; 0 |
| 22006 | бонусы целей | выполненная основная `bonus`, дополнительная `bonus × 400‰` | §8 |
| 22007 | смены цели | множитель `900‰^goal_changes` (узел-множитель) | §8.3 |

Отрицательные слагаемые допустимы, итог зажимается снизу 0. Годовой снимок (раздел 87) пишет система `game.score` 1 января (Yearly, прямой записью, ADR-0006 D5). Проверка: сумма узлов = корню точно; одинаковый мир → одинаковый счёт на всех платформах (без `float`).

## 10. Поток «новая партия → подготовка мира → игра» (E30-T10)

### 10.1 Вход

`MenuStart` (UI-D12) расширяется (T15): `ai_difficulty` (`u8`, по умолчанию «реалистичные», #670 B), `secondary_goal_id` (пусто — нет), в `MenuCustomHouse` — `province` (ключ), `family` (0/1), `coat`, а `name`/`patronymic` становятся выбором из пула. Seed выбирает хост (`src/game`, не `sim`) и передаёт явно.

### 10.2 Стадии `game::start_new_game(const MenuStart&, std::uint64_t seed, const StartOptions&, StartProgress*) → core::Result<StartedGame, StartError>`

| # | Стадия (экран «Подготовка мира») | Что происходит | Ошибка |
|---|---|---|---|
| 1 | Контент | загрузка `data/` и локализации (готовые загрузчики), `GameRules` из `goals.pd` | ошибки данных с позицией |
| 2 | Карта | PMD2 (ADR-0007) | `MapError` |
| 3 | Сценарий | `load_scenario("m1")` + проверки §4.4 | `ScenarioError` |
| 4 | Мир | `build_scenario_world(def, content, seed)` (§5) | `ScenarioError` |
| 5 | Сессия | `content::assemble_session(content, world)` — системы всех доменов в их порядке (§10.3) | — |
| 6 | Команды дня 0 | `submit` внешних команд: `SetAiOptions` (196, система; при включённом ИИ), `SetPlayerDynasty` (169) **или** `FoundDynasty` (168), `SetGoal` (170) × 1–2 — от системного актора | отказ `validate` → `StartError` с кодом `GameDenial` |
| 7 | Первый тик | один шаг симуляции на дату старта: команды применяются, E17 настраивает агентов, E11 выпускает `on_game_start`, запалы | — |
| 8 | Автосейв | начальное автосохранение (E13, `kind = 1`) | ошибка записи не мешает старту |

Отмена возможна между стадиями (как `LoadProgress` E13-T07). Партия открывается на паузе после первого тика; бюджет стадий 1–7 — в пределах загрузки ≤ 15 с (NFR-PERF-6, T20).

### 10.3 Одна сборка (E13-T07 ↔ T10)

`SessionContent` получает `std::shared_ptr<const sim::game::GameRules> game_rules` и правила остальных доменов, которые ещё не внутри (`court`, `crisis`, `army`, `diplomacy`, `religion`, `ai` — каждый эпик добавляет своё поле своей задачей); `from_scenario` заполняет всё из `ScenarioDefinition`. `assemble_session` ставит `install_game_systems` (§11.3) так же для загрузки, поэтому «Новая партия» и «Загрузить» не расходятся (save-format §2.4 п. 9). Тест T10: мир новой партии, сохранённый и загруженный, даёт тот же хеш через 365 тиков, что и непрерывный прогон.

### 10.4 `simcli`

`--scenario m1` без игрока — наблюдатель (как сейчас; `GameSetup` не создаётся, хеши `simcli_m1` не двигаются от T10). `--player golitsyn --goal survive [--secondary office] [--found <файл.pd>] [--ai-difficulty 1]` подаёт те же команды дня 0, что меню (T10; нужно T18/T19).

## 11. Конец партии и ворота команд (E30-T13)

### 11.1 Статусы

```
            SetPlayerDynasty/FoundDynasty (основная ≠ sandbox)
  (нет) ───────────────────────────────────────────────► Running
    │  (основная = sandbox)                                │  основная цель выполнена ─► Won ──┐
    └──────────────────────────────► Sandbox ◄─────────────┤  1780-01-01 ───────────────► TimeUp ┤
                                       ▲                   │                                   │
                                       └── ChangeGoal{0, sandbox} (без цены) ◄──────────────────┘
  Running / Won / TimeUp / Sandbox ── род игрока вымер ──► Lost (конечный)
```

- `game.end` (Daily, после доменных систем): вымирание (`dynasty_extinct(player)`, E7-T15) → `EndGame{Lost, 1}`; основная цель выполнена (`ResolveGoal` этого тика) → `EndGame{Won, 2}`; дата ≥ `end_date` и бит 1 не стоит → `EndGame{TimeUp, 3}` (в `Sandbox` — только итоговая сводка без смены статуса). Потеря владений и смерть главы с наследником партию **не** завершают (FR-DYN-8).
- `EndGame` (172, внутр.) пишет статус, `end_date`, летопись (`RecordChronicle`, шаблоны `chronicle.game.*`, E11-T16) и вызывает on-action 163.

### 11.2 Ворота команд

В общем `validate` (одна проверка, T13): внешняя команда с `actor == GameSetup.player` при `status ∈ {Won, TimeUp, Lost}` отклоняется `Rejection::InvalidState` с кодом `GameDenial::GameOver`, кроме `ChangeGoal{0, sandbox}` в `Won`/`TimeUp`. Системы и ИИ-роды продолжают работать (UI держит паузу на экране итогов). После `Lost` партия только завершается или загружается.

### 11.3 Системы E30 (`install_game_systems(simulation, rules)`)

| Система | Расписание | Что делает |
|---|---|---|
| `game.key_decisions` | Daily | разбирает `TickContext::applied_commands` → `RecordKeyDecision` (173); заполняет «после» у записей с `after_due ≤ today` |
| `game.goals` | Monthly (1-е) | условия §8.2, `progress_ppm`, `ResolveGoal` (174) |
| `game.score` | Yearly (1 января) | снимки раздела 87, затем шаг `score` целей |
| `game.end` | Daily | §11.1 |

Порядок: после всех доменных систем (E7…E12, E9 `economy.settlement`, E16, E15, E31), **до** `events.dispatch` (E11) и `install_ai_systems` (E17, последние). В мире без `GameSetup` все четыре — пустой проход (хеш прежний).

## 12. Ключевые решения и итоговая сводка (E30-T14)

### 12.1 `KeyDecisionLog`

- Источник 1 — принятые **внешние** команды рода игрока из списка `key_commands` в `goals.pd` с весом `[предл.]`: `Wed` (78) 5, `SetHeir` (70) 6, `ChangeSuccessionLaw` (71) 7, `Disown` (72) 6, `Tonsure` (69) 4, `PressClaim` (149) 8, `StartPlot` (152) 8, `ExecuteCoup` (156) 9, `Denounce` (148) 6, `SetFaithStance` (216) 7, `ProposeTreaty` (202) 4, `BreakTreaty` (205) 6, `TakeLoan` (38) ≥ 1000 ₽ 3, `PetitionForOffice` (131) 3, `ChangeGoal` (171) 5.
- Источник 2 — выбор варианта события игроком (`ChooseEventOption`, 96), если в эффекте варианта стоит `record_key_decision = { weight = N }` (эффект 2520, §14); автор события решает сам.
- Запись: снимок «до» в день решения, «после» — через `consequence_months` (12) системой; ёмкость 64: при полном журнале новая запись вытесняет запись с наименьшим `weight`, при равенстве — самую старую (`seq`); если вес новой меньше минимального — новая не пишется. Детерминированно, без ГСЧ.
- Без дублей с летописью: летопись E7 хранит факты; итоговая сводка показывает решение один раз (запись журнала побеждает запись летописи той же даты и того же объекта).

### 12.2 `game::build_final_summary(const World&, const GameRules&) → FinalSummary`

Состав (FR-GOAL-3, борды 24–27, 73, 74): статус и причина, даты (старт, конец, лет сыграно), род и главы по поколениям, итоговый счёт с `Breakdown` 22000, ряд снимков игрока и трёх сильнейших соперников, место среди `scored`, цели (основная/дополнительная, состояние, даты), до 30 записей летописи по важности (рождения наследников, смерти глав, браки, конец партии; без повторов), ключевые решения по дате с дельтами «до → после». Чистое чтение мира; строки — ключи локализации, мост `FinalScreenView` переводит.

## 13. Команды (теги 168…175)

### 13.1 Таблица

| Тег | Команда | Тело | Кто | Задача |
|---:|---|---|---|---|
| 168 | `FoundDynasty` | `surname` (u8 len + ≤ 48 Б), `house_genitive` (≤ 64 Б), `motto` (≤ 160 Б), `founder_name` (`u16` id имени), `father_name` (`u16`), `look` `u8`, `faith` `u8`, `coat` `u8`, `province` (`ProvinceId`), `family` `u8` | система, только тик 0 и без `GameSetup` | T09 |
| 169 | `SetPlayerDynasty` | `dynasty` | система, только тик 0 и без `GameSetup`; род из `playable` (в `--dev` — любой живой) | T02 |
| 170 | `SetGoal` | `slot` `u8`, `goal` `u8` | система (день 0) или игрок (первая дополнительная) | T11 |
| 171 | `ChangeGoal` | `slot` `u8`, `goal` `u8` | игрок | T11 |
| 172 | `EndGame` | `status` `u8`, `reason` `u8` | внутр. (`game.end`) | T13 |
| 173 | `RecordKeyDecision` | `tag` `u16`, `arg` `u32`, `event` `u32`, `option` `u8`, `weight` `u8` | внутр. (`game.key_decisions`) | T14 |
| 174 | `ResolveGoal` | `slot` `u8`, `state` `u8` | внутр. (`game.goals`) | T11 |
| 175 | — | резерв (M2: настройки партии, второй игрок) | | |

Строковые поля `FoundDynasty` — новый вид тела (длина + байты), только у тега 168; теги < 128 и тела прежних команд не меняются, `PRP1`/`PCL1` остаются версии 1 (save-format §6.1). Ограниченный текстовый тип для тела — сначала поиск готового в `src/core` (AGENTS.md §6), иначе один общий тип в `core`.

### 13.2 Порядок применения дня 0

`SetAiOptions` → `SetPlayerDynasty`/`FoundDynasty` → `SetGoal` (порядок подачи; `SetGoal` до `GameSetup` отклоняется `NoGame`).

### 13.3 `GameDenial` — коды отказа (`u16`, стабильны)

1 `NoGame`, 2 `AlreadyStarted`, 3 `NotPlayable`, 4 `BadText`, 5 `BadName`, 6 `ProvinceNotFoundable`, 7 `UnknownGoal`, 8 `GoalNotOffered`, 9 `SlotTaken`, 10 `SameGoal`, 11 `GoalLocked`, 12 `SandboxSecondary`, 13 `GameOver`, 14 `NotPlayer`, 15 `InternalOnly`, 16 `PlayerListMoved`; ключи `game.denial.<имя>`.

## 14. Каталог E11

| Код | Имя | Область | Аргумент | Производитель |
|---:|---|---|---|---|
| on-action 160 | `on_goal_resolved` | `dynasty` (игрок) | `slot + 2·state + 16·goal` | журнал тика: `ResolveGoal` |
| 161 | `on_goal_changed` | `dynasty` | `slot + 16·goal` | журнал: `SetGoal`/`ChangeGoal` |
| 162 | `on_game_end` | `realm` | `status + 16·reason` | журнал: `EndGame` |
| 163 | `on_house_founded` | `dynasty` | — | журнал: `FoundDynasty` |
| 164…169 | резерв | | | |

Триггеры/значения: 1430 `game_goal` (P, dynasty: цель в слоте), 1431 `goal_progress` (V, ‰), 1432 `goal_state` (V), 1433 `dynasty_score` (V), 1434 `dynasty_score_rank` (V), 1435 `game_status` (V, realm), 1436 `is_founded_house` (P, dynasty), 1437 `years_played` (V, realm); 1438…1449 резерв. Эффект: 2520 `record_key_decision` (`weight` 1…9; только в варианте события, выбранном игроком); 2521…2539 резерв. `is_player_controlled` (1120) остаётся у E11 и читает `game::is_player` после T03.

## 15. `Breakdown` — разложения (блок 22000…22999)

| Корень | Что | Узлы |
|---:|---|---|
| 22000 | счёт династии | 22001…22007 (§9) |
| 22100 | прогресс «Пережить» | 22101 время, 22102 фаза, 22103 стабильность, 22104 законная линия |
| 22200 | прогресс «Должность» | 22201 месяцы, 22202 держатель |
| 22300 | прогресс «Очки» | 22301 место, 22302 серия, 22303 отрыв от лидера |
| 22400 | прогресс «Престол» | 22401 месяцы правления |
| 22500…22999 | резерв (M2) | |

## 16. Сейв и хеш (E30-T03)

### 16.1 Разделы

| id | Раздел | Строка | Пишется |
|---:|---|---|---|
| 84 | `game_setup` | синглтон §6.1 | при `present = 1` |
| 85 | `goals` | `GoalRow` 24 Б × ≤ 2 | непустой |
| 86 | `key_decisions` | ≤ 64 × 64 Б | непустой |
| 87 | `score_history` | 10 Б × ≤ 936 | непустой |
| 88…91 | резерв E30 (M2: достижения, ironman-флаги, второй игрок) | | |

Все < 96 — `kMaxSections` не меняется. `kStateSchema` +1 (следующий свободный на момент слияния T03) с шагом миграции §6.5; фикстура прежней схемы и корпус порчи — процедура E13-T12.

### 16.2 Часть хеша

**`ExtPart` 64 `Game`** — одна объединённая часть: FNV-1a от подхешей разделов 84…87 (как `Economy`); 65 — резерв E30. Мир без `GameSetup` даёт 0 и не входит в `total` — хеши наблюдателя и всех старых сейвов прежние. Номер 64 требует `kExtPartCount = 128`: подъём (размер массива в памяти, без смены форматов и golden, save-format §5.3) делает **первая** из W-задач E30-T03 и E16-T03; по очереди W это E30-T03, тогда E16-T03 только занимает свои 50…56.

### 16.3 Сводка сейва

| key | Поле | Тип | Обяз. | Смысл |
|---:|---|---|---|---|
| 5 | `player_dynasty` | u32 | да | `GameSetup.player`; `0xFFFFFFFF` без `GameSetup` |
| 6 | `dynasty_key` | UTF-8 | нет | ключ рода; у своего рода — `custom` |
| 7 | `head_name` | UTF-8 | нет | у своего рода фамилия из `custom` |
| 11, 12 | `goal_key`, `goal_progress_ppm` | | нет | основная цель и её прогресс |
| **28** | `game_hash` | u64 | нет | `GameRules::hash()`; пишется при `present = 1`; сверяется при загрузке как `religion_hash` |
| **29** | `game_status` | u8 | нет | статус §6.1 (каталог показывает «Победа», «Род угас», «Песочница») |

Поля 5…12 заполняет `summary_from_world` из `query.hpp` (сейчас их заполняет вызывающий; E13-T08 [#373](https://github.com/savinovanton/patriarch/issues/373) ждёт T02 ради этого API).

## 17. Хеши `simcli_m1` и golden

| Задача | Закреплённые значения |
|---|---|
| T02, T03 | не меняются: мир без `GameSetup` хешируется прежне; `kExpectedSaveBytesHash` меняется только словом схемы — осознанно в T03 (W) |
| T04 | не меняются: перенос `m1` в формат сценария бит-в-бит |
| T05 | не меняются: построитель людей покрыт своими тестами, в `simcli` подключается в T07/T08 |
| **T06, T07, T08** | **меняют `simcli_m1`** (стартовые владения/должности, корона, роды) — G-задачи по одной, последовательно с пакетами E11 и E10-T20/E12-T22, с объяснением в коммите |
| T09…T17 | не меняются: системы E30 пусты без игрока, наблюдатель не создаёт `GameSetup` |
| T19 | единственная задача, добавляющая `tests/golden_start_test.cpp`: `simcli --player golitsyn --goal survive` и свой род, 2 года, сейв → загрузка → реплей |

Два PR, одновременно меняющих golden или версию формата, — стоп и согласование в issue.

## 18. Контракты с соседями

| Эпик | Что даёт/берёт E30 | Правка соседа |
|---|---|---|
| **E7** | `CreatePerson`/`Marry`/`SetDynastyHead`, пулы имён, вымирание; летопись — шаблоны `chronicle.game.*` | нет |
| **E8** | `SetHeir`, `ChangeSuccessionLaw` при установке; ключевые решения по журналу | нет |
| **E9** | прямой API стартового хозяйства (E9-T19 → `m1/holdings.pd`, `economy_start.pd`), казна и развитие в счёте | нет |
| **E10** | `AssignOffice`/`AssignParty` при установке, `listed` в `court.pd`; держания для цели «Должность» | нет |
| **E11** | `on_game_start` в первом тике; `SetEventSettings` без игроков; on-action 160…163, триггеры 1430…1437, эффект 2520; `is_player_controlled` → `game::is_player` | после T03 список `player_dynasties` пуст (event-engine §10.3, §24 — уже записано) |
| **E12** | `crisis.pd` (E12-T16) в каталоге `m1`; `Realm` для целей | пути `boyar_plot` → `m1` (этот PR) |
| **E13** | `assemble_session` (одна сборка, §10.3); сводка из `GameSetup`; начальный автосейв; E13-T08 ждёт T02 | ключи сводки 28, 29 (этот PR) |
| **E15** | `relations.pd`, `SignTreaty` при установке | нет |
| **E16** | `army.pd` прямым API до старта; `kExtPartCount` 128 поднимает первая из E30-T03/E16-T03 | save-format §5.1 (этот PR) |
| **E17** | `SetAiOptions` (196) дня 0 из `MenuStart.ai_difficulty`; агенты настраиваются в первом тике для всех родов `ai_agents.pd`, кроме `GameSetup.player` (#668 A); автопилот игрока — только AI-only/`--dev` (`ConfigureAgent{Autopilot}`, E17) | ai-model §20: `ai_agents.pd` перечисляет и играбельные роды (этот PR) |
| **E31** | `faiths.pd` прямым API при установке; вера своего рода; благосклонность в счёте | нет |
| **Меню UI-D12** | `MenuNewGame` из сценария (роды, карточки, цели, провинции своего рода), `MenuStart` + поля §10.1; строка «Сложность ИИ» и выбор провинции — в борд «Новая игра» | T15 |

## 19. Граница M1/M2

**M1:** один сценарий `m1` «Боярский заговор»; корона — Романовы; играбельные Голицыны, Шереметевы, Меншиковы и свой род; ИИ — Нарышкины и малые роды (#362); пять целей, основная + дополнительная; один игрок; три уровня сложности ИИ; выбор провинции своего рода — списком; конец по вымиранию, цели, 1780; итоговый экран; исторические якоря выключены по умолчанию (E11/E22).

**M2:** сценарии 2–4 и 4 × 20 родов (E21/E22); выбор провинции на карте и конструктор герба; ограничения роста после 1780 (ADR-0002); ironman (E28); достижения; туториал и подсказки (E23); свои цели игрока; резервы 175, 88…91, 65, 164…169, 1438…1449, 2521…2539.

## 20. Открытые вопросы

| # | Вопрос | Issue | До ответа |
|---|---|---|---|
| E30-Q4 | высшие должности для цели «Должность»; когда засчитываются «Очки»; что с просроченной целью | [#776](https://github.com/savinovanton/patriarch/issues/776) | вариант A в `goals.pd` `[предл.]` |
| E30-Q5 | какие провинции «свободны» для своего рода | [#777](https://github.com/savinovanton/patriarch/issues/777) | государевы с пометкой `foundable` |

Решены ранее: Q1 #362 (состав родов), Q2 #363 (свой род: выбор провинции, режим без семьи), Q3 #364 (рабочие значения целей), #668 A, #670 B. Борды: B1 #365.

## 21. Поправки задач и оценка

| Задача | Поправка | Дни |
|---|---|---|
| T02 | `query.hpp` для E11/E13/E17; инварианты GS-1…GS-8 | 3 |
| T03 | миграция `player_dynasties` (§6.5); подъём `kExtPartCount` до 128, если раньше E16-T03; ключи сводки 28, 29 | 2,5 → **3** |
| T04 | включаемые файлы E15/E16/E17/E31, `foundable_provinces`, `scored`, проверки SC01…SC09 | 5 → **5,5** |
| T08 | архетипы родов в `ai_agents.pd`, стартовые договоры, веры и армии родов — с ревью владельца | 6 → **7** |
| T09 | выбор провинции (E30-Q5), режим «с нуля» (#363), текстовые поля команды | 4 → **5** |
| T10 | команды дня 0 с `SetAiOptions`, первый тик, порядок установки E15/E16/E17/E31, `simcli --player` | 4 → **4,5** |
| T14 | вытеснение по весу, эффект 2520 | 3 → **3,5** |
| T15 | сложность ИИ, список провинций и режим семьи в мосте меню | 3 → **3,5** |
| T18 | баланс обоих режимов семьи своего рода | 4 → **4,5** |

Итого ≈ **75 дн. FTE** (было 70); критический путь T01 → T02 → T03 → T04 → T05 → T08 → T10 → T19 → T20 ≈ **36 дней**, календарно ≈ 48.
