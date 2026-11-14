# Fixture E8, схема 12

`dynasty_v12_e8.psv`: контейнер PSV1 v2, схема мира 12, все таблицы E8, архивные ссылки, экономика, действующие переговоры/помолвка/регентство/спор и удел. Фикстура строится функцией `make_family_save_world()` в `tests/family_save_test.cpp`; её содержимое сравнивается с ожидаемым состоянием, хешем и повторно записанными байтами.

После сборки Debug, для намеренного создания или обновления:

```powershell
$env:PATRIARCH_WRITE_E8_FIXTURE = '1'
.\out\win-msvc\tests\Debug\patriarch_tests.exe '--test-case=family save: committed schema-12 fixture round-trips' --no-colors
Remove-Item Env:PATRIARCH_WRITE_E8_FIXTURE
```

Обычные тесты ничего не записывают. Исходный результат E8-T03: 1184 байта, `WorldHash::total = 11932784967711559582`. Прежняя фикстура схемы 11 сохранена отдельно и не перегенерируется новым кодом.
