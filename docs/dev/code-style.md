# Соглашения по коду

> Статус: v0.1 (E0-T08). Правила `sim` вытекают из ADR-0001 и ADR-0002; остальное — для единообразия.
> Форматирование — автоматически: `.clang-format`, проверка `tools\format.ps1 -Check` (в CI).

## 1. Язык

- Стандарт **C++20**, расширения компилятора отключены (`CMAKE_CXX_EXTENSIONS OFF`).
- Идентификаторы, комментарии в коде и сообщения коммитов — **английский**. Документация (`docs/`) и тексты игры — **русский**.
- Исключения для управления потоком **не используются**. Ошибки, которые можно предвидеть, возвращаем через `pat::core::Result<T, E>`; ошибки программиста — `PAT_ASSERT` / `PAT_VERIFY`.
- RTTI и `dynamic_cast` — только в инструментах и тестах, не в `sim`.

## 2. Именование

| Что | Стиль | Пример |
|---|---|---|
| Типы (`class`, `struct`, `enum`, `concept`) | `PascalCase` | `LogSink`, `ProvinceId` |
| Функции и методы | `snake_case` | `days_for_ticks()` |
| Переменные, параметры | `snake_case` | `tick_count` |
| Поля `class` (приватные) | `snake_case_` | `sinks_`, `min_level_` |
| Поля `struct` (данные) | `snake_case` | `record.elapsed_ms` |
| Константы, `constexpr` | `kPascalCase` | `kMaxArmies` |
| Значения `enum class` | `PascalCase` | `LogLevel::Warn` |
| Макросы | `PAT_UPPER_SNAKE` | `PAT_ASSERT` |
| Пространства имён | `lower_snake` | `pat::core`, `pat::sim` |
| Файлы | `snake_case.hpp/.cpp` | `build_info.hpp` |

Публичные заголовки модуля лежат в `include/pat/<модуль>/`, подключаются как `#include "pat/core/log.hpp"`. Порядок `#include` (обеспечивает `clang-format`): заголовок самого файла, затем `pat/...`, затем сторонние (`<doctest/...>`, `<fmt/...>`), затем стандартные библиотеки.

## 3. Слои и зависимости

```
core  ←  sim  ←  ai, rules, data
core  ←  engine  ←  ui  ←  game
```

- `sim` и модули ниже **не зависят** от `engine`, `ui`, окон, рендера, ввода, файловой системы игры (кроме сериализатора).
- `core` не зависит ни от чего, кроме стандартной библиотеки (опционально Tracy).
- Нарушение зависимостей — ошибка сборки (проверяется структурой CMake: `sim` линкуется только с `core`).

## 4. Правила детерминизма (`sim`, `rules`, `ai`, `data` — всё, что влияет на состояние мира)

Обязательные (ADR-0001, NFR-DET-1). Нарушение ломает реплеи и сейвы.

1. **Нет `float` / `double`** в состоянии и в расчётах, влияющих на него. Только целые и `Fixed32` / `Fixed64`. Деньги — `int64` копейки.
2. **Нет `std::unordered_map` / `unordered_set` в итерациях**, влияющих на состояние (порядок не специфицирован). Допускается для поиска без итерации; для итерации — `std::vector`, плотные индексы, `SlotMap`, отсортированные структуры.
3. **Нет случайности вне `RngStreams`**: запрещены `rand()`, `std::random_device`, `std::mt19937` без явного seed. Каждая подсистема берёт свой поток.
4. **Нет реального времени** (`std::chrono::system_clock`, `steady_clock`) в логике. Время мира — `Date` / номер тика.
5. **Нет упорядочивания по адресам** (`std::set<T*>`, сортировка указателей) и по хешам строк, зависящим от платформы.
6. **Нет потоков в системах напрямую**. Параллелизм — только через планировщик задач (E1-T07, E2-T03) с детерминированным слиянием.
7. **Порядок обработки сущностей** — по возрастанию ID (или явному ключу). Удаление во время итерации — через отложенный список.
8. Нет глобального изменяемого состояния; зависимости передаются явно (`World&`, `TickContext&`).

## 5. Идентификаторы и ссылки

- Сущности адресуются типизированными ID (`StrongId<Tag>`, поколенческие хэндлы — E1-T03). Голые `int` как ID запрещены.
- Ссылки между сущностями — по ID, не по указателю; жизненный цикл проверяет поколение.
- Строки в горячих путях заменяются интернированными `StringId`.

## 6. Ошибки, проверки и логирование

- `PAT_ASSERT(cond, "msg {}", x)` — инвариант, проверяется в debug (не вычисляется в release).
- `PAT_VERIFY(cond, ...)` — условие вычисляется всегда; при сбое — лог, счётчик, обработчик.
- Ожидаемые ошибки (ввод пользователя, файлы, контент) — `Result<T, E>`; не `assert`.
- Логи: `PAT_LOG_INFO("category", "text {}", value)`. Категории — короткие строки в нижнем регистре: `sim`, `economy`, `ai`, `content`, `render`, `ui`, `io`. Уровни: Trace/Debug — отладка; Info — вехи; Warn — подозрительное, но не ошибка; Error — сбой операции; Fatal — после него работа невозможна.
- В горячих путях (тик) логи Info и выше — только по событию, не на каждую сущность.

## 7. Профилирование

`PAT_PROFILE_SCOPE("subsystem.stage")` на границах подсистем и стадий тика; `PAT_PROFILE_FRAME()` — конец кадра рендера, `PAT_PROFILE_FRAME_NAMED("sim.tick")` — конец тика. Имена — строковые литералы вида `economy.monthly`. Макросы пусты без Tracy; сборка и захват — `docs/dev/profiling.md`. Телеметрия для бюджетов (E2-T06) строится отдельным RAII-таймером.

## 8. Память и производительность

- Горячие данные — SoA в `std::vector`; без `shared_ptr` в графах сущностей.
- Аллокации на тик — через арены; в тике `new`/`malloc` в циклах запрещены.
- Любое «каждый тик по всем X» требует обоснования в ревью или перевода в событийную/периодическую модель (ADR-0002).

## 9. Тесты

- Каждый публичный API нового модуля покрывается `doctest`-тестами в `tests/`.
- Тест, зависящий от случайности, фиксирует seed.
- Тесты не должны оставлять файлов вне временного каталога и не зависят от рабочей директории.
- Для условий, вычисляемых как константа, в тестах использовать «непрозрачное» значение (иначе предупреждение C4127 при `/WX`).

## 10. Коммиты и ветки

- Ветка по умолчанию `main`; небольшие изменения допустимы прямыми коммитами после зелёного CI, крупные — через PR.
- Сообщение коммита: краткое (до ~72 символов) на английском; ID задачи в начале: `E1-T02: PCG32 with named streams`.

## 11. Где что искать (общие утилиты)

Прежде чем писать помощник, проверьте этот список (правило `AGENTS.md` §6: одна реализация на всё). Известные дубли и план их сведения — [code-duplication-audit.md](code-duplication-audit.md); после каждого PR консолидации обновляйте этот раздел.

| Нужно | Где | Что там |
|---|---|---|
| Запись/чтение little-endian в байтовый буфер | `pat/core/bytes.hpp` | `ByteWriter` (`u8…u64`, `i32`, `i64`, `boolean`, `bytes`), `ByteReader` (то же, без исключений: сбой → `ok() == false`, `remaining()`, `at_end()`); все форматы пишут только через них |
| Хеш байтов, состояния, контента | `pat/core/hash_stream.hpp` | `HashStream` — FNV-1a 64 по канонической LE-форме (`u8…u64`, `id`, `fixed`, `bytes`) |
| Хеш строки на этапе компиляции, потоки ГСЧ | `pat/core/rng.hpp` | `fnv1a64(string_view)`, `split_mix64_next`, `Pcg32`, `RngStreams` (`world.rng().stream("имя")`) |
| Фиксированная точка, насыщение | `pat/core/fixed.hpp`, `pat/core/detail/wide_math.hpp` | `Fixed32`/`Fixed64`, `saturating_add/sub/mul`, `mul_div`, `from_ratio`; `U128`, `mul_u64`, `div_u128_by_u64` |
| Деньги | `pat/sim/economy/money.hpp` | `Kopecks`, `checked_*`, `saturating_*`, `checked_mul_div`, `checked_mul_permille`, `split_permille`, `clamp_treasury` |
| Ошибки, проверки, логи, профилирование | `pat/core/result.hpp`, `assert.hpp`, `log.hpp`, `profile.hpp`, `telemetry.hpp` | `Result<T,E>`, `Error{code, message}`; `PAT_ASSERT`/`PAT_VERIFY`; `PAT_LOG_*`; `PAT_PROFILE_*` |
| ID и контейнеры | `pat/core/ids.hpp`, `slot_map.hpp`, `sparse_set.hpp`, `static_vec.hpp`, `arena.hpp`, `spsc_queue.hpp`, `string_interner.hpp` | `StrongId`, `SlotMap`, `SparseSet`, `StaticVec`, `Arena`, `SpscQueue`, `StringInterner` |
| Сжатие, PNG | `pat/core/compress.hpp`, `png_writer.hpp` | `compress`/`decompress` (zstd), `write_png_rgb` |
| Дата и календарь | `pat/sim/date.hpp`, `calendar.hpp`; UI — `pat/game/world_view.hpp` | `Date`, `YearMonthDay`, `age_years`, `to_string(Date)` (ISO); `game::format_date` (локализованная) |
| Объяснение числа (тултип) | `pat/sim/economy/breakdown.hpp`; вывод — `pat/game/tooltip_view.hpp` | `economy::Breakdown` (дерево ≤ 32 узлов, коды `u16`), `term_key(code)`; ВМ — `events::BreakdownRecorder`; `game::format_breakdown_value`, `format_money` |
| Защита от абсурдных счётчиков при чтении | `src/sim/state_sections.hpp` (внутр.) | `detail::plausible(in, count, bytes_each)` |
| Тег команды (varint) | `src/sim/command_log.cpp` | каноничный LEB128 тега, одна реализация |
| Атомарная запись и чтение файла сохранения | `pat/sim/save.hpp` | `write_file_atomic`, `read_file` (пока не в `core`, см. аудит D4/D5) |
| Карта: контейнер и слои | `pat/mapdata/pmd2_container.hpp`, `physical_layer.hpp`, `province_layer.hpp`, `map_format.hpp` | `Pmd2Writer`/`Pmd2Reader`, `encode_*`/`decode_*`, `validate_*` |
| Данные `.pd`, схемы, хеш контента | `pat/data/pd_parser.hpp`, `schema.hpp`, `content_registry.hpp`, `content_source.hpp` | парсер с позициями, `validate_object/file`, `ContentRegistry::content_hash()`, `MemorySource`/`DirectorySource` |
| Сообщения об ошибках загрузки контента | `pat/content/load_errors.hpp` | `format_errors` |
| Локализация и числа в тексте | `pat/loc/localizer.hpp`, `message.hpp`, `plural.hpp`; UI — `pat/ui/text.hpp` | `Localizer::format`, `format_integer`; `ui::format_number`, `format_delta`, `upper_text` |
| UTF-8 ↔ UTF-32 / UTF-16 (движок) | `pat/engine/sdf_font.hpp`, `src/engine/d3d11/d3d11_common.hpp` | `utf8_to_utf32`, `to_upper`; `utf8_to_wide`, `wide_to_utf8` |
| Планировщик задач `sim` | `pat/sim/task_executor.hpp` | `TaskExecutor`, `SerialExecutor` (потоки в системах — только через него) |
| Тестовые фикстуры | `tests/economy_test_access.hpp`, `test_fonts.hpp`, `ui_layout_fixture.hpp`, `ui_loc_fixture.hpp`, `ui_theme_fixture.hpp` | доступ к внутренностям экономики, шрифты, раскладки, локализатор и тема для UI-тестов |
| Проверка golden-хешей и ассетов | `tools/check_golden_hash.py`, `tools/check_assets.py`, `tools/gen_tables.py` | CI-проверки (см. `AGENTS.md` §4) |
