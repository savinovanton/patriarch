# Аудит дублей кода (issue #541, шаги 1–2)

> Дата: **2026-11-15**. Проверенный коммит: `13fab5b` (`main`). Автор: агент Claude.
> Документ только описывает находки; код в этом PR не меняется. Консолидация — отдельными малыми PR (раздел 4), спорные случаи — issue `type:decision` + `needs:owner` (раздел 5).
> Правило, ради которого сделан аудит: `AGENTS.md` §6 «Не пишите одну и ту же реализацию дважды». Карта общих утилит — `docs/dev/code-style.md` §11 «Где что искать».

## 1. Метод

1. Просмотрены все модули: `src/core`, `src/sim` (вкл. `economy/`, `events/`), `src/mapdata`, `src/data`, `src/loc`, `src/content`, `src/game`, `src/ui`, `src/engine`, `tools/` (C++ и Python), `tests/`, `apps/spike`.
2. Поиск по сигнатурам и характерным конструкциям (`rg`): константы FNV (`0x100000001B3`, `1099511628211`, `0xCBF29CE484222325`), сдвиги `>> (8 * i)`, `0x80`/`0x7F` (varint), `0xC0`/`0xE0`/`0xF0` (UTF-8), `u8string`, `temp_directory_path`, `std::ofstream` + `rename`, `std::thread`, `from_chars`/`strtoul`/`atoi`, `remaining()`, определения `validate*`, `*_of(`, `field_pos`, `require_valid`, `rules()`, `economy_sim`, `TermKey`, `Breakdown`.
3. Каждая находка проверена чтением кода: в таблицу попали только **одинаковые или почти одинаковые реализации** одной и той же логики. Похожий стиль, общий шаблон кода с разной семантикой и разные контракты — в разделе 6.
4. Риск оценён по документам форматов: `docs/dev/save-format.md` (PSV/PRP/PCL, сводка, коды ошибок), ADR-0011, ADR-0007 (PMD2: `raw_hash`, `stored_hash`, `header_hash`, `content_id`, `coast_key`), `docs/dev/event-engine.md` (`Program::hash`, `RunResult::hash`, `EventSet::hash` → ключ 21 сводки), `docs/dev/economy-model.md` §10 (`economy::Breakdown`).

**Главный вывод по форматам.** Во всех местах, где считается FNV, используется один и тот же алгоритм — побайтовый FNV-1a 64 с теми же константами, что в `core::HashStream`. Все форматы (PSV, PRP, PCL, PMD1, PMD2, слои) уже пишут и читают через `core::ByteWriter` / `core::ByteReader`; varint тега команды (E13-T03) реализован один раз (`src/sim/command_log.cpp:222-246`). Поэтому **ни одна рекомендованная консолидация не меняет байты форматов, golden-значения и хеши `simcli`** — если выполнять её побитово, как описано. Изменения поведения возможны только в трёх местах (D21, D22 и строгий разбор чисел в `mapgen` из D17), и они вынесены отдельно.

Обозначения риска: **нет** — байты форматов, хеши и golden не меняются; **поведение** — меняется видимое поведение (текст, UI, CLI), но не форматы; **API** — меняется публичный интерфейс модуля. Размер: **S** — до ~150 строк и нескольких файлов, **M** — до дня работы, **L** — больше.

## 2. Сводная таблица

| ID | Что дублируется | Где (file:line) | Единое место (с учётом слоёв) | Риск | Размер | Порядок |
|---|---|---|---|---|---|---|
| D1 | FNV-1a 64 «одним вызовом» над байтовым буфером (`HashStream h; h.bytes(b); return h.value();`) и константы FNV | `src/sim/save.cpp:69`; `src/mapdata/map_format.cpp:35`; `src/mapdata/pmd2_container.cpp:33`; `src/mapdata/physical_layer.cpp:77`; `src/sim/events/program.cpp:225-227` и `src/sim/events/vm.cpp:1130-1132` (через `fnv1a64(string_view(reinterpret_cast…))`); `tools/stressgen/dataset.cpp:15`; `tests/golden_test.cpp:61`; `tests/pmd2_container_test.cpp:126`; `tests/save_container_test.cpp:40`; ручной FNV в `tests/renderer_test.cpp:783-789` (с **неверным** offset basis `1469598103934665603`: на цифру короче, это не FNV, но тесту безразлично); константы ещё раз в `src/core/include/pat/core/rng.hpp:34-40` | `core::hash_bytes(std::span<const std::uint8_t>)` и перегрузка для `std::span<const std::byte>` в `pat/core/hash_stream.hpp`; константы FNV — один раз (`fnv1a64` в `rng.hpp` берёт их оттуда же) | нет (тот же алгоритм, те же байты) | S | 1 |
| D2 | Строгая проверка UTF-8 (длина последовательности, overlong, суррогаты, > U+10FFFF) | `src/sim/save.cpp:95-138` (`utf8_char_length`, ещё и запрет NUL); `src/data/pd_lexer.cpp:49-89` (`find_invalid_utf8`) | `pat/core/utf8.hpp`: `utf8_sequence_length(std::string_view) → 0 при ошибке`; запрет NUL остаётся в `save.cpp` | нет (по чтению кода поведение эквивалентно, см. §3; тест на эквивалентность по всем ведущим байтам обязателен в PR) | S | 3 |
| D3 | Путь ↔ UTF-8 (`generic_u8string` / `char8_t`) | `src/data/content_source.cpp:69-76`; `src/engine/ui.cpp:25-32`; `tools/contentlint/cli.cpp:41-48` | `pat/core/path_utf8.hpp` (header-only, `<filesystem>`): `path_to_utf8`, `path_from_utf8` | нет | S | 2 |
| D4 | Атомарная запись файла (`.tmp` + `ofstream` + `flush` + `rename`, при ошибке удалить `.tmp`) | `src/sim/save.cpp:651-676`; `src/mapdata/map_format.cpp:264-288`; `src/mapdata/pmd2_container.cpp:464-491` | `pat/core/file_io.hpp`: `write_file_atomic(path, bytes) → Result<void, FileIoError{stage}>`; вызывающие переводят стадию (открыть / записать / переместить) в свои коды и тексты (save 30/31/32, PMD1 40/41/42, PMD2 `kIo`) | нет (коды и тексты сохраняются) | S | 4 |
| D5 | Чтение файла целиком | `src/sim/save.cpp:678-697` (байты, коды 33/34); `src/data/content_source.cpp:111-120` (текст); `src/ui/theme.cpp:125-135`; `tools/schemadoc/main.cpp:42-48` | `pat/core/file_io.hpp`: `read_file_bytes`, `read_file_text` (та же ошибка со стадией); `UiContext::read_text_file` (`src/engine/ui.cpp:344`) не трогать — см. раздел 6 | нет | S | 5 |
| D6 | Строка с префиксом длины `u16` в слоях карты и мелкие помощники ошибок | `put_str/get_str`: `src/mapdata/physical_layer.cpp:41-50` и `src/mapdata/province_layer.cpp:29-38` (побайтно одинаковы); `Error{code, message}`-обёртки: `map_format.cpp:31`, `map_validate.cpp:16`, `pmd2_container.cpp:29`, `physical_layer.cpp:31`, `province_layer.cpp:25`; проверка «обрыв / лишние байты»: `physical_layer.cpp:52` (`finish_blob`) и вручную `province_layer.cpp:227` | внутренний заголовок `src/mapdata/codec_util.hpp` (не публичный, `mapdata` зависит только от `core`) | нет (байты те же; тексты ошибок сохранить) | S | 6 |
| D7 | Защита от абсурдного счётчика (`count * bytes_each <= remaining()`) | функция `detail::plausible` — `src/sim/state_sections.hpp:48`; вручную: `src/sim/replay.cpp:228`, `src/sim/replay.cpp:245`, `src/sim/save.cpp:619`, `src/mapdata/province_layer.cpp:189`, `src/mapdata/province_layer.cpp:208` | метод `core::ByteReader::can_hold(count, bytes_each)` (умножение в `uint64_t`) в `pat/core/bytes.hpp`; `detail::plausible` — обёртка или удалить | нет (на 64-битных платформах результат тот же; в `province_layer` умножение в `size_t` заменяется на `uint64_t` — только надёжнее) | S | 7 |
| D8 | Запись little-endian в `std::vector<std::byte>` в ВМ событий, параллельно `core::ByteWriter` | `src/sim/events/program.cpp:87-97` (`put_u32`, `put_u64`); `src/sim/events/vm.cpp:1035-1049` (`put_u8`, `put_u32`, `put_u64`) | писать через `core::ByteWriter` и в конце копировать в `std::vector<std::byte>` (публичная сигнатура `to_bytes()` не меняется) | нет, **но** байты входят в `Program::hash`/`RunResult::hash`, а `EventSet::hash` пойдёт в ключ 21 сводки PSV — до замены закрепить значения хешей тестом | S | 8 |
| D9 | 64-битная арифметика со знаком: модуль/знак, проверка переполнения сложения и вычитания, насыщение | `src/core/include/pat/core/fixed.hpp:156-169`, `:182-208` (приватные `wrap_add`, `abs_u64`, `from_magnitude`); `src/sim/include/pat/sim/economy/money.hpp:43-55`, `:77-124` (`detail::magnitude`, `from_magnitude`, `checked_*`, `saturating_*`); `src/sim/events/vm.cpp:20-89` (`abs_u64`, `from_magnitude`, `add_sat`, `sub_sat`, `mul_sat`, `div_sat`, `neg_sat`) | `pat/core/detail/wide_math.hpp` (уже есть `U128`, `mul_u64`, `div_u128_by_u64`): `abs_u64(int64)`, `checked_add_i64`, `checked_sub_i64`, `from_magnitude_i64`; `money.hpp` и `vm.cpp` пользуются ими, `Fixed` — для `Raw = int64` | нет при побитовом совпадении (детерминизм: только целые, порядок операций прежний); обязательны табличные тесты на границах | M | 13 (последней из кода `sim`) |
| D10 | Типизированные аксессоры контента (`text_of`, `i32_of`, `enum_index_of`, `has_field`, `field_pos`) | `text_of`: `src/content/credits_content.cpp:25`, `src/content/names_content.cpp:41`; `i32_of`: `credits_content.cpp:35`, `names_content.cpp:31`, `src/content/demography_content.cpp:31`; `enum_index_of`: `credits_content.cpp:45`, `names_content.cpp:61`; `has_field`: `credits_content.cpp:55`, `names_content.cpp:85`; `field_pos`: `credits_content.cpp:60`, `names_content.cpp:76`, `src/content/m1_content.cpp:39`, `src/content/traits_content.cpp:34` (варианты: `key_pos` — `demography_content.cpp:52`, `position` — `src/content/m1_economy_content.cpp:52`) | внутренний `src/content/typed_access.hpp` (`pat::content::detail`, над `data::TypedBlock` с перегрузками для `ContentObject`) | нет | S | 9 |
| D11 | Таблица «код слагаемого → ключ локализации» и поиск по ней | `src/sim/economy/breakdown.cpp:9-12` + `:270` (`term_key`, `lower_bound`); `src/sim/marriage_score.cpp:152-155` + `:186` (`marriage_term_key`, линейный поиск); E10 (`docs/dev/court-model.md`) добавит третье семейство | `struct TermKey` и `find_term_key(std::span<const TermKey>, code)` в `pat/sim/economy/breakdown.hpp` | нет (обе таблицы отсортированы и уникальны, это проверяют тесты) | S | 10 |
| D12 | Тестовые файловые помощники | класс `TempDir` (одинаковый): `tests/content_loader_test.cpp:589`, `tests/economy_content_test.cpp:119`, `tests/demography_content_test.cpp:220`; `read_text/read_file`: `tests/ui_theme_test.cpp:26`, `tests/events_vm_test.cpp:200`, `tests/ui_data_test.cpp:24`, `tests/ui_components_test.cpp:35`, `tests/pd_parser_test.cpp:18`, `tests/economy_rules_test.cpp:47`, `tests/renderer_test.cpp:58`; запись: `tests/renderer_test.cpp:74`, `tests/ui_test.cpp:39`; `temp_path`: `tests/save_test.cpp:19`, `tests/save_container_test.cpp:36`, `tests/province_layer_test.cpp:280` | `tests/test_files.hpp` | нет (только тесты) | S | 11 |
| D13 | Тестовые чтение/патч little-endian по смещению | `tests/family_save_test.cpp:346-356`; `tests/economy_save_test.cpp:221-236` (и вручную `:1297`); `tests/save_container_test.cpp:46-62`; `tests/pmd2_container_test.cpp:132-158` | `tests/test_bytes.hpp`: `load_le<T>(bytes, at)`, `store_le<T>(bytes, at, v)` | нет (только тесты) | S | 12 |
| D14 | Тестовые помощники мира и экономики | `require_valid` (~22 копии): `tests/archive_test.cpp:53`, `character_schema_test.cpp:59`, `conception_test.cpp:87`, `economy_unrest_test.cpp:52`, `economy_modifiers_test.cpp:79`, `economy_policy_test.cpp:42`, `economy_administrator_test.cpp:68`, `economy_taxes_test.cpp:51`, `economy_loans_test.cpp:55`, `economy_construction_test.cpp:55`, `economy_settlement_test.cpp:55`, `economy_mood_test.cpp:70`, `economy_population_test.cpp:58`, `family_state_test.cpp:126`, `economy_save_test.cpp:51`, `economy_recruits_test.cpp:43`, `economy_world_test.cpp:75`, `economy_production_test.cpp:53`, `family_save_test.cpp:39`, `m1_economy_content_test.cpp:134`, `kinship_test.cpp:41`, `world_test.cpp:29`; `rules()` (12 копий, из них вариант «администрация заморожена» 4 раза: `economy_taxes_test.cpp:34`, `economy_settlement_test.cpp:38`, `economy_population_test.cpp:34`, `economy_production_test.cpp:32`); `economy_sim` (7): `economy_modifiers_test.cpp:150`, `economy_policy_test.cpp:72`, `economy_taxes_test.cpp:124`, `economy_construction_test.cpp:93`, `economy_population_test.cpp:118`, `economy_recruits_test.cpp:82`, `economy_production_test.cpp:96` | `tests/world_fixture.hpp` (`require_valid`, вариант с проверкой экономики) и `tests/economy_fixture.hpp` (`default_rules()`, `frozen_admin_rules()`, `economy_sim`); `make_world` у каждого теста свой (разные параметры) — не трогать | нет (только тесты) | M (можно двумя PR: мир, экономика) | 14 |
| D15 | Тестовая физическая карта 64×48 | `tests/physical_layer_test.cpp:72` (`make_layer`) = `tests/province_layer_test.cpp:68` (`make_physical`) | `tests/map_fixture.hpp` | нет | S | 15 |
| D16 | Потоки в `mapdata`: выбор числа потоков и параллельный цикл | `resolve_threads`: `src/mapdata/pmd2_container.cpp:57`, `src/mapdata/map_gen.cpp:21`; `parallel_for` (чередование) — `pmd2_container.cpp:67-89`, `parallel_rows` (полосы) — `map_gen.cpp:30-48` | внутренний `src/mapdata/parallel.hpp` с обоими способами разбиения (не в `core`: в `sim` потоки только через планировщик) | нет (запись в непересекающуюся память; байты PMD2 и карты те же — проверяют тесты) | S | 16 |
| D17 | Разбор целого из аргумента командной строки | строгий: `tools/simcli/main.cpp:110` (`parse_u64`), `tools/stressgen/main.cpp:103`, `tools/tick_spike/main.cpp:203`, `tools/trigger_spike/main.cpp:138`, `tools/snapshot_spike/main.cpp:381`; без проверки: `tools/mapgen/main.cpp:48-56` (`strtoul`), `apps/spike/main.cpp:190`, `:193`, `:204`, `:268`, `:335` (`atoi`) | `tools/common/cli_args.hpp` (header-only): `parse_u64(std::string_view) → std::optional`; применить в `simcli`, `stressgen`, `mapgen`; спайки и `apps/spike` не трогать | нет для `simcli`/`stressgen`; **поведение** для `mapgen` (мусор вида `12abc` станет ошибкой) | S | 17 |
| D18 | Штамп правил модификаторов — свой FNV по 64-битным словам | `src/sim/economy/modifiers.cpp:75-78` (`mix`), `:111` (`modifier_rules_stamp`) | `core::HashStream` (`i64` на каждое поле) | нет: значение только в памяти (`ModifierCache`, не сохраняется и не входит в хеш мира); тесты сравнивают только равенство (`tests/economy_modifiers_test.cpp:500`, `:631-635`) | S | 18 |
| D19 | `in_range(Fixed32, lo, hi)` | `src/sim/world.cpp:38`, `src/sim/world_economy.cpp:26` | мелочь: исправить при следующем касании файлов (отдельный PR не нужен) | нет | XS | — |
| D20 | UTF-8 → UTF-16 в движке | `src/engine/d3d11/d3d11_ui_renderer.cpp:52-57` повторяет `utf8_to_wide` из `src/engine/d3d11/d3d11_common.cpp:31-38` | `utf8_to_wide` (тот же модуль) | нет | XS | — |
| D21 | Модель «объяснения числа» (base / factor / clamp, Σ слагаемых = итогу) существует трижды | `economy::Breakdown` — `src/sim/include/pat/sim/economy/breakdown.hpp:59` (стандарт E8, E10, E11 по документам); `ProbabilityBreakdown` — `src/sim/include/pat/sim/demography.hpp:46` + `BreakdownBuilder` `src/sim/demography.cpp:19`; `CharacterBreakdown` — `src/sim/include/pat/sim/traits.hpp:93` + `clamp_total` `src/sim/traits.cpp:43` | решение владельца (раздел 5) | **API** + **поведение** (тултипы E7) | M–L | решение |
| D22 | Верхний регистр кириллицы | `src/engine/sdf_font.cpp:267` (`to_upper`: латиница, а–я, только ё из U+0450…045F); `src/ui/text.cpp:24` (`upper_text`: латиница, U+0430…044F и весь U+0450…045F) — **поведение расходится** | решение владельца (раздел 5) | **поведение** (подписи карты, заголовки HUD) | S | решение |

Итого: **20 подтверждённых дублей** (D1–D20, из них 2 мелочи без отдельного PR) и **2 параллельные реализации с расхождением**, которые требуют решения (D21, D22).

## 3. Подробности по рискованным местам

- **D1.** `fnv1a64(std::string_view)` в `rng.hpp` — `constexpr` для имён потоков ГСЧ, его оставить (тот же алгоритм); внутри он должен брать те же константы. `Program::hash`/`RunResult::hash` переходят на `core::hash_bytes(std::span<const std::byte>)` без `reinterpret_cast`. Проверка: `ctest`, `tools/check_golden_hash.py`, `simcli --verify-determinism`; golden не меняются.
- **D2.** Различия контрактов: `save.cpp` дополнительно отвергает NUL (сводка, ADR-0011), лексер NUL пропускает. Ведущие `0xC0/0xC1` и `0xF5…0xF7` в `save.cpp` проходят первую проверку, но отсекаются проверкой минимума и `> 0x10FFFF` — результат тот же, что у лексера. В PR — тест: для всех 256 ведущих байтов и граничных последовательностей обе старые функции и новая дают один ответ. Нестрогие декодеры (раздел 6) в этот PR не входят.
- **D8.** До замены добавить тест, который закрепляет `Program::hash()` и `RunResult::hash()` известной программы (если такого ещё нет в `tests/events_vm_test.cpp`); после замены значения обязаны совпасть.
- **D9.** Самая чувствительная консолидация: код в горячем пути ВМ и экономики. Делать последней, только с табличными тестами на `INT64_MIN`, `INT64_MAX`, `±1`, `0`, переполнение умножения и деления; проверять `simcli_m1` и golden.
- **D17 (`mapgen`).** Строгий разбор меняет поведение при неверном вводе (раньше `strtoul` молча давал 0 или префикс). Это исправление, а не регресс, но о нём нужно написать в PR.

## 4. Рекомендованные PR консолидации (по порядку, «сначала безопасное»)

Каждый PR — один дубль, без изменения форматов и golden; проверка — сборка, затронутые тесты, `check_golden_hash.py` для PR, затрагивающих `sim`/`mapdata`. Для каждого создан issue (`type:chore`, `area:core`, `status:ready`), номер указан в скобках.

1. **D1** (#543) — `core::hash_bytes`, константы FNV в одном месте (8 копий + 2 через `reinterpret_cast` + неверная копия в тесте). S.
2. **D3** (#544) — `core::path_to_utf8 / path_from_utf8` (3 копии). S.
3. **D2** (#545) — `core/utf8.hpp`: строгая длина UTF-8-последовательности (2 копии). S.
4. **D4** (#546) — `core::write_file_atomic` (3 копии). S.
5. **D5** (#547) — `core::read_file_bytes / read_file_text` (4 копии). S.
6. **D6** (#548) — `src/mapdata/codec_util.hpp` (строка `u16`, обёртка ошибки, `finish_blob`). S.
7. **D7** (#549) — `ByteReader::can_hold` (1 функция + 5 ручных проверок). S.
8. **D8** (#550) — ВМ событий пишет байты через `core::ByteWriter` (2 копии). S.
9. **D10** (#551) — `src/content/typed_access.hpp` (5 файлов). S.
10. **D11** (#552) — общая таблица `TermKey` / `find_term_key` (2 копии; до появления третьей в E10). S.
11. **D12** (#553) — `tests/test_files.hpp` (`TempDir`, `read_text`, `write_text`, `temp_path`). S.
12. **D13** (#554) — `tests/test_bytes.hpp` (LE-патч в 4 тестах). S.
13. **D9** (#555) — общая 64-битная арифметика с насыщением в `core/detail/wide_math.hpp` (3 копии). M.
14. **D14** (#556) — `tests/world_fixture.hpp` и `tests/economy_fixture.hpp`. M.
15. **D15** (#557) — `tests/map_fixture.hpp`. S.
16. **D16** (#558) — `src/mapdata/parallel.hpp`. S.
17. **D17** (#559) — `tools/common/cli_args.hpp`. S.
18. **D18** (#560) — `modifier_rules_stamp` через `HashStream`. S.

D19 и D20 — мелочи, правятся при следующем касании файлов (без отдельных issue).

Порядок выбран так, чтобы сначала шли изменения без влияния на `sim`-хеши (общие утилиты `core`, I/O, тесты), затем внутренности форматов с побитовой проверкой, и в конце арифметика ВМ. PR между собой почти не пересекаются по файлам, кроме пар D4/D5 (`save.cpp`) и D1/D6 (`physical_layer.cpp`, `pmd2_container.cpp`) — их делать последовательно.

## 5. Требуют решения владельца (`type:decision` + `needs:owner`)

Ни одна консолидация не меняет форматы или хеши, поэтому решений «по формату» нет. Решения нужны там, где объединение меняет поведение или API:

- **D21 (#561). Три модели объяснения числа.** Варианты: (A) оставить `ProbabilityBreakdown` и `CharacterBreakdown` как есть — это E7, они устоялись, плоские и с ключами-строками; (B) перевести обе на `economy::Breakdown` (коды `u16`, дерево ≤ 32 узлов, единый мост в UI `game::format_breakdown_value`), семейства кодов E7 выделить по образцу E8/E10/E11; (C) перевести только при следующей доработке E7 (вместе с тултипами персонажа). **Рекомендация: C** — правило §6 соблюдается для нового кода (E8+ уже на `economy::Breakdown`), а переделка E7 ради унификации меняет API и тултипы без пользы для игрока прямо сейчас.
- **D22 (#562). Верхний регистр кириллицы.** Варианты: (A) одна функция по широкому правилу `ui::upper_text` (весь U+0450…045F → U+0400…040F) в `core/utf8.hpp`, движок использует её; (B) одна функция по узкому правилу движка (только ё); (C) оставить две. **Рекомендация: A** — для русских текстов результат не меняется (ё → Ё в обоих), расхождение касается только ђ, ї, і и т. п., которых в данных нет.

## 6. Не дубли (разделены намеренно)

| Что | Почему не объединять |
|---|---|
| `core::ByteWriter/ByteReader` — единственный LE-кодек | PSV, PRP, PCL, PMD1, PMD2, физический и провинциальный слои, мир (`world_*_io.cpp`) уже пишут через него; параллельных классов чтения/записи нет (кроме ВМ, D8) |
| Varint тега команды | один раз: `src/sim/command_log.cpp:222-246` (E13-T03) |
| Каркасы форматов: сводка PSV (ключ/длина, `save.cpp:182-189`), таблица чанков PMD2, разделы мира (`world_io.cpp:160`) | разные форматы по ADR-0007 и ADR-0011; общий только кодек байтов |
| Типы ошибок валидаторов (`RulesError`, `MarriageRulesError`, `NamesError`, `EconomyRulesError`, `ProgramError`, `CommandError`, `data::LoadError`) | у каждой области свои коды и диапазоны; помощники диапазонов (`marriage_rules.cpp:89`, `economy/rules.cpp:353`) похожи, но тексты сообщений разные и проверяются тестами — выгода ~10 строк не стоит смены сообщений |
| Нестрогие декодеры UTF-8: `engine/sdf_font.cpp:239` (в UTF-32 с заменой на U+FFFD), `ui/character_window.cpp:50` (первая буква), `loc/message.cpp:47` (не резать последовательность), `loc/localizer.cpp:307-331` (позиции в исходнике), `data/pd_lexer.cpp:34`, `:112` (позиция, кодировщик) | разные контракты на битом вводе; перевод на `core/utf8.hpp` возможен после D2, но меняет поведение на ошибочных данных — только при касании |
| `UiContext::read_text_file` (`src/engine/ui.cpp:344`) | читает через `Rml::FileInterface` RmlUi (пути UTF-8 на Windows), а не напрямую |
| `wide_to_utf8 / utf8_to_wide` (`src/engine/d3d11/d3d11_common.cpp:21-38`) | Win32-API, единственная копия (кроме D20) |
| `ui::EconomyUnit` и `sim::economy::Unit` | `ui` не зависит от `sim`; мост — `src/game/economy_number.cpp:10` |
| `sim::to_string(Date)` и `game::format_date` | ISO-дата для логов/CLI и локализованная дата для UI |
| `game::season_of` (`map_overview.cpp:8`) и `winter_factor` (`season.cpp`) | разная семантика (дискретное время года и плавная доля зимы) |
| Сортировки (`std::sort`/`stable_sort`, ~50 мест) | компараторы по доменным ключам, общих помощников нет и не нужно |
| `sim::Runner` (`runner.cpp:28`), `PoolExecutor` (`tools/tick_spike/pool_executor.hpp:18`) | один поток симуляции и экспериментальный пул спайка E2; в `sim` параллелизм только через `TaskExecutor` |
| Спайки `tools/tick_spike`, `trigger_spike`, `snapshot_spike` | замороженные эксперименты (результаты в ADR-0002); их CLI и фикстуры не трогать |
| `make_world` в тестах экономики (12 вариантов) | параметры и состав мира различаются по задаче теста; общими делаются только помощники D14 |
| UI-фикстуры тестов (`character_window_test.cpp:54`, `credits_window_test.cpp:56`, `tooltip_manager_test.cpp:43`, `ui_test.cpp:80`) | похожая настройка (временный каталог, программный рендерер, `UiContext`), но разные файлы и окна; кандидат на общий `UiHarness` позже (есть `tests/ui_*_fixture.hpp`) |
| `tools/gen_credits.py:163` и `tools/check_assets.py:60` читают `assets/LICENSES.csv` | один проверяет реестр, другой берёт строки для титров; можно переиспользовать `load_registry`, но выгода минимальна |

## 7. После консолидации (2026-11-18)

Дубли D1–D20 сведены отдельными малыми PR (по одному дублю; D21 по решению владельца не трогается, D22 — вариант A). Что где лежит (полный список «где что искать» — `code-style.md` §11):

| ID | Единое место | PR |
|---|---|---|
| D1 | `core::hash_bytes`, константы FNV в `hash_stream.hpp` | #568 |
| D2 | `core/utf8.hpp`: `utf8_sequence_length` (save.cpp оставляет только запрет NUL) | #585 |
| D3 | `core/path_utf8.hpp` | #580 |
| D4 | `core/file_io.hpp`: `write_file_atomic` (стадия ошибки → коды вызывающего) | #587 |
| D5 | `core/file_io.hpp`: `read_file_bytes` / `read_file_text`; кроме перечисленных, найдены ещё копии (PMD1/PMD2, шейдеры D3D11, шрифт) — сведены | #603 |
| D6 | `src/mapdata/layer_codec.hpp` (`put_str/get_str` уже были), `make_error`, `finish_blob` | #591 |
| D7 | `ByteReader::can_hold`; `detail::plausible` удалён | #592 |
| D8 | ВМ событий пишет байты через `ByteWriter` (`core::to_std_bytes`); побайтовый закрепляющий тест `Program`/`RunResult` | #598 |
| D9 | `core/detail/wide_math.hpp`: `wrap_add/sub`, `add/sub_overflowed`, `abs_u64`, `from_magnitude<T>`; табличные тесты; замеры без регрессии | #624 |
| D10 | `src/content/typed_access.hpp` | #588 |
| D11 | `economy::TermKey` / `find_term_key` | #593 |
| D12 | `tests/test_files.hpp` | #595 |
| D13 | `tests/test_bytes.hpp` | #596 |
| D14 | `tests/world_fixture.hpp`, `tests/economy_fixture.hpp` | #604, #606 |
| D15 | `tests/map_fixture.hpp` (`make_test_terrain`; фикстуры различались озером, георефом и provenance — общий только цикл заполнения) | #605 |
| D16 | `src/mapdata/parallel.hpp` | #607 |
| D17 | `tools/common/cli_args.hpp` (мусор вида `12abc` в `mapgen` теперь ошибка) | #602 |
| D18 | `modifier_rules_stamp` через `HashStream` (значение штампа изменилось, оно только в памяти) | #597 |
| D19, D20 | `core::in_range` для `Fixed`; `utf8_to_wide` в UI-рендерере | #622 |
| D22 | `core::to_upper_cp` / `to_upper_utf8` (широкое правило, решение владельца A) | #620 |

Остаётся: D21 (три модели «объяснения числа») — по решению владельца C ничего не делается, E7-модели переводятся на `economy::Breakdown` при их следующей доработке. Нестрогие декодеры UTF-8 из раздела 6 остаются как есть (другие контракты на битом вводе). Форматы, golden-значения и хеши `simcli` ни одним PR не менялись.
