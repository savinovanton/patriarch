#pragma once

#include "pat/core/result.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Minimal strict JSON reader (RFC 8259) for offline tool inputs such as the stage-1 geodata
// descriptor (docs/dev/geodata-pipeline.md). Not for the simulation: it allocates freely.
//
// Numbers keep their literal text; as_i64() accepts only integers (no fraction or exponent), so
// nothing is rounded through floating point. Object members keep file order; duplicate keys are
// an error. Nesting is limited to kMaxJsonDepth.

namespace pat::core {

inline constexpr std::size_t kMaxJsonDepth = 64;

struct JsonMember;

struct JsonValue {
    enum class Type : std::uint8_t { null, boolean, number, string, array, object };

    Type type = Type::null;
    bool boolean = false;
    std::string text;  ///< String value (decoded UTF-8) or the literal text of a number.
    std::vector<JsonValue> items;
    std::vector<JsonMember> members;  ///< Object members in file order.

    [[nodiscard]] bool is_null() const noexcept { return type == Type::null; }
    [[nodiscard]] bool is_string() const noexcept { return type == Type::string; }
    [[nodiscard]] bool is_object() const noexcept { return type == Type::object; }
    [[nodiscard]] bool is_array() const noexcept { return type == Type::array; }

    /// Member of an object by key, or nullptr (also for non-objects).
    [[nodiscard]] const JsonValue* find(std::string_view key) const noexcept;
    /// An integer number that fits an i64; nullopt for anything else.
    [[nodiscard]] std::optional<std::int64_t> as_i64() const noexcept;
};

/// One member of a JSON object (a separate struct: std::pair would need JsonValue to be complete).
struct JsonMember {
    std::string key;
    JsonValue value;
};

/// Parses one JSON document (surrounding whitespace allowed). The error message names the byte
/// offset of the problem; the error code is always 1.
[[nodiscard]] Result<JsonValue, Error> parse_json(std::string_view text);

}  // namespace pat::core
