#include "pat/sim/events/catalog_doc.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <string_view>
#include <vector>

namespace pat::sim::events {

namespace {

std::string join(const std::vector<std::string>& items, std::string_view separator) {
    std::string out;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i != 0) {
            out += separator;
        }
        out += items[i];
    }
    return out;
}

std::string scopes_text(ScopeMask from) {
    if (from == 0) {
        return "любая";
    }
    std::vector<std::string> names;
    for (unsigned k = 1; k <= 10; ++k) {
        if ((from & (ScopeMask{1} << k)) != 0) {
            names.emplace_back(scope_kind_name(static_cast<ScopeKind>(k)));
        }
    }
    return join(names, ", ");
}

std::string arg_text(const ArgSpec& a) {
    std::string type;
    switch (a.kind) {
        case ArgKind::Number:
            type = "число";
            break;
        case ArgKind::Bool:
            type = "yes/no";
            break;
        case ArgKind::Scope:
            type = a.scope_kind == ScopeKind::None ? "область"
                                                   : std::format("область {}", scope_kind_name(a.scope_kind));
            break;
        case ArgKind::Name:
            type = a.ref_kind.empty() ? "имя" : std::format("имя ({})", a.ref_kind);
            break;
        case ArgKind::Ref:
            type = std::format("ссылка `{}:`", a.ref_kind);
            break;
        case ArgKind::Enum:
            type = join(a.enum_values, " \\| ");
            break;
        case ArgKind::Duration:
            type = "срок";
            break;
        case ArgKind::Date:
            type = "дата";
            break;
    }
    return std::format("`{}`: {}{}", a.name, type, a.required ? "" : " (необяз.)");
}

std::string args_text(const CatalogEntry& e) {
    std::vector<std::string> parts;
    if (!e.suffixes.empty()) {
        parts.push_back(std::format("вид: {}", join(e.suffixes, " \\| ")));
    }
    for (const ArgSpec& a : e.args) {
        parts.push_back(arg_text(a));
    }
    return parts.empty() ? std::string("—") : join(parts, "; ");
}

std::string keys_text(const CatalogEntry& e) {
    std::string out = std::format("`{}`", tooltip_key(e));
    if (e.space == NameSpace::Predicate) {
        out += std::format(", `{}`", tooltip_not_key(e));
    }
    if (e.space == NameSpace::Predicate || e.space == NameSpace::Value) {
        out += std::format(", `{}`", tooltip_fail_key(e));
    }
    return out;
}

struct Section {
    NameSpace space;
    std::string_view title;
};

}  // namespace

std::string render_catalog_reference(const Catalog& catalog) {
    std::string out;
    out += "<!-- GENERATED FILE. Do not edit by hand: run `schemadoc --catalog --write "
           "docs/dev/event-catalog-reference.md` (source of truth: the registrations in "
           "src/sim/events/catalog_*.cpp). -->\n\n";
    out += "# Справочник каталога движка событий\n\n";
    out += "> **Файл сгенерирован, не редактируйте его вручную:** `schemadoc --catalog --write "
           "docs/dev/event-catalog-reference.md`. `ctest` (`schemadoc_catalog_check`) сравнивает файл с "
           "выводом генератора. Спецификация и правила нумерации — [event-engine.md](event-engine.md) §8; "
           "язык правил — §6.\n\n";
    out += "Столбцы: **Код** — стабильный числовой код записи (не меняется и не переиспользуется); "
           "**Области** — где запись допустима без перехода; **Аргументы** — подписи аргументов в блоке "
           "`имя = { аргумент = … }` (главный аргумент можно писать как `имя = значение`); **Ключи "
           "тултипа** — шаблоны на русском в `data/loc/ru/events/` (`.not` — предикат с `= no`, `.fail` — "
           "причина недоступности варианта); **Пример** — запись в файле события. Запись с пометкой "
           "«заглушка» уже допустима в файлах событий, но система за ней ещё не создана: вычислитель "
           "отвечает «нет данных» (значение 0).\n\n";

    constexpr std::array<Section, 4> kSections{{
        {NameSpace::Predicate, "Предикаты (триггеры)"},
        {NameSpace::Value, "Значения"},
        {NameSpace::Effect, "Эффекты"},
        {NameSpace::StateOp, "Операции состояния"},
    }};
    for (const Section& section : kSections) {
        std::vector<const CatalogEntry*> rows;
        for (const CatalogEntry& e : catalog.entries()) {
            if (e.space == section.space && !e.retired) {
                rows.push_back(&e);
            }
        }
        if (rows.empty()) {
            continue;
        }
        std::ranges::sort(rows, {}, &CatalogEntry::code);
        out += std::format("## {} ({})\n\n", section.title, rows.size());
        out += "| Код | Имя | Области | Аргументы | Ключи тултипа | Пример | Примечание |\n";
        out += "|---:|---|---|---|---|---|---|\n";
        for (const CatalogEntry* e : rows) {
            out += std::format("| {} | `{}` | {} | {} | {} | `{}` | {} |\n",
                               e->code,
                               e->name,
                               scopes_text(e->from),
                               args_text(*e),
                               keys_text(*e),
                               e->example,
                               e->stub.empty() ? std::string("—")
                                               : std::format("заглушка до {}: «нет данных»", e->stub));
        }
        out += "\n";
    }
    return out;
}

}  // namespace pat::sim::events
