# Fixture схемы 13 перед E10-T03

`dynasty_v13_court.psv` создаётся исходным кодом **main `7a359af`** (E10-T02 слит, таблицы двора и влияния объявлены, но не сохраняются), до подъёма схемы до 14 в E10-T03. Исходник — `generate_dynasty_v13_court.cpp`; `static_assert` запрещает его сборку против другой схемы. После миграции fixture не перезаписывать.

Детерминированный мир: seed 1403, 2 июня 1703 года, режим династии, три рода, три провинции и три главы; строки событий схемы 13 (флаг персонажа, переменная рода, таймер, кулдаун). Экономика выключена, двора и влияния нет (схема 13 их не хранит). Контейнер — PSV1 v2, `World::kStateSchema = 13`.

Генератор проверяет инварианты, затем тем же исходным reader загружает сохранение и сравнивает полный `WorldHash` и таблицы событий. Только после этого пишет файл и печатает исходные хеши.

В Developer PowerShell for VS 2022, из корня checkout `7a359af`, после сборки `patriarch_sim` и `patriarch_core` в `win-msvc-debug`:

```powershell
cl.exe /nologo /std:c++20 /EHsc /MDd /Zi /D_DEBUG /I src/core/include /I src/sim/include tests/data/saves/generate_dynasty_v13_court.cpp /Fe:out/generate_dynasty_v13_court.exe /Fo:out/generate_dynasty_v13_court.obj /Fd:out/generate_dynasty_v13_court.compiler.pdb /link out/win-msvc/src/sim/Debug/patriarch_sim.lib out/win-msvc/src/core/Debug/patriarch_core.lib out/win-msvc/vcpkg_installed/x64-windows-static-md/debug/lib/zstd.lib
.\out\generate_dynasty_v13_court.exe tests/data/saves/dynasty_v13_court.psv
```

Исходный запуск `main 7a359af`, до изменений E10-T03:

```text
source=7a359af schema=13 container=2 bytes=448
total=9575088371203622812 calendar=15831398653128002313 rng=7582569637242788197 dynasties=17725493305770833748 provinces=3322450131423534774 characters=2113166119695896707
ext[0]=12478008331234465636
ext[1]=13017856919522880864
ext[5]=15292816675669018638
ext[24]=12839949525724368523
ext[25]=9883428011598517022
ext[29]=7700767836255011778
```

Остальные части `ext` равны нулю. После E10-T03 reader должен сохранить эти хеши, строки событий и дать пустые двор и влияние (части 32…40 — нули). Пересохранение выполняется уже в схему 14; стабильность байтов проверяется повторным round-trip текущей схемы (`tests/court_save_test.cpp`).
