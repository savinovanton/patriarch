# Работа двух агентов (Claude и Codex) через GitHub Issues и Projects

> Статус: v1 (2026-11-06). Источник правды о **задачах и статусах** — GitHub Issues (+ Project-доска). Источник правды о **требованиях и дизайне** — `docs/` (ТЗ, ADR, GDD, `docs/epics/*`).

## 1. Принцип

| Что | Где |
|---|---|
| Требования, дизайн, критерии приёмки | `docs/` (меняются PR-ами, с обоснованием) |
| Кто что делает, статус, обсуждение | GitHub Issues, метки, комментарии, Project |
| Вопросы к владельцу | Issues с `type:decision` + `needs:owner` |
| Код | ветки и Pull Request-ы, CI обязателен |

Оба агента работают через **один и тот же GitHub-аккаунт** (`gh` авторизован как владелец), поэтому «кто взял задачу» нельзя определить по автору или assignee. Роль агента задаётся **метками `agent:claude` / `agent:codex`** и **подписью в комментариях** (`[claude]`, `[codex]`), а в ветках — префиксом имени.

## 2. Метки

| Группа | Метки |
|---|---|
| Тип | `type:epic`, `type:feature`, `type:spike`, `type:bug`, `type:chore`, `type:docs`, `type:decision` |
| Область | `area:core`, `area:sim`, `area:engine`, `area:map`, `area:ui`, `area:tools`, `area:ci`, `area:perf`, `area:content`, `area:gameplay`, `area:docs` |
| Эпик | `epic:E0` … `epic:E34` (номера из `docs/epics`, `docs/02-epics-roadmap.md`) |
| Размер | `size:S`, `size:M`, `size:L`, `size:XL` |
| Статус | `status:ready`, `status:in-progress`, `status:blocked`, `status:review` |
| Кто | `agent:claude`, `agent:codex`, `needs:owner` |
| Приоритет | `prio:P0`, `prio:P1`, `prio:P2` |

Вехи (milestones): **M0** Фундамент и спайки, **M1** Вертикальный срез, **M2** Альфа, **M3** Бета, **M4** Релиз.

## 3. Протокол работы над задачей

1. **Найти задачу**
   ```powershell
   gh issue list --label status:ready --state open --json number,title,labels --limit 30
   ```
   Берите из ближайшей незавершённой вехи по порядку «сначала риск» (`docs/epics/README.md`). Не берите задачу, у которой уже есть метка `agent:*` другого агента или `needs:owner`.
2. **Взять в работу** (до первой строки кода)
   ```powershell
   gh issue edit <N> --add-label "agent:<claude|codex>,status:in-progress" --remove-label status:ready
   gh issue comment <N> --body "[<агент>] Беру в работу. Ветка: <агент>/<ID>-<slug>."
   ```
3. **Ветка:** `claude/E3-T01-sdl-window` или `codex/E3-T01-sdl-window`. Одна задача — одна ветка — один PR. Перед работой: `git fetch; git checkout -b <ветка> origin/main`.
4. **Работа:** по критериям приёмки из issue и `docs/epics/*`. Правила — `AGENTS.md`. Решение, которого нет в документах, — не принимать молча: открыть issue `type:decision` + `needs:owner` и (если возможно) переключиться на другую задачу.
5. **Проверки** (локально, как в `AGENTS.md` §4): сборка, тесты в нескольких конфигурациях, ASan, форматирование, `simcli --verify-determinism`.
6. **Pull Request:** `git pull --rebase origin main`, push, `gh pr create` с шаблоном; в описании `Closes #<N>`; метка задачи → `status:review`. Дождаться зелёного CI (`gh pr checks`, `gh run watch`).
7. **Слияние:** squash-merge после зелёного CI. **Владелец разрешил (2026-11-06) агентам самим сливать свои PR без отдельного подтверждения**, если все проверки CI зелёные, а PR не меняет golden-хеши/форматы файлов без обоснования в описании и не содержит нерешённых `needs:owner`. После слияния issue закрывается автоматически; снимите `agent:*`/`status:*`.
8. **Передача работы** (остановка до готовности): снять `status:in-progress` (вернуть `status:ready` или поставить `status:blocked`), оставить комментарий **handoff**: что сделано, что осталось, ветка/PR, известные проблемы, как проверить.

## 4. Как избежать конфликтов между агентами

- Перед началом: `gh pr list` и `gh issue list --label status:in-progress` — нет ли открытого PR в том же модуле/файлах.
- Не правьте чужую ветку; если нужно — комментарий в PR/issue.
- Общие «горячие» файлы (`CMakeLists.txt`, `vcpkg.json`, `tests/CMakeLists.txt`, `tools/CMakeLists.txt`, `.github/workflows/ci.yml`) держите изменения маленькими и коммитьте их быстро; перед PR всегда `rebase` на `origin/main`.
- `docs/dev/status.md` — обзорный документ; **не обновляйте его в каждой задаче** (конфликтует). Статусы задач — в issues; `status.md` правит агент, закрывающий веху, или владелец. Файл эпика (`docs/epics/E0x-*.md`) обновляйте в PR своей задачи (строка статуса).
- Golden-значения (`tests/golden_test.cpp`) и версии форматов меняет **только** PR, который осознанно меняет поведение; два таких PR одновременно — повод остановиться и согласовать в issue.

## 5. Project-доска

Поля: **Status** (Backlog · Ready · In progress · Review · Done), **Milestone**, **Epic**, **Size**, **Agent**, **Priority**. Представления: «Доска по статусам», «Таблица по вехам», «Ожидает владельца» (фильтр `needs:owner`).

Настройка (один раз, владелец): разрешить `gh` управлять проектами и создать доску.
```powershell
gh auth refresh -s project            # откроет браузер для подтверждения
gh project create --owner savinovanton --title "Patriarch"
# дальше агент добавляет issues: gh project item-add <номер> --owner savinovanton --url <url issue>
```
Пока доски нет, статус виден по меткам (`status:*`) и вехам — этого достаточно для работы.

## 6. Что агент делает в начале сессии

```powershell
git fetch; git status
gh issue list --label status:in-progress --state open        # что уже в работе (в т.ч. у другого агента)
gh issue list --label needs:owner --state open                # что ждёт владельца
gh pr list                                                    # открытые PR
gh run list --limit 5                                         # состояние CI
```
Красный CI на `main` — первая задача (`prio:P0`).

## 7. Рекомендуемые настройки репозитория (решает владелец, issue «Защита ветки main»)

- Защита ветки `main`: требовать PR и зелёные проверки (`MSVC debug`, `MSVC relwithdebinfo`, `MSVC AddressSanitizer`, `clang-format`, `asset license registry`); разрешить squash-merge; удалять ветки после слияния.
- До включения защиты: прямые коммиты в `main` допустимы только для документации и очевидных мелких исправлений, и только если нет открытых PR в тех же файлах.
