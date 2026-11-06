# Бенчмарки (E2-T01)

Таргет `bench` использует Google Benchmark, уже зафиксированный в `vcpkg.json`.
Он собирается по умолчанию; отключение — `-DPATRIARCH_BUILD_BENCHMARKS=OFF`.
Для измерений используйте **Release/RelWithDebInfo без ASan** и закройте фоновые нагрузки.

```powershell
cmake --preset win-msvc
cmake --build --preset win-msvc-relwithdebinfo --target bench
ctest --preset win-msvc-relwithdebinfo -L bench --no-tests=error
.\out\win-msvc\bench\RelWithDebInfo\bench.exe --benchmark_out=out/bench-results.json
```

По умолчанию: **0.05 с прогрева**, минимум **0.1 с измерения**, **5 повторов** на
каждый набор параметров. Вывод в stdout — JSON с агрегатами; файл
`--benchmark_out=...` сохраняет отдельные повторы и агрегаты (включая медиану и CV).
Папка вывода должна существовать. Результаты храните в `out/`, не коммитьте локальные замеры.
Все стандартные CLI-флаги Google Benchmark поддерживаются и переопределяют эти значения:

```powershell
.\out\win-msvc\bench\RelWithDebInfo\bench.exe --benchmark_list_tests
.\out\win-msvc\bench\RelWithDebInfo\bench.exe --benchmark_filter=core.pcg32_next --benchmark_format=console --benchmark_repetitions=7 --benchmark_min_time=0.5s
```

## CPU affinity

Общий runner (`support.cpp`) до измерений закрепляет **главный поток** за первым
доступным логическим CPU. `--pat_cpu=N` выбирает CPU явно, `--pat_cpu=none` отключает
закрепление. Маска восстанавливается при выходе из runner. При ошибке закрепления
запуск завершается с ненулевым кодом. Выбранный CPU записывается в JSON: `context.cpu_affinity`.

Закрепление реализовано для Windows (CPU текущей process/group affinity mask) и Linux
(текущая allowed mask). На macOS автоматический режим продолжает без закрепления и
записывает `unsupported`; явный `--pat_cpu=N` завершается ошибкой. Новые рабочие потоки
нужно закреплять отдельно в соответствующих многопоточных бенчмарках; этот runner не
задаёт политику будущего пула задач E2-T03.

## Проверки и добавление бенчмарка

`ctest -L bench` запускает короткий **smoke**, а не гейт производительности: проверяет
реальное выполнение, прогрев/повторы, корректность JSON, медиану, throughput,
переопределение флагов и отклонение ошибочных параметров. JSON сохраняется рядом с
исполняемым файлом (`bench-smoke.json`); Windows CI публикует артефакт для Debug и
RelWithDebInfo. Для smoke нужен Python ≥ 3.8. Пороги регрессии и nightly — задача E2-T07;
шумные измерения CI не служат эталоном производительности.

1. Возьмите `rng_bench.cpp` как шаблон и добавьте файл в `bench/CMakeLists.txt`.
2. Подготовьте данные с фиксированным seed до `for (auto _ : state)`; сбрасывайте
   изменяемые данные между независимыми прогонами. Не включайте файловый ввод-вывод в замер.
3. Используйте `benchmark::DoNotOptimize` / `ClobberMemory` по смыслу нагрузки,
   `SetItemsProcessed` для throughput, понятное имя и явные размеры входа.

Справочник стандартных флагов и API: [Google Benchmark User Guide](https://google.github.io/benchmark/user_guide.html).

Windows-пресет `win-msvc-asan` отключает `bench`: готовый пакет Google Benchmark из
vcpkg собран без ASan и не совместим с аннотациями STL в MSVC ASan (LNK2038
`annotate_vector` / `annotate_string`). Остальные тесты проекта в этом пресете
сохраняются. Бенчмарки проверяются в обычных Debug/RelWithDebInfo/Release;
для совместной ASan-сборки Benchmark потребуется отдельный инструментированный
пакет. Причина конфликта описана в [документации MSVC](https://learn.microsoft.com/en-us/cpp/sanitizers/error-container-overflow?view=msvc-170).
