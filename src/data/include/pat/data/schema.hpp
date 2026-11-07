#pragma once

#include "pat/core/fixed.hpp"
#include "pat/data/pd_tree.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// Declarative schemas for .pd objects (E4-T03, docs/dev/data-schemas.md).
//
// A schema describes the fields of one object kind (`trait "greedy" { ... }`): their types, whether
// they are required, defaults, ranges and whether a key may repeat. Validating a parsed entry against
// a schema yields typed values plus ALL problems found in one pass, each with a source position, a
// stable code (PD101..PD110) and a helpful message ("did you mean ...?").
//
// Determinism: nothing here uses floating point. Decimals are converted from their exact
// mantissa/scale to Fixed32/Fixed64 with 128-bit integer arithmetic (truncated toward zero, like
// Fixed::from_ratio). Output order depends only on the schema and the input.

namespace pat::data {

class ObjectSchema;

// ---- types -----------------------------------------------------------------------------------

enum class TypeKind : std::uint8_t { String, Int, Fixed32, Fixed64, Bool, Enum, Ref, Date, List, Block, Map };

/// Type of one field value. Build with the static factory functions.
struct Type {
    TypeKind kind = TypeKind::String;
    std::optional<std::int64_t> min_int;         // Int
    std::optional<std::int64_t> max_int;         // Int
    std::optional<core::Fixed64> min_fixed;      // Fixed32 / Fixed64 (compared at Q32.32)
    std::optional<core::Fixed64> max_fixed;      // Fixed32 / Fixed64
    std::vector<std::string> enum_values;        // Enum: the closed set of identifiers
    std::string ref_kind;                        // Ref: the object kind that is referenced
    std::shared_ptr<const Type> element;         // List: element type; Map: value type
    std::shared_ptr<const ObjectSchema> schema;  // Block: the nested object schema

    [[nodiscard]] static Type string();
    [[nodiscard]] static Type integer(std::optional<std::int64_t> min = {},
                                      std::optional<std::int64_t> max = {});
    [[nodiscard]] static Type fixed32(std::optional<core::Fixed64> min = {},
                                      std::optional<core::Fixed64> max = {});
    [[nodiscard]] static Type fixed64(std::optional<core::Fixed64> min = {},
                                      std::optional<core::Fixed64> max = {});
    [[nodiscard]] static Type boolean();
    [[nodiscard]] static Type enumeration(std::vector<std::string> values);
    /// Reference to an object of `kind`. Accepts `kind:id`, a bare identifier or a string. Resolving
    /// the reference is the job of the registry (E4-T04); here only kind, id and position are recorded.
    [[nodiscard]] static Type reference(std::string kind);
    /// Calendar date `Y.MM.DD`; the day must exist in the Julian calendar.
    [[nodiscard]] static Type date();
    [[nodiscard]] static Type list(Type element);
    [[nodiscard]] static Type block(ObjectSchema schema);
    /// Block of `identifier = value` entries with arbitrary identifier keys and values of one type.
    [[nodiscard]] static Type map(Type value);
};

/// Human-readable type name used in messages ("int", "list of string", "reference to 'trait'").
[[nodiscard]] std::string describe_type(const Type& type);

// ---- typed values ----------------------------------------------------------------------------

struct TypedValue;
struct TypedField;

struct EnumValue {
    std::string name;
    std::size_t index = 0;  // position in the schema's list of allowed values

    [[nodiscard]] bool operator==(const EnumValue&) const = default;
};

struct RefValue {
    std::string kind;
    std::string id;

    [[nodiscard]] bool operator==(const RefValue&) const = default;
};

struct DateValue {
    std::int32_t year = 0;
    std::int32_t month = 1;
    std::int32_t day = 1;

    [[nodiscard]] bool operator==(const DateValue&) const = default;
};

using TypedList = std::vector<TypedValue>;

/// Fields of a nested block in schema order (repeated fields appear in source order).
struct TypedBlock {
    std::vector<TypedField> fields;

    /// First field with this name, or nullptr.
    [[nodiscard]] const TypedValue* find(std::string_view name) const;
    /// All fields with this name (repeatable fields), in source order.
    [[nodiscard]] std::vector<const TypedValue*> find_all(std::string_view name) const;
};

/// `key -> value` pairs of a map in source order.
struct TypedMap {
    std::vector<TypedField> entries;

    [[nodiscard]] const TypedValue* find(std::string_view key) const;
};

struct TypedValue {
    using Data = std::variant<std::int64_t,
                              std::string,
                              core::Fixed32,
                              core::Fixed64,
                              bool,
                              EnumValue,
                              RefValue,
                              DateValue,
                              TypedList,
                              TypedBlock,
                              TypedMap>;

    SourcePos pos;
    Data data;

    /// Builds a value with an explicit alternative, e.g. `TypedValue::of(std::int64_t{5})`,
    /// `TypedValue::of(core::Fixed32::from_int(1))`; used for schema defaults.
    /// Defined after TypedField: default-constructing the variant needs TypedBlock's destructor,
    /// which needs the complete TypedField (Clang checks this at the point of definition).
    template <class T>
    [[nodiscard]] static TypedValue of(T value);

    /// Pointer to the stored alternative, or nullptr when the value holds another type.
    template <class T>
    [[nodiscard]] const T* get_if() const noexcept {
        return std::get_if<T>(&data);
    }
};

struct TypedField {
    std::string name;
    SourcePos pos;           // position of the key (the object for defaulted fields)
    bool defaulted = false;  // true when the value comes from the schema default
    TypedValue value;
};

template <class T>
TypedValue TypedValue::of(T value) {
    TypedValue v;
    v.data = Data(std::in_place_type<T>, std::move(value));
    return v;
}

/// A validated top-level object: `kind "id" { ... }`.
struct TypedObject {
    std::string kind;
    std::string id;
    SourcePos pos;
    TypedBlock body;
};

// ---- schemas ---------------------------------------------------------------------------------

struct FieldSpec {
    std::string name;
    Type type;
    bool required = false;
    bool repeatable = false;
    std::optional<TypedValue> default_value;
};

/// The fields of one object kind (or of a nested block). Fluent builder.
class ObjectSchema {
public:
    explicit ObjectSchema(std::string kind_name = {}) : kind_(std::move(kind_name)) {}

    /// A field that must be present exactly once.
    ObjectSchema& required(std::string name, Type type);
    /// A field that may be absent (no value then) or present once.
    ObjectSchema& optional(std::string name, Type type);
    /// Optional field; when absent the result contains `default_value` (marked `defaulted`).
    ObjectSchema& optional(std::string name, Type type, TypedValue default_value);
    /// An optional field whose key may occur many times (multimap).
    ObjectSchema& repeatable(std::string name, Type type);

    [[nodiscard]] const std::string& kind() const noexcept { return kind_; }
    [[nodiscard]] const std::vector<FieldSpec>& fields() const noexcept { return fields_; }
    /// Index of the field with this exact name, or -1.
    [[nodiscard]] int find_field(std::string_view name) const noexcept;

private:
    std::string kind_;
    std::vector<FieldSpec> fields_;
};

/// All object kinds known to a validation run, looked up by the key of a top-level entry.
class SchemaSet {
public:
    /// Adds a kind; a second schema with the same kind name replaces the first.
    SchemaSet& add(ObjectSchema schema);

    [[nodiscard]] const ObjectSchema* find(std::string_view kind) const noexcept;
    [[nodiscard]] const std::vector<ObjectSchema>& schemas() const noexcept { return schemas_; }

private:
    std::vector<ObjectSchema> schemas_;
};

// ---- validation ------------------------------------------------------------------------------

struct ObjectResult {
    TypedObject object;
    std::vector<Diagnostic> errors;  // sorted by position; empty on success

    [[nodiscard]] bool ok() const noexcept { return errors.empty(); }
};

/// Validates one top-level entry (`kind "id" { ... }`) against its schema. All errors are reported.
/// When errors is not empty the object holds only the fields that were valid.
[[nodiscard]] ObjectResult validate_object(const ObjectSchema& schema, const Entry& entry);

struct FileResult {
    std::vector<TypedObject> objects;  // objects without errors, in file order
    std::vector<Diagnostic> errors;    // sorted by position; empty on success

    [[nodiscard]] bool ok() const noexcept { return errors.empty(); }
};

/// Validates every top-level object of a parsed file. The `format_version` header entry is skipped.
/// An entry whose key is not a known kind is PD108.
[[nodiscard]] FileResult validate_file(const SchemaSet& schemas, const ParsedFile& file);

/// True when `year.month.day` exists in the Julian calendar (leap year = every year divisible by 4,
/// the same rule as pat::sim::Date, which this library deliberately does not depend on).
[[nodiscard]] bool is_valid_julian_date(std::int32_t year, std::int32_t month, std::int32_t day) noexcept;

/// Converts an exact decimal (`mantissa / 10^scale`) to a fixed-point number without floating
/// point, truncating toward zero. Returns false when the value does not fit the type.
[[nodiscard]] bool decimal_to_fixed(std::int64_t mantissa, unsigned scale, core::Fixed32& out) noexcept;
[[nodiscard]] bool decimal_to_fixed(std::int64_t mantissa, unsigned scale, core::Fixed64& out) noexcept;

/// Best match for `name` among `candidates` (edit distance, case-insensitive), or an empty string.
/// Ties are resolved in favour of the earlier candidate.
[[nodiscard]] std::string suggest_similar(std::string_view name, const std::vector<std::string>& candidates);

}  // namespace pat::data
