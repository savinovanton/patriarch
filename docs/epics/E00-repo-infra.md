# E0 — Репозиторий и инфраструктура

> Веха: **M0**. Размер: **S–M (~13 рабочих дней)**. Статус: **выполнен (T01–T09); отложено: `clang-cl`/`clang-tidy` в CI, проверка интеграции Tracy**.
> Опирается на: ADR-0001 (стек, структура каталогов), ADR-0002 (бенчмарки, CI-гейт), TZ NFR-TEST-1, NFR-PERF-7, NFR-COMPAT-1.
> Оценки задач: **S** ≤ 1 дн., **M** 2–3 дн., **L** ~1 нед., **XL** > 1 нед. (разбивать).

## 1. Цель

Сделать так, чтобы любой следующий эпик начинался командой `cmake --preset ... && ctest`: одинаковая сборка локально и в CI, тесты, форматирование, санитайзеры и логирование — **до** появления кода игры.

## 2. Исходное состояние (по проверке машины)

| Что | Состояние |
|---|---|
| ОС | Windows 11 |
| Компилятор | Visual Studio Community 2026 (v18) установлена **без C++-нагрузки**; рабочий компилятор — **Build Tools 2022 (MSVC 19.44)** + Windows SDK 10.0.26100; генератор CMake `Visual Studio 17 2022` (vcvars и Developer Prompt не нужны) |
| CMake | 4.4.4 (`C:\Program Files\CMake`) |
| Ninja | Не в PATH (есть в Build Tools); в текущих пресетах не используется — генератор Visual Studio |
| vcpkg | Клонирован в `C:\Users\Anton\vcpkg` (shallow), `VCPKG_ROOT` пока задаётся в сессии |
| clang-format / clang-tidy | 19.1.5, из LLVM внутри Build Tools (скрипт `tools/format.ps1` находит сам); `clang-cl` — не проверялся |
| Git | Установлен. **Внимание:** папка проекта лежит внутри git-репозитория, корень которого — домашняя папка `C:\Users\Anton` (ветка `master`, коммитов нет). Для проекта нужен **собственный** `git init` в `patriarch/`; из домашнего репозитория ничего не коммитить и не пушить (там конфиги, токены и реестры пользователя) |
| GitHub | Репозиторий `savinovanton/patriarch` создан, **публичный**, пустой; `gh` авторизован (scopes: repo, workflow) |
| GitHub CLI | Установлен |
| Железо | i5-12400F (6/12), 32 ГБ ОЗУ, RTX 3050 |

Железо — выше «минимального» из ТЗ; бюджеты NFR-PERF-2 дополнительно проверяются на «слабом профиле» (E2-T08/E3-T10).

## 3. Объём

**Входит:** структура репозитория, CMake + presets, vcpkg manifest, форматирование/линтеры, фреймворк тестов, CI (GitHub Actions), базовые утилиты (лог, assert, профилирование-обёртки), документация разработчика, реестр ассетов.
**Не входит:** игровой код, ассеты, релизная упаковка (E28).

## 4. Пользовательские истории и критерии приёмки

**US-0.1.** *Как разработчик, я хочу собрать проект одной командой.*
- *Дано:* чистый клон репозитория на Windows с MSVC (VS/Build Tools 2022) и vcpkg. *Когда:* `cmake --preset win-msvc` и `cmake --build --preset win-msvc-debug`. *Тогда:* собираются `core`, `sim`, `tests` без ошибок и предупреждений уровня /W4.

**US-0.2.** *Как разработчик, я хочу, чтобы тесты запускались локально и в CI одинаково.*
- *Дано:* тест `core_smoke`. *Когда:* `ctest --preset win-msvc-debug`. *Тогда:* тесты проходят; CI-job воспроизводит результат на чистой машине.

**US-0.3.** *Как разработчик, я хочу находить ошибки памяти и UB до коммита.*
- *Дано:* пресет `win-msvc-asan`. *Когда:* сборка и запуск тестов. *Тогда:* ASan включён; тестовая ошибка (намеренно внесённая) обнаруживается и роняет тесты.

**US-0.4.** *Как разработчик, я хочу единый стиль кода.*
- *Дано:* `.clang-format`. *Когда:* `tools/format.ps1 --check`. *Тогда:* проходит на всём коде; в CI — проверка форматирования.

## 5. Структура репозитория (цель)

```
patriarch/
├─ CMakeLists.txt, CMakePresets.json, vcpkg.json
├─ .clang-format, .clang-tidy, .editorconfig, .gitignore, .gitattributes
├─ src/
│  ├─ core/ sim/ data/ rules/ ai/ engine/ ui/ game/
├─ tools/                # утилиты (CLI): simcli, stressgen, content-lint…
├─ tests/                # doctest
├─ bench/                # Google Benchmark
├─ content/              # данные игры (позже)
├─ assets/               # + LICENSES.csv — реестр ассетов
├─ docs/                 # ТЗ, ADR, GDD, эпики
└─ .github/workflows/
```

Начинаем с `core`, `sim`, `tools`, `tests`, `bench`; остальные каталоги создаются пустыми с README.

## 6. Технические задачи

| ID | Задача | Размер | Зависит от | Результат / приёмка |
|---|---|---|---|---|
| **E0-T01** | Git и публичный репозиторий: `git init -b main` **внутри `patriarch/`** (отдельный репозиторий, не домашний), `origin` = `https://github.com/savinovanton/patriarch`; `.gitignore` (build/, out/, .vs/, *.user, vcpkg_installed/, **`.claude/`**, секреты и локальные конфиги), `.gitattributes` (текст `eol=lf` для исходников, бинарные для ассетов), `LICENSE` (решение владельца — обязательно для публичного репозитория), первый коммит и push; проверка, что в историю не попало ничего вне `patriarch/` | S | — | `git rev-parse --show-toplevel` указывает на `patriarch/`; на GitHub видна только документация; `git status` чист |
| **E0-T02** | Каркас CMake: корневой `CMakeLists.txt`, C++20, `core`/`sim` как static libs, `tests`, `bench`, `tools`; `CMakePresets.json` (win-msvc-debug/release/relwithdebinfo, win-msvc-asan, win-clangcl-debug); вывод в `out/` | M | T01 | Сборка по пресетам; список таргетов задокументирован |
| **E0-T03** | vcpkg manifest: `vcpkg.json` с зафиксированным baseline; зависимости E0–E2: `doctest`, `benchmark`, `fmt`, `zstd`, `xxhash`; (E3 добавит `sdl3`, `rmlui`, `freetype`, `imgui`); подключение через toolchain-file; бинарный кэш | M | T02 | Чистая сборка тянет зависимости автоматически; версии воспроизводимы |
| **E0-T04** | Предупреждения и стиль: `/W4 /permissive-` (+ `-Wall -Wextra` для clang-cl), `.clang-format`, `.clang-tidy` (базовый набор: bugprone, performance, modernize выборочно), `.editorconfig`; скрипт `tools/format.ps1` | S | T02 | Пустой проект собирается без предупреждений; format-check проходит |
| **E0-T05** | Фреймворк тестов: `doctest`, `ctest`-интеграция, тест-утилиты (временные каталоги, генераторы данных), пример `core_smoke` | S | T03 | `ctest` запускает тесты; вывод читаемый |
| **E0-T06** | CI (GitHub Actions, раннеры `windows-latest`, репозиторий публичный — минуты бесплатны): job `windows-msvc` (build+test, Debug и RelWithDebInfo), job `windows-clangcl`, job `windows-asan`; кэш vcpkg; артефакты (лимит хранения — по политике GitHub, не «безлимит»; срок жизни настраивается); статус-бейдж в README | M | T01, T03, T05 | Pull request запускает три job; зелёный статус обязателен для слияния |
| **E0-T07** | Базовые утилиты `core`: логирование (уровни, категории, sink в файл+консоль), `ASSERT`/`VERIFY` (в debug — break, в release — лог+счётчик), обёртки профилирования (`PROFILE_SCOPE` → Tracy при включении), `Result<T, Error>`-тип | S | T02 | Юнит-тесты; макросы работают без Tracy |
| **E0-T08** | Документация разработчика: `docs/dev/setup.md` (установка VS/CMake/vcpkg, пресеты, запуск тестов), шаблон ADR (`docs/adr/_template.md`), соглашения по коду (`docs/dev/code-style.md`: именование, ID-типы, запрет float в `sim`, правила итерации) | S | T04 | Новый человек собирает проект по инструкции без вопросов |
| **E0-T09** | Скелет `assets/LICENSES.csv` (колонки: id, файл, источник URL, лицензия, автор, требует атрибуции, дата проверки), скрипт `tools/check_assets.py` (проверка, что каждый файл в `assets/` есть в реестре) | S | T01 | Скрипт падает, если ассет не зарегистрирован; интегрирован в CI |

**Суммарно:** ~13 дней (S: 1 + 1 + 1 + 1 + 0.5; M: 2.5 × 3 = 7.5 → ≈ 13).

### Статус выполнения

| Задача | Статус | Заметки |
|---|---|---|
| T01 | ✅ | Отдельный git, `main`, публичный `origin`, `LICENSE`, первый push |
| T02 | ✅ | `CMakeLists.txt`, `CMakePresets.json` (`win-msvc`, `win-msvc-asan`; build/test пресеты Debug/Release/RelWithDebInfo), `core`/`sim`/`tests`. Пресет `win-clangcl` отложен — `clang-cl` на машине не проверялся |
| T03 | ✅ | `vcpkg.json` (doctest, fmt, zstd, xxhash, benchmark) с `builtin-baseline`; триплет `x64-windows-static-md` |
| T04 | ✅ | `/W4 /permissive- /utf-8 /WX`; `.clang-format`, `.clang-tidy`, `.editorconfig`, `tools/format.ps1` (проверено: apply и `-Check`). `clang-tidy` не запускался — для генератора Visual Studio нет `compile_commands.json`; вернёмся к нему в CI |
| T05 | ✅ | doctest + CTest (`doctest_discover_tests`), 3 smoke-теста; Debug/Release/RelWithDebInfo и ASan проходят. ASan намеренно проверен на `heap-buffer-overflow` (ловит) |
| T06 | ✅ | `.github/workflows/ci.yml`: `clang-format` (Ubuntu, 19.1.5), MSVC Debug и RelWithDebInfo, MSVC ASan на `windows-2022`; общее действие `setup-vcpkg` (baseline из `vcpkg.json` + кэш). Ярлыки `windows-latest`/`windows-2025` теперь указывают на образ с VS 2026 — их не использовать, пока пресеты на `Visual Studio 17 2022`. `clang-cl` и `clang-tidy` в CI — позже |
| T07 | ✅ | `pat/core`: `log.hpp` (уровни, категории, консоль/файл/память, `ScopedLogCapture`, `PAT_LOG_*` на `std::format`), `assert.hpp` (`PAT_ASSERT` — только debug, `PAT_VERIFY` — всегда, счётчик и подменяемый обработчик), `profile.hpp` (`PAT_PROFILE_*`, пусто без Tracy; опция `PATRIARCH_ENABLE_TRACY` — интеграция Tracy не проверялась), `result.hpp` (`Result<T,E>`, `Result<void,E>`). 16 новых тестов; Debug/Release/RelWithDebInfo/ASan проходят |
| T08 | ✅ | `docs/dev/code-style.md` (именование, слои, правила детерминизма, ошибки/логи, тесты), `docs/adr/_template.md`, `docs/adr/README.md` |
| T09 | ✅ | `assets/LICENSES.csv` + `assets/README.md`, `tools/check_assets.py` (неучтённые файлы, запрет NC/ND, обязательные поля, даты), 13 unit-тестов (`tests/tools`), job `assets` в CI |

**Порядок:** T01 → T02 → (T03 ∥ T04) → T05 → (T06 ∥ T07) → T08, T09.

## 7. Тесты и проверки

- Smoke-тест сборки в CI на каждом PR.
- Проверка форматирования и clang-tidy на изменённых файлах (опционально: строгий режим после стабилизации).
- Проверка реестра ассетов (T09).
- Периодическая сборка «с нуля» в CI (nightly), чтобы ловить деградацию кэша.

## 8. Риски и вопросы

| Риск | Митигация |
|---|---|
| MSVC и clang-cl дают разное поведение (UB, предупреждения) | Два компилятора в CI; различия документируются |
| Время сборки растёт (шаблоны, vcpkg) | Бинарный кэш vcpkg, precompiled headers только в `core`, unity-build по желанию |
| Публичный репозиторий: всё, что закоммичено, видно всем и остаётся в истории | Не коммитить секреты, личные пути и конфиги; `.claude/` в `.gitignore`; проверка перед первым push; ассеты — только с подтверждённой лицензией (T09) |
| Репозиторий проекта вложен в домашний git-репозиторий | Отдельный `git init` в `patriarch/` (T01); домашний репозиторий не использовать |

**Открытые вопросы:**
1. ✅ **Лицензия — решено:** «все права защищены» (файл `LICENSE`, правообладатель `savinovanton`); просмотр и форки на GitHub разрешены правилами сервиса.
2. ✅ **Публикация — решено:** вся документация (ТЗ, ADR, GDD, эпики) публична.
3. Нужен ли Linux в CI с самого начала или только Windows?

**Статус E0-T01:** выполнено частично — отдельный git в `patriarch/` создан, ветка `main`, `origin` = публичный репозиторий, `LICENSE`, `.gitignore`, `.gitattributes` и первый коммит отправлены. Коммиты делаются под адресом GitHub noreply (личная почта в истории не публикуется).
