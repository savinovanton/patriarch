# Схемы и валидация данных `.pd`

> Статус: E4-T03 (готово). Код — `src/data` (`pat/data/schema.hpp`, пример — `pat/data/example_schemas.hpp`), тесты — `tests/schema_test.cpp`.
> Формат файлов и коды парсера `PD001…PD024` — в [data-format.md](data-format.md). Здесь — типизация дерева разбора и коды `PD101…PD110`.

Парсер (`parse_pd`) строит нетипизированное дерево. **Схема** описывает вид объекта (`trait "greedy" { … }`): поля, их типы, обязательность, значения по умолчанию, диапазоны. `validate_object` / `validate_file` проверяют дерево по схеме, возвращают **типизированные значения** и **все** найденные ошибки за один проход (каждая — с `файл:строка:столбец`, стабильным кодом и подсказкой). Разрешение ссылок между объектами, переопределение и загрузка нескольких корней — реестр и загрузчик (E4-T04, [data-loader.md](data-loader.md)); его коды — `PD111…PD114`.

Детерминизм (NFR-DET-1): никаких `float`/`double`; десятичные числа переводятся в `Fixed32`/`Fixed64` из точной пары «мантисса, порядок» целочисленной 128-битной арифметикой с усечением к нулю (результат совпадает с `Fixed::from_ratio`, но допускает мантиссы, не помещающиеся в `int32`). Порядок вывода зависит только от схемы и входных данных.

## 1. Описание схемы (C++)

```cpp
ObjectSchema chance("chance");
chance.required("base", Type::fixed64(Fixed64::zero(), Fixed64::one()))
      .optional("max", Type::fixed64(Fixed64::zero(), Fixed64::one()), TypedValue::of(Fixed64::one()));

ObjectSchema trait("trait");
trait.required("name_key", Type::string())
     .required("cost", Type::integer(0, 10))
     .optional("group", Type::enumeration({"vice", "virtue", "neutral"}), TypedValue::of(EnumValue{"neutral", 2}))
     .optional("opposite", Type::reference("trait"))
     .repeatable("note", Type::string())
     .optional("chance", Type::block(std::move(chance)));

SchemaSet schemas;
schemas.add(std::move(trait));
FileResult r = validate_file(schemas, parse_pd(text).file);   // r.objects, r.errors
```

Методы `ObjectSchema` (порядок объявления = порядок полей в результате):

| Метод | Смысл |
|---|---|
| `required(name, type)` | поле обязательно, ровно один раз |
| `optional(name, type)` | поле можно опустить (значения в результате нет) |
| `optional(name, type, default)` | при отсутствии в результат попадает `default` (`TypedField::defaulted = true`) |
| `repeatable(name, type)` | необязательное поле, ключ может повторяться (мультикарта); значения в порядке файла, `TypedBlock::find_all` |

`describe(текст)` после объявления поля и `describe_kind(текст)` для всего вида добавляют однострочные описания — они попадают в сгенерированный справочник [data-reference.md](data-reference.md) (E4-T09; на валидацию не влияют). Реальные схемы регистрируются в `content_schemas()` (`pat/data/content_schemas.hpp`); руководство автора — [data-authoring.md](data-authoring.md).

Ключ без `repeatable`, повторённый дважды, — ошибка `PD105`; значение берётся из первого вхождения.

### Типы (`Type::…`)

| Фабрика | Что принимает | Результат (`TypedValue::get_if<…>`) |
|---|---|---|
| `string()` | строка `"…"` / `"""…"""` | `std::string` |
| `integer(min?, max?)` | целое; включительные границы | `std::int64_t` |
| `fixed32(min?, max?)`, `fixed64(min?, max?)` | целое или десятичное; границы — `Fixed64` | `core::Fixed32` (Q16.16) / `core::Fixed64` (Q32.32) |
| `boolean()` | `yes` / `no` | `bool` |
| `enumeration({…})` | идентификатор из закрытого набора | `EnumValue{name, index}` |
| `reference(kind)` | `kind:id`, голый идентификатор или строка | `RefValue{kind, id}` (+ позиция `TypedValue::pos`); существование объекта проверяет загрузчик (`PD111`, E4-T04) |
| `date()` | `Г.ММ.ДД`, день должен существовать в **юлианском** календаре (високосный — каждый 4-й год, как `sim::Date`; `1704.02.29` верно, `1703.02.29` нет) | `DateValue{year, month, day}` |
| `list(type)` | `[ … ]`, каждый элемент проверяется | `TypedList` |
| `block(schema)` | вложенный блок `name = { … }` / `name { … }` по вложенной схеме (свои обязательные поля и умолчания) | `TypedBlock` |
| `map(type)` | блок `ключ = значение` с произвольными идентификаторами-ключами и значениями одного типа | `TypedMap` |

Дробные границы и значения: `0.1` в `Fixed32` — это `6553` (усечение к нулю), `32768.0` для `Fixed32` — ошибка диапазона (`-32768.0` допустимо).

Операторы в данных (`>=`, `!=` …) схемой не допускаются (`PD110`) — они зарезервированы за языком правил (E11). Метка (`field "x" { }`) допустима только у объекта верхнего уровня (`PD109` иначе).

## 2. Коды ошибок схем

Формат тот же, что у парсера (`format_diagnostic`): `файл:строка:столбец: error[КОД]: текст`. Тесты сверяют код и позицию. Список ошибок отсортирован по позиции.

| Код | Смысл | Позиция |
|---|---|---|
| `PD101` | неизвестное поле; подсказка «did you mean» по расстоянию редактирования (с перестановками, без учёта регистра) либо список допустимых полей | ключ записи |
| `PD102` | не задано обязательное поле | объект (ключ записи верхнего уровня) или `{` вложенного блока |
| `PD103` | неверный тип: `expected int, found string`; также неверный вид ссылки, ключ карты не идентификатор, объект не блок | значение |
| `PD104` | значение вне диапазона (границы схемы или пределы `Fixed32`/`Fixed64`) | значение |
| `PD105` | повтор ключа, не объявленного `repeatable` (в сообщении — место первого), повтор ключа карты | ключ повтора |
| `PD106` | неизвестное значение перечисления; сообщение содержит допустимые значения и подсказку | значение |
| `PD107` | несуществующая дата в юлианском календаре | значение |
| `PD108` | неизвестный вид объекта верхнего уровня (`validate_file`), с подсказкой | ключ записи |
| `PD109` | у объекта нет id (`trait { … }`) или метка там, где её быть не должно | ключ записи |
| `PD110` | вместо `=` использован другой оператор | ключ записи |

Пример:

```
data/traits/core.pd:14:5: error[PD101]: unknown field 'skils' (did you mean 'skills'?)
data/traits/core.pd:15:12: error[PD104]: value 11 is out of range (0..10)
data/traits/core.pd:16:13: error[PD106]: unknown value 'vise' (allowed: vice, virtue, neutral) (did you mean 'vice'?)
```

## 3. Результат

- `validate_object(schema, entry)` → `ObjectResult{object, errors}`; при ошибках `object` содержит только корректные поля.
- `validate_file(schemas, file)` → `FileResult{objects, errors}`: запись `format_version` пропускается, `objects` — только объекты без ошибок (в порядке файла).
- `TypedObject{kind, id, pos, body}`; `TypedBlock::find(name)`, `find_all(name)`, `TypedMap::find(key)`; каждое `TypedValue` хранит позицию (для будущих сообщений реестра о битых ссылках).

Пример схемы `trait` с комментариями — `make_example_trait_schema()` (`example_schemas.hpp`); реальные схемы M1 появятся в E4-T05 (регистрируются в `content_schemas()`).
