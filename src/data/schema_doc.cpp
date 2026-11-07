#include "pat/data/schema_doc.hpp"

#include <algorithm>
#include <cstdint>
#include <format>
#include <optional>
#include <string_view>
#include <variant>
#include <vector>

namespace pat::data {

namespace {

constexpr std::string_view kDash = "—";

/// Decimal text of a fixed-point raw value (`frac_bits` fractional bits), rounded to 6 decimals,
/// trailing zeros removed ("0", "1.5", "0.05"). Integer arithmetic only.
std::string format_q(std::int64_t raw, int frac_bits) {
    const bool negative = raw < 0;
    const std::uint64_t mag =
        negative ? 0ULL - static_cast<std::uint64_t>(raw) : static_cast<std::uint64_t>(raw);
    std::uint64_t whole = mag >> frac_bits;
    const std::uint64_t frac = mag & ((std::uint64_t{1} << frac_bits) - 1);
    constexpr std::uint64_t kScale = 1'000'000;
    std::uint64_t scaled = (frac * kScale + (std::uint64_t{1} << (frac_bits - 1))) >> frac_bits;
    if (scaled >= kScale) {
        ++whole;
        scaled -= kScale;
    }
    std::string out;
    if (negative && (whole != 0 || scaled != 0))
        out += '-';
    out += std::to_string(whole);
    if (scaled != 0) {
        std::string digits = std::format("{:06}", scaled);
        while (!digits.empty() && digits.back() == '0')
            digits.pop_back();
        out += '.';
        out += digits;
    }
    return out;
}

std::string format_fixed64(core::Fixed64 v) {
    return format_q(static_cast<std::int64_t>(v.raw()), 32);
}

std::string format_fixed32(core::Fixed32 v) {
    return format_q(static_cast<std::int64_t>(v.raw()), 16);
}

/// Markdown table cell text: pipes escaped, line breaks flattened.
std::string cell(std::string_view text) {
    std::string out;
    for (const char c : text) {
        if (c == '|')
            out += "\\|";
        else if (c == '\n' || c == '\r')
            out += ' ';
        else
            out += c;
    }
    return out;
}

std::string code(std::string_view text) {
    std::string out = "`";
    out += cell(text);
    out += '`';
    return out;
}

/// Type name for the documentation: "int", "enum", "ref(trait)", "list of string", "map of int".
std::string doc_type(const Type& type) {
    switch (type.kind) {
        case TypeKind::String:
            return "string";
        case TypeKind::Int:
            return "int";
        case TypeKind::Fixed32:
            return "fixed32";
        case TypeKind::Fixed64:
            return "fixed64";
        case TypeKind::Bool:
            return "bool";
        case TypeKind::Enum:
            return "enum";
        case TypeKind::Ref:
            return std::format("ref({})", type.ref_kind);
        case TypeKind::Date:
            return "date";
        case TypeKind::List:
            return "list of " + doc_type(*type.element);
        case TypeKind::Block:
            return "block";
        case TypeKind::Map:
            return "map of " + doc_type(*type.element);
    }
    return "?";
}

/// "lo..hi", "≥ lo" or "≤ hi"; empty when there is no limit.
std::string range_text(const std::optional<std::string>& lo, const std::optional<std::string>& hi) {
    if (lo && hi)
        return std::format("{}..{}", *lo, *hi);
    if (lo)
        return std::format("≥ {}", *lo);
    if (hi)
        return std::format("≤ {}", *hi);
    return {};
}

/// Constraints of the leaf type (elements of lists and maps are described by their own limits).
std::string leaf_constraints(const Type& type) {
    switch (type.kind) {
        case TypeKind::Int: {
            std::optional<std::string> lo;
            std::optional<std::string> hi;
            if (type.min_int)
                lo = std::to_string(*type.min_int);
            if (type.max_int)
                hi = std::to_string(*type.max_int);
            const std::string r = range_text(lo, hi);
            return r.empty() ? r : "диапазон " + r;
        }
        case TypeKind::Fixed32:
        case TypeKind::Fixed64: {
            std::optional<std::string> lo;
            std::optional<std::string> hi;
            if (type.min_fixed)
                lo = format_fixed64(*type.min_fixed);
            if (type.max_fixed)
                hi = format_fixed64(*type.max_fixed);
            const std::string r = range_text(lo, hi);
            return r.empty() ? r : "диапазон " + r;
        }
        case TypeKind::Enum: {
            std::string out = "значения: ";
            for (std::size_t i = 0; i < type.enum_values.size(); ++i) {
                if (i != 0)
                    out += ", ";
                out += '`' + type.enum_values[i] + '`';
            }
            return out;
        }
        case TypeKind::Ref:
            return "вид: `" + type.ref_kind + "`";
        case TypeKind::List:
        case TypeKind::Map:
            return leaf_constraints(*type.element);
        case TypeKind::String:
        case TypeKind::Bool:
        case TypeKind::Date:
        case TypeKind::Block:
            return {};
    }
    return {};
}

std::string constraints_text(const Type& type) {
    std::string leaf = leaf_constraints(type);
    if (leaf.empty())
        return std::string(kDash);
    if (type.kind == TypeKind::List)
        return "элементы: " + leaf;
    if (type.kind == TypeKind::Map)
        return "значения: " + leaf;
    return leaf;
}

/// Source-like text of a typed value (used for defaults).
std::string value_text(const TypedValue& v) {
    struct Visitor {
        std::string operator()(std::int64_t x) const { return std::to_string(x); }
        std::string operator()(const std::string& x) const { return '"' + x + '"'; }
        std::string operator()(core::Fixed32 x) const { return format_fixed32(x); }
        std::string operator()(core::Fixed64 x) const { return format_fixed64(x); }
        std::string operator()(bool x) const { return x ? "yes" : "no"; }
        std::string operator()(const EnumValue& x) const { return x.name; }
        std::string operator()(const RefValue& x) const { return x.kind + ":" + x.id; }
        std::string operator()(const DateValue& x) const {
            return std::format("{}.{:02}.{:02}", x.year, x.month, x.day);
        }
        std::string operator()(const TypedList& x) const {
            std::string out = "[";
            for (std::size_t i = 0; i < x.size(); ++i) {
                if (i != 0)
                    out += ", ";
                out += value_text(x[i]);
            }
            return out + "]";
        }
        std::string operator()(const TypedBlock&) const { return "{ … }"; }
        std::string operator()(const TypedMap&) const { return "{ … }"; }
    };
    return std::visit(Visitor{}, v.data);
}

/// The nested block schema behind a field type (directly or as list/map element), or nullptr.
const ObjectSchema* nested_block(const Type& type) {
    const Type* t = &type;
    while ((t->kind == TypeKind::List || t->kind == TypeKind::Map) && t->element)
        t = t->element.get();
    return t->kind == TypeKind::Block ? t->schema.get() : nullptr;
}

std::string heading(std::size_t level, std::string_view title) {
    return std::string(std::min<std::size_t>(level, 6), '#') + ' ' + std::string(title) + "\n\n";
}

/// `key` is the name used in a data file: the kind for a top-level object, the field name for a nested block.
void render_schema(std::string& out,
                   const ObjectSchema& schema,
                   const std::string& path,
                   const std::string& key,
                   bool top_level,
                   std::size_t level) {
    out += heading(level, (top_level ? "Вид " : "Блок ") + code(path));
    if (!schema.description().empty()) {
        out += schema.description();
        out += "\n\n";
    }
    if (top_level) {
        out += std::format("Объект: `{} \"<id>\" {{ … }}`. Метка (id) обязательна и уникальна в пределах "
                           "вида (`PD109`, `PD112`).\n\n",
                           path);
    } else {
        out += std::format(
            "Вложенный блок: `{} {{ … }}` или `{} = {{ … }}`; метка не допускается (`PD109`).\n\n", key, key);
    }

    if (schema.fields().empty()) {
        out += "Полей нет.\n\n";
        return;
    }

    out += "| Поле | Тип | Обязательность | По умолчанию | Ограничения | Описание |\n";
    out += "|---|---|---|---|---|---|\n";
    for (const FieldSpec& f : schema.fields()) {
        const char* presence =
            f.required ? "обязательное" : (f.repeatable ? "необязательное, повторяемое" : "необязательное");
        const std::string def = f.default_value ? code(value_text(*f.default_value)) : std::string(kDash);
        std::string type_cell = code(doc_type(f.type));
        if (nested_block(f.type) != nullptr)
            type_cell += " (см. `" + path + "." + f.name + "`)";
        out += std::format("| {} | {} | {} | {} | {} | {} |\n",
                           code(f.name),
                           type_cell,
                           presence,
                           def,
                           constraints_text(f.type),
                           f.description.empty() ? std::string(kDash) : cell(f.description));
    }
    out += '\n';

    for (const FieldSpec& f : schema.fields()) {
        if (const ObjectSchema* block = nested_block(f.type))
            render_schema(out, *block, path + "." + f.name, f.name, false, level + 1);
    }
}

}  // namespace

std::string render_reference_body(const SchemaSet& schemas) {
    std::vector<const ObjectSchema*> sorted;
    sorted.reserve(schemas.schemas().size());
    for (const ObjectSchema& s : schemas.schemas())
        sorted.push_back(&s);
    std::sort(sorted.begin(), sorted.end(), [](const ObjectSchema* a, const ObjectSchema* b) {
        return a->kind() < b->kind();  // std::string compares bytes (unsigned char), locale independent
    });

    std::string out;
    out += heading(2, "Виды объектов");
    if (sorted.empty()) {
        out += "Схемы не зарегистрированы.\n";
        return out;
    }
    for (const ObjectSchema* s : sorted) {
        out += "- " + code(s->kind());
        if (!s->description().empty())
            out += " — " + s->description();
        out += '\n';
    }
    out += '\n';
    for (const ObjectSchema* s : sorted)
        render_schema(out, *s, s->kind(), s->kind(), true, 2);
    while (out.size() >= 2 && out[out.size() - 1] == '\n' && out[out.size() - 2] == '\n')
        out.pop_back();
    return out;
}

std::string render_reference_document(const SchemaSet& schemas) {
    std::string out;
    out += "<!-- GENERATED FILE. Do not edit by hand: run `schemadoc --write docs/dev/data-reference.md` "
           "(tools/schemadoc, source of truth: the schemas in src/data/content_schemas.cpp). -->\n\n";
    out += "# Справочник схем данных `.pd`\n\n";
    out += "> **Файл сгенерирован, не редактируйте его вручную:** запустите `tools/schemadoc` "
           "(`schemadoc --write docs/dev/data-reference.md`). CI сравнивает файл с выводом генератора "
           "(`schemadoc --check docs/dev/data-reference.md`) и падает при расхождении.\n\n";
    out +=
        "Справочник описывает все виды объектов, зарегистрированные в `content_schemas()` "
        "(`src/data/content_schemas.hpp`): поля, типы, обязательность, значения по умолчанию, ограничения. "
        "Как писать файлы `.pd` — [data-authoring.md](data-authoring.md); синтаксис — "
        "[data-format.md](data-format.md); схемы и коды `PD101…PD110` — [data-schemas.md](data-schemas.md); "
        "загрузка корней и модов — [data-loader.md](data-loader.md).\n\n";
    out += "## Как читать таблицы\n\n";
    out += "- **Тип:** `string`, `int`, `fixed32` / `fixed64` (целое или десятичное число), `bool` (`yes` / "
           "`no`), "
           "`enum` (идентификатор из списка), `ref(вид)` (ссылка `вид:id`, голый идентификатор или строка), "
           "`date` (`Г.ММ.ДД`, юлианский календарь), `list of …` (`[ … ]`), `map of …` (блок `ключ = "
           "значение`), "
           "`block` (вложенный блок, описан в отдельном разделе).\n";
    out += "- **Обязательность:** обязательное поле должно встретиться ровно один раз; необязательное можно "
           "опустить; повторяемое — ключ можно писать несколько раз.\n";
    out += "- **По умолчанию:** значение, которое подставляется, если поле опущено; «—» — значения нет.\n";
    out += "- **Ограничения:** границы включительные; для списков и словарей относятся к элементам.\n\n";
    out += render_reference_body(schemas);
    return out;
}

}  // namespace pat::data
