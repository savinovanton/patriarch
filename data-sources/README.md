# data-sources — манифест исходных геоданных

Здесь лежит **только описание** внешних источников для карты реальной географии ([ADR-0004](../docs/adr/0004-geodata-pipeline.md)): [`geodata.manifest.json`](geodata.manifest.json). **Сырые сторонние данные в репозиторий не кладутся** — ни DEM, ни векторы, ни сканы карт. Они скачиваются скриптом `tools/geo/fetch.py` в локальный кэш вне репозитория и проверяются по SHA-256. Валидатор (`tools/geo/geo_manifest.py`) считает ошибкой любой посторонний файл в этом каталоге.

Как получить данные и проверить их — [docs/dev/geodata-howto.md](../docs/dev/geodata-howto.md). Вердикты по лицензиям — [docs/dev/geodata-sources.md](../docs/dev/geodata-sources.md).

## Формат (`manifest_version: 1`)

Верхний уровень: `manifest_version`, `description`, `sources` (список). Поля источника (все обязательны; неизвестное значение — явный `null`):

| Поле | Смысл |
|---|---|
| `id` | уникальный slug (`[a-z0-9-]`), он же подкаталог в кэше |
| `name`, `provider` | название набора и правообладатель/поставщик |
| `layer` | `terrain`, `coastline`, `hydrography`, `lakes`, `historical` |
| `status` | `unverified` (хеши/условия не подтверждены), `ready` (всё подтверждено), `blocked-on-owner` (ждёт решения владельца, см. `decision_issue`), `rejected` |
| `version`, `version_date` | версия набора и/или дата получения (`ГГГГ-ММ-ДД`) |
| `source_page` | страница набора |
| `files` | список `{path, url, sha256, size}`; `path` — относительный путь внутри `<кэш>/<id>/`, `sha256` — 64 hex-символа или `null`, `size` — байты или `null` |
| `license`, `license_url` | идентификатор из словаря `assets/README.md` либо собственный `LicenseRef-<имя>`; страница с текстом лицензии. `NC`/`ND` запрещены |
| `attribution` | `{required, text}` — нужна ли атрибуция и её текст (попадёт в экран «Об игре») |
| `checked_on` | дата, когда лицензия проверена |
| `distribution` | `may_commit_raw` (всегда `false`), `may_distribute_derivatives` (`yes`/`no`/`conditional`/`unknown`) |
| `transformations` | свободный список преобразований, применяемых к набору |
| `notes` | заметки и оговорки |
| `components`, `decision_issue` | необязательные: состав набора; номер issue с решением владельца |

Статус `ready` допустим только когда у каждого файла заданы `url`, `sha256`, `size`, а также лицензия с её страницей, дата проверки и явное решение по атрибуции. Хеши и URL **никогда не выдумываются**: их записывает владелец после первой загрузки (`fetch.py --allow-unverified` печатает вычисленные значения).
