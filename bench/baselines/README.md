# Эталоны производительности

`suite.json` задаёт обязательные имена, протокол capture и пороги. `reference.json`
хранит нормализованные выборки, профиль и исходную ревизию; начальный `source_sha` —
`0a330c797baf41dec08247bb6e72a8180479e593` (E2-T06).

Числа reference предназначены для ручного сравнения на совместимом профиле. GitHub CI
не сравнивает их с другим CPU: nightly/manual **заново собирает исходную ревизию** и
текущую версию на одном runner. PR сравнивает собственную базу с merge SHA.

Нормализованный формат schema 1 содержит полный `source_sha`, профиль и массив случаев
`{name, time_unit: "ns", samples, label?}`. Выборки — отдельные `real_time` повторы,
не готовые медианы Google Benchmark. Личные пути/hostname в эталон не включаются.

## Ручное обновление

1. Выберите уже слитую в `main` ревизию с объяснённым изменением нагрузки/производительности.
   Оцените отчёт относительно прежнего reference. Не обновляйте эталон просто ради зелёного CI.
2. Возьмите `candidate.json` успешного nightly/manual **с main** либо выполните свежий
   Release build/capture этой ревизии. Candidate синтетического PR merge для freeze не подходит.
3. Проверьте достижимость SHA и явно заморозьте файл:

```powershell
git fetch origin
$revision = (Get-Content out/performance-manual/reports/candidate.json -Raw | ConvertFrom-Json).source_sha
git merge-base --is-ancestor $revision origin/main
if ($LASTEXITCODE -ne 0) { throw 'Reference source must already be reachable from main' }
python tools/bench_compare.py freeze out/performance-manual/reports/candidate.json --suite bench/baselines/suite.json --output bench/baselines/reference.json
```

`freeze` проверяет формат, полный набор случаев, профиль, минимум выборок и RSD ≤5%,
затем атомарно заменяет reference. Git ancestry проверяется отдельно и повторно самим
paired runner, в том числе на PR. При ошибке freeze старый reference сохраняется.

Закоммитьте обновление отдельным понятным изменением/PR: причина, исходный SHA,
профиль, старые/новые медианы и ссылка на отчёт. Nightly и обычный compare ничего не
коммитят и не обновляют эталон автоматически. Политика suite также изменяется через ревью.

Набор сейчас: PCG32 на 1 024/16 384 извлечения и sim-batch из 32 тиков без/с телеметрией.
Capture: 7 × ≥0.5 с с прогревом 0.1 с. Порог — строго больше 10% по медиане, sample RSD
не выше 5%; шум означает inconclusive, а не доказанную регрессию или успешную проверку.
Повторов до удобного результата нет.
На Windows используется последний разрешённый CPU и AboveNormal только для дочернего
бенчмарка; эти условия входят в сравниваемый профиль.

Команды capture/compare, коды выхода `0/1/2/3`, ограничения и CI artifacts описаны в
[docs/dev/performance-gate.md](../../docs/dev/performance-gate.md).
