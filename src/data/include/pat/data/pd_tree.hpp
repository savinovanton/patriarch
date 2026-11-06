#pragma once

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Untyped parse tree of the .pd data format (docs/dev/data-format.md). The tree carries source
// positions; types, required fields and cross references are checked by later stages (schemas,
// registry). Nothing here uses floating point: decimals are kept as an exact mantissa and scale.

namespace pat::data {

/// 1-based line and column; the column counts Unicode code points.
struct SourcePos {
    std::uint32_t line = 0;
    std::uint32_t column = 0;

    [[nodiscard]] auto operator<=>(const SourcePos&) const = default;
};

enum class Op : std::uint8_t { Eq, Ne, Lt, Le, Gt, Ge };

[[nodiscard]] std::string_view op_text(Op op) noexcept;

enum class ValueKind : std::uint8_t { Int, Decimal, Bool, Date, String, Ident, Ref, Block, List };

[[nodiscard]] std::string_view value_kind_name(ValueKind kind) noexcept;

struct Entry;

/// One value. Which fields are meaningful depends on `kind`:
///   Int      integer
///   Decimal  integer = signed mantissa, scale = digits after the point, text = source text
///   Bool     integer = 0 or 1
///   Date     year/month/day, text = source text (calendar validity is checked by schemas)
///   String   text = content with escapes resolved
///   Ident    text
///   Ref      text = "prefix:name"
///   Block    entries
///   List     items
struct Value {
    ValueKind kind = ValueKind::Int;
    SourcePos pos;
    std::int64_t integer = 0;
    std::uint8_t scale = 0;
    std::int32_t year = 0;
    std::uint8_t month = 0;
    std::uint8_t day = 0;
    std::string text;
    std::vector<Entry> entries;
    std::vector<Value> items;
};

/// `key [label] op value`. The key is an identifier, an integer or a date (key_kind tells which;
/// `key` holds the canonical text). `key { ... }` is stored as `key = { ... }`.
struct Entry {
    ValueKind key_kind = ValueKind::Ident;
    std::string key;
    std::optional<std::string> label;
    Op op = Op::Eq;
    SourcePos pos;  // position of the key
    Value value;
};

/// Prefix of a Ref value ("scope" in "scope:governor").
[[nodiscard]] std::string_view ref_prefix(const Value& ref) noexcept;
/// Name part of a Ref value ("governor" in "scope:governor").
[[nodiscard]] std::string_view ref_name(const Value& ref) noexcept;

/// Stable error codes; the numeric value is the PDnnn number from the specification.
enum class ErrorCode : std::uint16_t {
    BadChar = 1,
    UnterminatedString = 2,
    BadEscape = 3,
    BadNumber = 4,
    BadDate = 5,
    OutOfRange = 6,
    BadUtf8 = 7,
    ExpectedOperator = 10,
    ExpectedValue = 11,
    Reserved = 12,
    UnmatchedClose = 13,
    Unclosed = 14,
    TooDeep = 15,
    ExpectedKey = 16,
    TrueFalse = 18,
    DoubleEquals = 19,
    MissingVersion = 20,
    UnsupportedVersion = 21,
    TooManyErrors = 22,
    FileTooLarge = 23,
    UnterminatedBlockString = 24,
};

/// "PD001" style name of an error code.
[[nodiscard]] std::string error_code_name(ErrorCode code);

struct Diagnostic {
    SourcePos pos;
    ErrorCode code = ErrorCode::BadChar;
    std::string message;
};

/// `file:line:column: error[PDnnn]: message`
[[nodiscard]] std::string format_diagnostic(std::string_view file_name, const Diagnostic& diagnostic);

struct ParsedFile {
    std::int64_t format_version = 0;  // 0 when the header is missing or malformed
    std::vector<Entry> entries;       // all top-level entries, including `format_version`
};

/// Canonical text form of a tree (specification section 5); used by the test corpus and for debugging.
[[nodiscard]] std::string dump_tree(const ParsedFile& file);

}  // namespace pat::data
