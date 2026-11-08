#pragma once

#include "pat/core/result.hpp"
#include "pat/loc/plural.hpp"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Message syntax of the localisation module (E4-T06, docs/dev/localization.md): compile a message
// text once (at load time, with validation) and format it many times without allocation.
//
//   text          literal text; `{{` and `}}` are a literal `{` and `}`
//   {name}        the parameter `name`: an integer (decimal, no locale) or a string
//   {name:group}  an integer with groups of three digits separated by U+00A0: 12480 -> "12 480"
//   {n:plural(a|b|c)}  the form of the language's plural rule chosen by the integer `n`
//
// Nothing here uses floating point.

namespace pat::loc {

// ---- arguments -------------------------------------------------------------------------------

/// One named argument: an integer or a string (the string must outlive the format call).
class Arg {
public:
    /// Any integral type; stored as int64 (an unsigned value above INT64_MAX wraps).
    template <std::integral T>
    constexpr Arg(std::string_view name, T value) noexcept
        : name_(name), number_(static_cast<std::int64_t>(value)) {}
    constexpr Arg(std::string_view name, std::string_view text) noexcept
        : name_(name), text_(text), is_text_(true) {}
    constexpr Arg(std::string_view name, const char* text) noexcept
        : name_(name), text_(text != nullptr ? std::string_view(text) : std::string_view()), is_text_(true) {}

    [[nodiscard]] constexpr std::string_view name() const noexcept { return name_; }
    [[nodiscard]] constexpr bool is_text() const noexcept { return is_text_; }
    [[nodiscard]] constexpr std::int64_t number() const noexcept { return number_; }
    [[nodiscard]] constexpr std::string_view text() const noexcept { return text_; }

private:
    std::string_view name_;
    std::int64_t number_ = 0;
    std::string_view text_;
    bool is_text_ = false;
};

// ---- output ----------------------------------------------------------------------------------

/// What happened while formatting. `size` is the number of bytes appended/written.
struct FormatResult {
    std::size_t size = 0;
    bool truncated = false;     // a fixed buffer was too small; the output ends on a code point boundary
    bool missing_key = false;   // the key is in no language of the chain; the placeholder was written
    bool bad_argument = false;  // a parameter was absent or had the wrong type; `{!name}` was written

    [[nodiscard]] bool ok() const noexcept { return !truncated && !missing_key && !bad_argument; }
};

/// Where formatted text goes: either appended to a std::string (no allocation while its capacity
/// suffices) or written into a fixed span (never allocates; stops at a code point boundary when full).
class Output {
public:
    explicit Output(std::string& target) noexcept : str_(&target) {}
    explicit Output(std::span<char> target) noexcept : span_(target) {}

    void append(std::string_view piece);

    [[nodiscard]] std::size_t size() const noexcept { return written_; }
    [[nodiscard]] bool truncated() const noexcept { return truncated_; }

private:
    std::string* str_ = nullptr;
    std::span<char> span_;
    std::size_t written_ = 0;
    bool truncated_ = false;
};

/// Size of the scratch buffer format_integer() needs: sign, 19 digits and 6 two-byte separators.
inline constexpr std::size_t kIntegerBufferSize = 40;

/// Decimal text of `value` without any locale; with `group`, groups of three digits are separated by
/// U+00A0 (UTF-8 C2 A0). The returned view points into `buffer`.
[[nodiscard]] std::string_view
format_integer(std::span<char, kIntegerBufferSize> buffer, std::int64_t value, bool group) noexcept;

// ---- compiled messages -----------------------------------------------------------------------

enum class MessageErrorCode : std::uint8_t {
    UnmatchedClose = 10,     // a single `}` outside a field
    UnterminatedField = 11,  // `{` without the closing `}`
    BadName = 12,            // empty or malformed parameter name, junk inside a field
    UnknownModifier = 13,    // a modifier other than `group` or `plural(...)`
    BadPlural = 14,          // malformed `plural(...)`: missing `)`, braces inside forms
    WrongFormCount = 15,     // the number of forms differs from the language's rule
    TooLong = 16,            // the message does not fit the compiled representation
};

/// "LC010" style name of a message error code.
[[nodiscard]] std::string message_error_code_name(MessageErrorCode code);

struct MessageError {
    MessageErrorCode code = MessageErrorCode::UnmatchedClose;
    std::size_t offset = 0;  // byte offset in the message text where the problem is
    std::string message;
};

/// A message parsed into segments once, formatted many times. Owns its text.
class CompiledMessage {
public:
    /// Validates and compiles `text` for a language with plural rule `rule`.
    [[nodiscard]] static core::Result<CompiledMessage, MessageError> compile(std::string_view text,
                                                                             const PluralRule& rule);

    /// Appends the formatted message to `out`. Does not allocate by itself.
    void format(Output& out, std::span<const Arg> args, FormatResult& status) const;

    /// Names of all parameters used, sorted and without duplicates (allocates; for tools).
    [[nodiscard]] std::vector<std::string> parameter_names() const;

    /// True when the message has no parameters at all (plain text).
    [[nodiscard]] bool is_plain() const noexcept;

private:
    struct Segment {
        enum class Kind : std::uint8_t { Literal, Param, Group, Plural };
        Kind kind = Kind::Literal;
        std::uint8_t form_count = 0;
        std::uint32_t text_begin = 0;  // Literal: the text; otherwise the parameter name (in pool_)
        std::uint32_t text_len = 0;
        std::uint32_t forms_begin = 0;  // Plural: first entry in forms_
    };
    struct TextRef {
        std::uint32_t begin = 0;
        std::uint32_t len = 0;
    };

    [[nodiscard]] std::string_view view(std::uint32_t begin, std::uint32_t len) const noexcept {
        return std::string_view(pool_).substr(begin, len);
    }

    std::string pool_;
    std::vector<Segment> segments_;
    std::vector<TextRef> forms_;
    PluralRule rule_;
};

}  // namespace pat::loc
