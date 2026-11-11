# Профилирование (Tracy)

> Проверено на: Windows 11, MSVC 19.44, vcpkg baseline из `vcpkg.json`, Tracy **0.14.1** (задача #32).

Tracy — опциональный профилировщик (ADR-0001, ADR-0002). По умолчанию он **выключен**: макросы `PAT_PROFILE_*` из `pat/core/profile.hpp` раскрываются в `((void)0)`, зависимость не скачивается, обычная сборка, хеши и golden-значения не меняются. Телеметрия бюджетов (`docs/dev/telemetry.md`) от Tracy не зависит.

## 1. Сборка с Tracy

Клиент Tracy — feature `tracy` в `vcpkg.json` (не входит в default-features). Для Windows есть готовый пресет:

```powershell
$env:VCPKG_ROOT = "C:\Users\<вы>\vcpkg"
cmake --preset win-msvc-tracy                              # каталог out/win-msvc-tracy
cmake --build --preset win-msvc-tracy-relwithdebinfo       # или win-msvc-tracy-debug
```

Пресет включает `PATRIARCH_ENABLE_TRACY=ON` и `VCPKG_MANIFEST_FEATURES=tracy`. Руками (любой пресет/платформа): `-DPATRIARCH_ENABLE_TRACY=ON -DVCPKG_MANIFEST_FEATURES=tracy`. Опция задаёт `PATRIARCH_ENABLE_TRACY` и `TRACY_ENABLE` для `patriarch_core` (PUBLIC, то есть для всех потребителей) и линкует `Tracy::TracyClient`. Для профилирования берите **RelWithDebInfo** (в Debug цифры искажены); бенчмарки в пресете выключены, а в замерах производительности (`docs/dev/performance-gate.md`) Tracy всегда выключен.

Запускаются те же `simcli.exe` и `patriarch_spike.exe` из `out/win-msvc-tracy/...`. Клиент слушает порт **8086** (TCP, UDP-broadcast для обнаружения) сразу при старте процесса.

## 2. Профайлер (GUI)

Версия интерфейса должна совпадать с версией клиента (протокол меняется между минорными релизами): **0.14.1**. Скачайте готовый Windows-архив со страницы релиза `https://github.com/wolfpld/tracy/releases/tag/v0.14.1` (ассет `windows-0.14.1.zip`, внутри `tracy-profiler.exe`) и распакуйте вне репозитория. Альтернатива — собрать через vcpkg (`tracy[gui-tools]`, тяжёлая цепочка зависимостей), в проект это не добавляется. При смене версии порта в vcpkg baseline обновите эту строку.

## 3. Захват

1. Запустите приложение из пресета `win-msvc-tracy` (например `simcli.exe --days 3650 --seed 7` или `patriarch_spike.exe`).
2. Откройте `tracy-profiler.exe`, выберите процесс в списке обнаруженных клиентов (или введите `127.0.0.1`) и нажмите Connect.
3. Остановите захват и сохраните `.tracy`, если нужно сравнить позже.

Без подключённого профайлера клиент копит события в памяти: для длинных прогонов `simcli` без GUI это неоправданно, запускайте с подключением или не включайте Tracy.

## 4. Зоны и кадры

| Имя | Где | Что |
|---|---|---|
| `sim.tick` | `src/sim/simulation.cpp` | весь тик симуляции |
| `sim.commands.internal` / `sim.commands.external` | `src/sim/simulation.cpp` | применение внутренних и внешних команд (через `PAT_SCOPED_TIMER`) |
| система по ID метрики (`economy.monthly` и т. п.) | `src/sim/simulation.cpp` | стадии тика, по литералу `PAT_SCOPED_TIMER` |
| `window.poll_events` | `src/engine/window.cpp` | обработка событий SDL |
| `ui.update`, `ui.render` | `src/engine/ui.cpp` | RmlUi: `Update` и `Render` |
| `render.present` | `src/engine/d3d11/d3d11_renderer.cpp` | `IDXGISwapChain::Present` (включает ожидание vsync) |

Кадры: основной набор Tracy — кадр рендера (`PAT_PROFILE_FRAME()` в конце итерации цикла спайка); тик симуляции — отдельный именованный набор **`sim.tick`** (`PAT_PROFILE_FRAME_NAMED("sim.tick")`), чтобы они не смешивались. В профайлере переключайте набор в выпадающем списке кадров.

## 5. Правила для нового кода

- Имя зоны — строковый литерал `подсистема.стадия`, строчными, через точку (как у метрик телеметрии).
- Зона на границе подсистемы или стадии, а не в горячем цикле по сущностям: зона стоит порядка десятков наносекунд, а в `sim` без Tracy — ноль.
- Для стадий симуляции используйте `PAT_SCOPED_TIMER(collector, metric, "имя")`: он даёт и телеметрию, и зону Tracy.
- Зоны не должны влиять на состояние и не вызываются «ради побочного эффекта» (детерминизм, `docs/dev/code-style.md` §4).

## 6. CI

Job `Linux gcc Tracy client` (`.github/workflows/ci.yml`) собирает ядро без движка с `PATRIARCH_ENABLE_TRACY=ON` и прогоняет все тесты, включая golden-хеши: конфигурация с Tracy остаётся собираемой и не меняет симуляцию. Зоны движка (D3D11) в CI не компилируются; проверять их локально пресетом `win-msvc-tracy`.
