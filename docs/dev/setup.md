# Настройка среды разработки (Windows)

> Проверено на: Windows 11, CMake 4.4, MSVC 19.44 (Visual Studio Build Tools 2022 17.14), vcpkg 2026-10-29.

## 1. Что нужно

| Компонент | Версия / примечание |
|---|---|
| **MSVC (C++20)** | Visual Studio 2022 (или Build Tools 2022) с компонентом «Desktop development with C++» и Windows SDK 10.0.26100+ |
| **CMake** | ≥ 3.25 (проверено 4.4) |
| **Git** | любой актуальный |
| **vcpkg** | клон официального репозитория, переменная `VCPKG_ROOT` |
| **Python** | 3.x (скрипты проверок и бенчмарков, позже) |
| **clang-format / clang-tidy** | входят в компонент «C++ Clang tools for Windows» (LLVM из VS) |

> На текущей машине Visual Studio 2026 установлена **без** C++-нагрузки, поэтому используется **Build Tools 2022**. CMake находит его как генератор `Visual Studio 17 2022`. Чтобы перейти на VS 2026, добавьте нагрузку «Разработка классических приложений на C++» в Visual Studio Installer и смените генератор в `CMakePresets.json` на `Visual Studio 18 2026`.

## 2. vcpkg (один раз)

```powershell
git clone --depth 1 https://github.com/microsoft/vcpkg.git C:\Users\<вы>\vcpkg
C:\Users\<вы>\vcpkg\bootstrap-vcpkg.bat -disableMetrics
[Environment]::SetEnvironmentVariable('VCPKG_ROOT', 'C:\Users\<вы>\vcpkg', 'User')   # перезапустите терминал
```

Версии библиотек фиксирует `builtin-baseline` в `vcpkg.json`. Если вы клонируете vcpkg не поверхностно (`--depth 1`), baseline ищется в истории без проблем; при поверхностном клоне нужный коммит должен быть `HEAD` (или выполните `git fetch --unshallow`).

Используемый триплет: `x64-windows-static-md` (статические библиотеки, динамический CRT).

## 3. Сборка и тесты

Пресеты — в `CMakePresets.json`. Генератор Visual Studio мультиконфигурационный: **один** configure-пресет, конфигурация выбирается в build/test-пресете.

```powershell
cmake --preset win-msvc                         # конфигурация (скачивает и собирает зависимости)
cmake --build --preset win-msvc-debug           # или win-msvc-release / win-msvc-relwithdebinfo
ctest --preset win-msvc-debug                   # запуск тестов
```

AddressSanitizer:

```powershell
cmake --preset win-msvc-asan
cmake --build --preset win-msvc-asan-debug
ctest --preset win-msvc-asan-debug
```

Выходные каталоги: `out/win-msvc`, `out/win-msvc-asan` (в `.gitignore`).

## 4. Форматирование

```powershell
tools\format.ps1           # переформатировать исходники
tools\format.ps1 -Check    # проверка (для CI)
```

Справочник схем данных `docs/dev/data-reference.md` генерируется (`schemadoc --write docs/dev/data-reference.md`; exe — `out\win-msvc\tools\schemadoc\<Config>\schemadoc.exe`); актуальность проверяет `schemadoc --check docs/dev/data-reference.md` (в CI и в `ctest`).

Скрипт сам ищет `clang-format` (в `PATH` или в каталоге LLVM внутри Visual Studio).

## 5. Предупреждения и политика качества

- `/W4 /permissive- /utf-8`, предупреждения — ошибки (`PATRIARCH_WARNINGS_AS_ERRORS=ON` в пресетах).
- Код `sim` не использует `float/double` (ADR-0001, NFR-DET-1); это будет проверяться тестами и ревью в E1.
- Заголовки проекта подключаются как `#include "pat/<модуль>/<файл>.hpp"`, внешние — через `<...>`.

## 6. Известные особенности

- Сообщения MSVC локализованы (русский) — это нормально, коды ошибок (`C2027` и т.д.) остаются прежними.
- ASan-сборка копирует runtime-DLL (`clang_rt.asan_*`) рядом с тестами автоматически (см. `tests/CMakeLists.txt`).
- Для `doctest` включён `DOCTEST_CONFIG_USE_STD_HEADERS`, чтобы сравнивать `std::string_view` в `CHECK`.

## 7. Linux: проверка детерминизма (CI)

Платформа игры — Windows, но ядро симуляции обязано давать **те же хеши** на другом компиляторе и ОС (NFR-DET-1). Для этого в CI есть job `Linux gcc/clang …` (Ubuntu 24.04, GCC 13, Clang 18, Ninja). Он собирает `core`, `sim`, `mapdata`, `data`, тесты и tools **без движка** (`PATRIARCH_BUILD_ENGINE=OFF`, SDL3/RmlUi не нужны; в `vcpkg.json` они вынесены в feature `engine`, а на Linux включён `VCPKG_MANIFEST_NO_DEFAULT_FEATURES`), запускает `ctest` (включая `tests/golden_test.cpp` и `simcli_determinism`) и затем `tools/check_golden_hash.py`:

- выполняет `simcli --verify-determinism --days 800 --seed 7` и печатает `final hash` — по этой строке в логах сравниваются Windows и Linux (эталонное значение для seed 7 в репозитории не хранится);
- выполняет `simcli --seed 42 --days 1461` и сверяет **каждую часть** хеша с `kExpectedHash` из `tests/golden_test.cpp` (константы читаются оттуда же, дублей нет). Тот же шаг запускается и в MSVC-job.

Локально на Linux (нужны gcc/clang, Ninja, CMake ≥ 3.25, vcpkg и `VCPKG_ROOT`):

```bash
cmake --preset linux-gcc            # или linux-clang; -DCMAKE_BUILD_TYPE=Debug для отладочной сборки
cmake --build --preset linux-gcc
ctest --preset linux-gcc
python3 tools/check_golden_hash.py out/linux-gcc/tools/simcli
```

Если хеш на Linux отличается от Windows — это **баг детерминизма** (не обновляйте golden-значения): сравните части хеша (`calendar/rng/dynasties/provinces/characters`) и найдите подсистему.
