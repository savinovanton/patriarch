#include "pat/data/credits_schemas.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace pat::data {

namespace {

constexpr std::int64_t kMaxOrder = 9999;

ObjectSchema credit_schema() {
    ObjectSchema s("credit");
    s.describe_kind(
        "Запись экрана «Титры» (UI-213, `docs/dev/credits.md`): автор, источник данных или ассет, который "
        "нужно назвать в игре. Записи из манифеста геоданных и реестра ассетов генерирует "
        "`tools/gen_credits.py` в `data/credits/generated.pd`; остальные пишутся вручную в "
        "`data/credits/credits.pd`. Заголовок задаётся ровно одним из полей `name_key` / `name`.");
    std::vector<std::string> sections;
    for (const std::string_view section : kCreditSections) {
        sections.emplace_back(section);
    }
    s.required("section", Type::enumeration(std::move(sections)))
        .describe("Раздел экрана: `authors` — авторы, `data` — источники данных, `assets` — ассеты и шрифты.")
        .optional("order", Type::integer(0, kMaxOrder), TypedValue::of(std::int64_t{100}))
        .describe("Порядок внутри раздела (по возрастанию; при равенстве — по id).")
        .optional("name_key", Type::string())
        .describe("Ключ строки локализации с заголовком записи.")
        .optional("name", Type::string())
        .describe("Заголовок записи как есть (названия наборов данных, не переводятся).")
        .optional("text_key", Type::string())
        .describe("Ключ строки локализации с пояснением записи.")
        .optional("text", Type::string())
        .describe("Текст атрибуции как есть (юридический текст источника, не переводится).")
        .optional("provider", Type::string())
        .describe("Поставщик или автор, как он назван в манифесте или реестре.")
        .optional("license", Type::string())
        .describe("Идентификатор лицензии (SPDX или `LicenseRef-…`).")
        .optional("source", Type::string())
        .describe("Id источника в `data-sources/geodata.manifest.json`, который закрывает эта запись. "
                  "Проверка `tools/gen_credits.py --check` требует запись у каждого источника с "
                  "`attribution.required`.")
        .optional("asset", Type::string())
        .describe("Путь файла в `assets/LICENSES.csv`, который закрывает эта запись.");
    return s;
}

}  // namespace

void add_credits_schemas(SchemaSet& set) {
    set.add(credit_schema());
}

}  // namespace pat::data
