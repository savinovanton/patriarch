#include "pat/data/schema.hpp"

#include "pat/core/assert.hpp"
#include "pat/core/detail/wide_math.hpp"

#include <algorithm>
#include <format>
#include <limits>
#include <utility>

namespace pat::data {

// ---- types -----------------------------------------------------------------------------------

Type Type::string() {
    return Type{};
}

Type Type::integer(std::optional<std::int64_t> min, std::optional<std::int64_t> max) {
    Type t;
    t.kind = TypeKind::Int;
    t.min_int = min;
    t.max_int = max;
    return t;
}

Type Type::fixed32(std::optional<core::Fixed64> min, std::optional<core::Fixed64> max) {
    Type t;
    t.kind = TypeKind::Fixed32;
    t.min_fixed = min;
    t.max_fixed = max;
    return t;
}

Type Type::fixed64(std::optional<core::Fixed64> min, std::optional<core::Fixed64> max) {
    Type t;
    t.kind = TypeKind::Fixed64;
    t.min_fixed = min;
    t.max_fixed = max;
    return t;
}

Type Type::boolean() {
    Type t;
    t.kind = TypeKind::Bool;
    return t;
}

Type Type::enumeration(std::vector<std::string> values) {
    Type t;
    t.kind = TypeKind::Enum;
    t.enum_values = std::move(values);
    return t;
}

Type Type::reference(std::string kind) {
    Type t;
    t.kind = TypeKind::Ref;
    t.ref_kind = std::move(kind);
    return t;
}

Type Type::date() {
    Type t;
    t.kind = TypeKind::Date;
    return t;
}

Type Type::list(Type element) {
    Type t;
    t.kind = TypeKind::List;
    t.element = std::make_shared<const Type>(std::move(element));
    return t;
}

Type Type::block(ObjectSchema schema) {
    Type t;
    t.kind = TypeKind::Block;
    t.schema = std::make_shared<const ObjectSchema>(std::move(schema));
    return t;
}

Type Type::map(Type value) {
    Type t;
    t.kind = TypeKind::Map;
    t.element = std::make_shared<const Type>(std::move(value));
    return t;
}

std::string describe_type(const Type& type) {
    switch (type.kind) {
        case TypeKind::String:
            return "string";
        case TypeKind::Int:
            return "int";
        case TypeKind::Fixed32:
        case TypeKind::Fixed64:
            return "number";
        case TypeKind::Bool:
            return "bool (yes/no)";
        case TypeKind::Enum:
            return "identifier (enum)";
        case TypeKind::Ref:
            return std::format("reference to '{}'", type.ref_kind);
        case TypeKind::Date:
            return "date";
        case TypeKind::List:
            return "list of " + describe_type(*type.element);
        case TypeKind::Block:
            return "block";
        case TypeKind::Map:
            return "map of " + describe_type(*type.element);
    }
    return "?";
}

// ---- typed values ----------------------------------------------------------------------------

const TypedValue* TypedBlock::find(std::string_view name) const {
    for (const TypedField& f : fields) {
        if (f.name == name)
            return &f.value;
    }
    return nullptr;
}

std::vector<const TypedValue*> TypedBlock::find_all(std::string_view name) const {
    std::vector<const TypedValue*> out;
    for (const TypedField& f : fields) {
        if (f.name == name)
            out.push_back(&f.value);
    }
    return out;
}

const TypedValue* TypedMap::find(std::string_view key) const {
    for (const TypedField& f : entries) {
        if (f.name == key)
            return &f.value;
    }
    return nullptr;
}

// ---- schemas ---------------------------------------------------------------------------------

ObjectSchema& ObjectSchema::required(std::string name, Type type) {
    fields_.push_back(FieldSpec{std::move(name), std::move(type), true, false, std::nullopt, std::string{}});
    return *this;
}

ObjectSchema& ObjectSchema::optional(std::string name, Type type) {
    fields_.push_back(FieldSpec{std::move(name), std::move(type), false, false, std::nullopt, std::string{}});
    return *this;
}

ObjectSchema& ObjectSchema::optional(std::string name, Type type, TypedValue default_value) {
    fields_.push_back(
        FieldSpec{std::move(name), std::move(type), false, false, std::move(default_value), std::string{}});
    return *this;
}

ObjectSchema& ObjectSchema::repeatable(std::string name, Type type) {
    fields_.push_back(FieldSpec{std::move(name), std::move(type), false, true, std::nullopt, std::string{}});
    return *this;
}

ObjectSchema& ObjectSchema::describe(std::string text) {
    PAT_ASSERT(!fields_.empty(), "describe() must follow a field declaration");
    if (!fields_.empty())
        fields_.back().description = std::move(text);
    return *this;
}

ObjectSchema& ObjectSchema::describe_kind(std::string text) {
    description_ = std::move(text);
    return *this;
}

int ObjectSchema::find_field(std::string_view name) const noexcept {
    for (std::size_t i = 0; i < fields_.size(); ++i) {
        if (fields_[i].name == name)
            return static_cast<int>(i);
    }
    return -1;
}

SchemaSet& SchemaSet::add(ObjectSchema schema) {
    for (ObjectSchema& existing : schemas_) {
        if (existing.kind() == schema.kind()) {
            existing = std::move(schema);
            return *this;
        }
    }
    schemas_.push_back(std::move(schema));
    return *this;
}

const ObjectSchema* SchemaSet::find(std::string_view kind) const noexcept {
    for (const ObjectSchema& s : schemas_) {
        if (s.kind() == kind)
            return &s;
    }
    return nullptr;
}

// ---- helpers ---------------------------------------------------------------------------------

bool is_valid_julian_date(std::int32_t year, std::int32_t month, std::int32_t day) noexcept {
    if (month < 1 || month > 12 || day < 1)
        return false;
    constexpr std::int32_t kLengths[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const std::int32_t length = (month == 2 && year % 4 == 0) ? 29 : kLengths[month - 1];
    return day <= length;
}

namespace {

template <class F>
bool decimal_to_fixed_impl(std::int64_t mantissa, unsigned scale, F& out) noexcept {
    if (scale > 18)
        return false;
    std::uint64_t divisor = 1;
    for (unsigned i = 0; i < scale; ++i)
        divisor *= 10;

    const bool negative = mantissa < 0;
    const std::uint64_t magnitude =
        negative ? (0ULL - static_cast<std::uint64_t>(mantissa)) : static_cast<std::uint64_t>(mantissa);
    const core::detail::U128 product = core::detail::mul_u64(magnitude, std::uint64_t{1} << F::frac_bits);
    bool overflow = false;
    const std::uint64_t q = core::detail::div_u128_by_u64(product, divisor, overflow);
    if (overflow)
        return false;

    using Raw = typename F::raw_type;
    const std::uint64_t limit = std::uint64_t{1} << (sizeof(Raw) * 8 - 1);  // 2^31 or 2^63
    if (negative ? q > limit : q >= limit)
        return false;
    out = F::from_raw(negative ? static_cast<Raw>(0ULL - q) : static_cast<Raw>(q));
    return true;
}

core::Fixed64 widen(core::Fixed32 v) noexcept {
    return core::Fixed64::from_raw(static_cast<std::int64_t>(v.raw()) * 65536);
}

core::Fixed64 widen(core::Fixed64 v) noexcept {
    return v;
}

/// Optimal string alignment distance (insert, delete, substitute, swap adjacent).
std::size_t edit_distance(std::string_view a, std::string_view b) {
    std::vector<std::vector<std::size_t>> d(a.size() + 1, std::vector<std::size_t>(b.size() + 1, 0));
    for (std::size_t i = 0; i <= a.size(); ++i)
        d[i][0] = i;
    for (std::size_t j = 0; j <= b.size(); ++j)
        d[0][j] = j;
    for (std::size_t i = 1; i <= a.size(); ++i) {
        for (std::size_t j = 1; j <= b.size(); ++j) {
            const std::size_t cost = a[i - 1] == b[j - 1] ? 0 : 1;
            d[i][j] = std::min({d[i - 1][j] + 1, d[i][j - 1] + 1, d[i - 1][j - 1] + cost});
            if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1])
                d[i][j] = std::min(d[i][j], d[i - 2][j - 2] + 1);
        }
    }
    return d[a.size()][b.size()];
}

std::string lower_ascii(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

std::string join_quoted(const std::vector<std::string>& names, std::size_t max_items) {
    std::string out;
    for (std::size_t i = 0; i < names.size() && i < max_items; ++i) {
        if (i > 0)
            out += ", ";
        out += names[i];
    }
    if (names.size() > max_items)
        out += ", ...";
    return out;
}

std::string found_name(const Value& v) {
    switch (v.kind) {
        case ValueKind::Int:
            return "int";
        case ValueKind::Decimal:
            return "decimal";
        case ValueKind::Bool:
            return "bool";
        case ValueKind::Date:
            return "date";
        case ValueKind::String:
            return "string";
        case ValueKind::Ident:
            return "identifier";
        case ValueKind::Ref:
            return "reference";
        case ValueKind::Block:
            return "block";
        case ValueKind::List:
            return "list";
    }
    return "?";
}

std::string value_text(const Value& v) {
    return v.kind == ValueKind::Int ? std::to_string(v.integer) : v.text;
}

std::string range_text(const std::optional<std::string>& lo, const std::optional<std::string>& hi) {
    if (lo && hi)
        return std::format("{}..{}", *lo, *hi);
    if (lo)
        return "at least " + *lo;
    return "at most " + *hi;
}

class Validator {
public:
    explicit Validator(std::vector<Diagnostic>& errors) : errors_(errors) {}

    void report(SourcePos pos, ErrorCode code, std::string message) {
        errors_.push_back(Diagnostic{pos, code, std::move(message)});
    }

    TypedBlock check_block(const ObjectSchema& schema,
                           const std::vector<Entry>& entries,
                           SourcePos where,
                           const std::string& context) {
        const std::size_t field_count = schema.fields().size();
        std::vector<std::vector<TypedField>> slots(field_count);
        std::vector<std::optional<SourcePos>> first_seen(field_count);

        std::vector<std::string> names;
        names.reserve(field_count);
        for (const FieldSpec& f : schema.fields())
            names.push_back(f.name);

        for (const Entry& entry : entries) {
            const int index = entry.key_kind == ValueKind::Ident ? schema.find_field(entry.key) : -1;
            if (index < 0) {
                report_unknown_field(entry.pos, entry.key, names);
                continue;
            }
            const auto idx = static_cast<std::size_t>(index);
            const FieldSpec& spec = schema.fields()[idx];

            if (entry.label) {
                report(
                    entry.pos,
                    ErrorCode::BadLabel,
                    std::format("field '{}' does not take a label (found \"{}\")", spec.name, *entry.label));
            }
            if (entry.op != Op::Eq) {
                report(
                    entry.pos,
                    ErrorCode::BadOperator,
                    std::format("expected '=' after field '{}', found '{}'", spec.name, op_text(entry.op)));
            }
            if (first_seen[idx] && !spec.repeatable) {
                report(entry.pos,
                       ErrorCode::DuplicateField,
                       std::format("duplicate field '{}' (first defined at {}:{})",
                                   spec.name,
                                   first_seen[idx]->line,
                                   first_seen[idx]->column));
                continue;
            }
            if (!first_seen[idx])
                first_seen[idx] = entry.pos;

            std::optional<TypedValue> value = check_value(spec.type, entry.value, spec.name);
            if (value)
                slots[idx].push_back(TypedField{spec.name, entry.pos, false, std::move(*value)});
        }

        TypedBlock block;
        for (std::size_t i = 0; i < field_count; ++i) {
            const FieldSpec& spec = schema.fields()[i];
            if (!first_seen[i]) {
                if (spec.required) {
                    report(where,
                           ErrorCode::MissingField,
                           std::format("missing required field '{}' in {}", spec.name, context));
                } else if (spec.default_value) {
                    TypedValue v = *spec.default_value;
                    v.pos = where;
                    block.fields.push_back(TypedField{spec.name, where, true, std::move(v)});
                }
                continue;
            }
            for (TypedField& f : slots[i])
                block.fields.push_back(std::move(f));
        }
        return block;
    }

private:
    std::vector<Diagnostic>& errors_;

    void report_unknown_field(SourcePos pos, const std::string& name, const std::vector<std::string>& valid) {
        const std::string hint = suggest_similar(name, valid);
        if (!hint.empty()) {
            report(pos,
                   ErrorCode::UnknownField,
                   std::format("unknown field '{}' (did you mean '{}'?)", name, hint));
        } else {
            report(pos,
                   ErrorCode::UnknownField,
                   std::format("unknown field '{}' (valid fields: {})", name, join_quoted(valid, 12)));
        }
    }

    void mismatch(const Type& type, const Value& value) {
        report(value.pos,
               ErrorCode::WrongType,
               std::format("expected {}, found {}", describe_type(type), found_name(value)));
    }

    template <class F>
    std::optional<TypedValue> check_fixed(const Type& type, const Value& value) {
        if (value.kind != ValueKind::Int && value.kind != ValueKind::Decimal) {
            mismatch(type, value);
            return std::nullopt;
        }
        const std::int64_t mantissa = value.integer;
        const unsigned scale = value.kind == ValueKind::Decimal ? value.scale : 0U;
        F result;
        if (!decimal_to_fixed(mantissa, scale, result)) {
            report(value.pos,
                   ErrorCode::ValueOutOfRange,
                   std::format("value {} is out of range for {} ({} .. {})",
                               value_text(value),
                               type.kind == TypeKind::Fixed32 ? "fixed32" : "fixed64",
                               core::to_string(F::min()),
                               core::to_string(F::max())));
            return std::nullopt;
        }
        const core::Fixed64 wide = widen(result);
        if ((type.min_fixed && wide < *type.min_fixed) || (type.max_fixed && wide > *type.max_fixed)) {
            std::optional<std::string> lo;
            std::optional<std::string> hi;
            if (type.min_fixed)
                lo = core::to_string(*type.min_fixed);
            if (type.max_fixed)
                hi = core::to_string(*type.max_fixed);
            report(value.pos,
                   ErrorCode::ValueOutOfRange,
                   std::format("value {} is out of range ({})", value_text(value), range_text(lo, hi)));
            return std::nullopt;
        }
        TypedValue out = TypedValue::of(result);
        out.pos = value.pos;
        return out;
    }

    std::optional<TypedValue> check_value(const Type& type, const Value& value, const std::string& path) {
        std::optional<TypedValue> out;
        switch (type.kind) {
            case TypeKind::String:
                if (value.kind != ValueKind::String) {
                    mismatch(type, value);
                    return std::nullopt;
                }
                out = TypedValue::of(value.text);
                break;
            case TypeKind::Int: {
                if (value.kind != ValueKind::Int) {
                    mismatch(type, value);
                    return std::nullopt;
                }
                if ((type.min_int && value.integer < *type.min_int) ||
                    (type.max_int && value.integer > *type.max_int)) {
                    std::optional<std::string> lo;
                    std::optional<std::string> hi;
                    if (type.min_int)
                        lo = std::to_string(*type.min_int);
                    if (type.max_int)
                        hi = std::to_string(*type.max_int);
                    report(value.pos,
                           ErrorCode::ValueOutOfRange,
                           std::format("value {} is out of range ({})", value.integer, range_text(lo, hi)));
                    return std::nullopt;
                }
                out = TypedValue::of(value.integer);
                break;
            }
            case TypeKind::Fixed32:
                return check_fixed<core::Fixed32>(type, value);
            case TypeKind::Fixed64:
                return check_fixed<core::Fixed64>(type, value);
            case TypeKind::Bool:
                if (value.kind != ValueKind::Bool) {
                    mismatch(type, value);
                    return std::nullopt;
                }
                out = TypedValue::of(value.integer != 0);
                break;
            case TypeKind::Enum: {
                if (value.kind != ValueKind::Ident) {
                    mismatch(type, value);
                    return std::nullopt;
                }
                const auto it = std::find(type.enum_values.begin(), type.enum_values.end(), value.text);
                if (it == type.enum_values.end()) {
                    const std::string hint = suggest_similar(value.text, type.enum_values);
                    std::string message = std::format(
                        "unknown value '{}' (allowed: {})", value.text, join_quoted(type.enum_values, 32));
                    if (!hint.empty())
                        message += std::format(" (did you mean '{}'?)", hint);
                    report(value.pos, ErrorCode::UnknownEnumValue, std::move(message));
                    return std::nullopt;
                }
                out = TypedValue::of(
                    EnumValue{value.text, static_cast<std::size_t>(it - type.enum_values.begin())});
                break;
            }
            case TypeKind::Ref: {
                if (value.kind == ValueKind::Ref) {
                    if (ref_prefix(value) != type.ref_kind) {
                        report(value.pos,
                               ErrorCode::WrongType,
                               std::format("expected {}, found reference to '{}'",
                                           describe_type(type),
                                           ref_prefix(value)));
                        return std::nullopt;
                    }
                    out = TypedValue::of(RefValue{type.ref_kind, std::string(ref_name(value))});
                } else if (value.kind == ValueKind::Ident || value.kind == ValueKind::String) {
                    out = TypedValue::of(RefValue{type.ref_kind, value.text});
                } else {
                    mismatch(type, value);
                    return std::nullopt;
                }
                break;
            }
            case TypeKind::Date: {
                if (value.kind != ValueKind::Date) {
                    mismatch(type, value);
                    return std::nullopt;
                }
                if (!is_valid_julian_date(value.year, value.month, value.day)) {
                    report(value.pos,
                           ErrorCode::InvalidDate,
                           std::format("{}.{:02}.{:02} does not exist in the Julian calendar",
                                       value.year,
                                       value.month,
                                       value.day));
                    return std::nullopt;
                }
                out = TypedValue::of(DateValue{value.year, value.month, value.day});
                break;
            }
            case TypeKind::List: {
                if (value.kind != ValueKind::List) {
                    mismatch(type, value);
                    return std::nullopt;
                }
                TypedList items;
                for (std::size_t i = 0; i < value.items.size(); ++i) {
                    std::optional<TypedValue> item =
                        check_value(*type.element, value.items[i], std::format("{}[{}]", path, i));
                    if (item)
                        items.push_back(std::move(*item));
                }
                out = TypedValue::of(std::move(items));
                break;
            }
            case TypeKind::Block: {
                if (value.kind != ValueKind::Block) {
                    mismatch(type, value);
                    return std::nullopt;
                }
                out = TypedValue::of(
                    check_block(*type.schema, value.entries, value.pos, "block '" + path + "'"));
                break;
            }
            case TypeKind::Map: {
                if (value.kind != ValueKind::Block) {
                    mismatch(type, value);
                    return std::nullopt;
                }
                TypedMap map;
                std::vector<std::pair<std::string, SourcePos>> seen;  // keys in source order
                for (const Entry& entry : value.entries) {
                    if (entry.key_kind != ValueKind::Ident) {
                        report(entry.pos,
                               ErrorCode::WrongType,
                               std::format("map keys must be identifiers, found '{}'", entry.key));
                        continue;
                    }
                    if (entry.label) {
                        report(entry.pos,
                               ErrorCode::BadLabel,
                               std::format("map entry '{}' does not take a label (found \"{}\")",
                                           entry.key,
                                           *entry.label));
                    }
                    if (entry.op != Op::Eq) {
                        report(
                            entry.pos,
                            ErrorCode::BadOperator,
                            std::format("expected '=' after '{}', found '{}'", entry.key, op_text(entry.op)));
                    }
                    const auto previous = std::find_if(
                        seen.begin(), seen.end(), [&](const auto& s) { return s.first == entry.key; });
                    if (previous != seen.end()) {
                        report(entry.pos,
                               ErrorCode::DuplicateField,
                               std::format("duplicate key '{}' (first defined at {}:{})",
                                           entry.key,
                                           previous->second.line,
                                           previous->second.column));
                        continue;
                    }
                    seen.emplace_back(entry.key, entry.pos);
                    std::optional<TypedValue> item =
                        check_value(*type.element, entry.value, path + "." + entry.key);
                    if (item)
                        map.entries.push_back(TypedField{entry.key, entry.pos, false, std::move(*item)});
                }
                out = TypedValue::of(std::move(map));
                break;
            }
        }
        if (out)
            out->pos = value.pos;
        return out;
    }
};

void sort_errors(std::vector<Diagnostic>& errors) {
    std::stable_sort(
        errors.begin(), errors.end(), [](const Diagnostic& a, const Diagnostic& b) { return a.pos < b.pos; });
}

TypedObject check_object(Validator& validator, const ObjectSchema& schema, const Entry& entry) {
    TypedObject object;
    object.kind = schema.kind();
    object.pos = entry.pos;
    if (entry.label) {
        object.id = *entry.label;
    } else {
        validator.report(
            entry.pos,
            ErrorCode::BadLabel,
            std::format("'{}' needs an id: {} \"name\" {{ ... }}", schema.kind(), schema.kind()));
    }
    if (entry.op != Op::Eq) {
        validator.report(
            entry.pos,
            ErrorCode::BadOperator,
            std::format("expected '=' or '{{' after '{}', found '{}'", schema.kind(), op_text(entry.op)));
    }
    if (entry.value.kind != ValueKind::Block) {
        validator.report(entry.value.pos,
                         ErrorCode::WrongType,
                         std::format("expected block, found {}", found_name(entry.value)));
        return object;
    }
    const std::string context =
        object.id.empty() ? schema.kind() : std::format("{} \"{}\"", schema.kind(), object.id);
    object.body = validator.check_block(schema, entry.value.entries, entry.pos, context);
    return object;
}

}  // namespace

bool decimal_to_fixed(std::int64_t mantissa, unsigned scale, core::Fixed32& out) noexcept {
    return decimal_to_fixed_impl(mantissa, scale, out);
}

bool decimal_to_fixed(std::int64_t mantissa, unsigned scale, core::Fixed64& out) noexcept {
    return decimal_to_fixed_impl(mantissa, scale, out);
}

std::string suggest_similar(std::string_view name, const std::vector<std::string>& candidates) {
    const std::string lowered = lower_ascii(name);
    std::size_t best_distance = std::numeric_limits<std::size_t>::max();
    const std::string* best = nullptr;
    for (const std::string& candidate : candidates) {
        const std::string c = lower_ascii(candidate);
        // About one edit per three characters; names shorter than three only match up to letter case.
        const std::size_t limit = std::max(c.size(), lowered.size()) / 3;
        const std::size_t distance = edit_distance(lowered, c);
        if (distance <= limit && distance < best_distance) {
            best_distance = distance;
            best = &candidate;
        }
    }
    return best != nullptr ? *best : std::string{};
}

ObjectResult validate_object(const ObjectSchema& schema, const Entry& entry) {
    ObjectResult result;
    Validator validator(result.errors);
    result.object = check_object(validator, schema, entry);
    sort_errors(result.errors);
    return result;
}

FileResult validate_file(const SchemaSet& schemas, const ParsedFile& file) {
    FileResult result;
    Validator validator(result.errors);
    std::vector<std::string> kinds;
    for (const ObjectSchema& s : schemas.schemas())
        kinds.push_back(s.kind());

    for (const Entry& entry : file.entries) {
        if (entry.key_kind == ValueKind::Ident && entry.key == "format_version")
            continue;
        const ObjectSchema* schema = entry.key_kind == ValueKind::Ident ? schemas.find(entry.key) : nullptr;
        if (schema == nullptr) {
            const std::string hint = suggest_similar(entry.key, kinds);
            validator.report(
                entry.pos,
                ErrorCode::UnknownKind,
                hint.empty() ? std::format("unknown object kind '{}' (known kinds: {})",
                                           entry.key,
                                           join_quoted(kinds, 12))
                             : std::format("unknown object kind '{}' (did you mean '{}'?)", entry.key, hint));
            continue;
        }
        const std::size_t before = result.errors.size();
        TypedObject object = check_object(validator, *schema, entry);
        if (result.errors.size() == before)
            result.objects.push_back(std::move(object));
    }
    sort_errors(result.errors);
    return result;
}

}  // namespace pat::data
