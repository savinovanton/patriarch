# Fixture схемы 11 перед E8-T03

`dynasty_v11_e8_economy.psv` создаётся исходным кодом **main `a1c3e69`**, до изменения схемы, раскладки ведомости или экономического хеша в E8-T03. Исходник — `generate_dynasty_v11_e8_economy.cpp`; `static_assert` запрещает его сборку против другой схемы или расширенной ведомости. После миграции fixture не перезаписывать.

Детерминированный мир: seed 298, режим династии, три рода, три провинции и три главы; отдельный счёт короны; начальные казны; заём первого рода у короны 50 000 копеек; разные январские доходы, расходы и квота; закрыт январь; 1 февраля 1703 года добавлены открытые статьи и погашено 5 000 копеек долга. Игровые системы не запускаются. Данные E8 пусты. Контейнер — PSV1 v2, `World::kStateSchema = 11`, старая плотная ведомость из 31 статьи.

Генератор проверяет структурные инварианты, затем тем же исходным reader загружает сохранение и сравнивает полный `WorldHash` и все таблицы экономики. Только после этого пишет файл и печатает исходные хеши. Это проверка самой создаваемой фикстуры, не диагностический игровой прогон.

В Developer PowerShell for VS 2022, из корня checkout `a1c3e69`, после обычной сборки `win-msvc-debug`:

```powershell
cl.exe /nologo /std:c++20 /EHsc /MDd /Zi /D_DEBUG /I src/core/include /I src/sim/include tests/data/saves/generate_dynasty_v11_e8_economy.cpp /Fe:out/generate_dynasty_v11_e8_economy.exe /Fo:out/generate_dynasty_v11_e8_economy.obj /Fd:out/generate_dynasty_v11_e8_economy.compiler.pdb /link out/win-msvc/src/sim/Debug/patriarch_sim.lib out/win-msvc/src/core/Debug/patriarch_core.lib out/win-msvc/vcpkg_installed/x64-windows-static-md/debug/lib/zstd.lib
.\out\generate_dynasty_v11_e8_economy.exe tests/data/saves/dynasty_v11_e8_economy.psv
```

Если каталог сборки/триплет отличается, заменить только пути библиотек. Генератор не использует локальные настройки и не записывает пути в файл.

Исходный запуск `main a1c3e69`, до изменений E8-T03:

```text
schema=11 container=2 ledger_lines=31 bytes=726
total=15479572076255755579
calendar=14369850264643241161
rng=4172692673313245204
dynasties=12330653458028426818
provinces=17062011115320500544
characters=12579569978138345613
ext[0]=12478008331234465636
ext[1]=6662615029219399614
ext[5]=2318443219762437030
ext[16]=12620759142815849368
borrower_treasury=152124 crown_treasury=1958210
closed_months=1 loans=1 loan_id=0
```

Остальные части `ext` равны нулю. После E8-T03 reader должен сохранить эти хеши и финансовое состояние. Пересохранение выполняется уже в новую схему; стабильность байтов проверяется на повторном round-trip текущей схемы, а не сравнением новых байтов со старым контейнером.
