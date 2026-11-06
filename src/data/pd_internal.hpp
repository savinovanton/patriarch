#pragma once

#include "pat/data/pd_parser.hpp"
#include "pat/data/pd_tree.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Private pieces shared by the lexer and the parser of the .pd format.

namespace pat::data::detail {

enum class Tok : std::uint8_t {
    Eof,
    Error,  // a lexical error was already reported; the parser abandons the entry silently
    Ident,
    Int,
    Decimal,
    Date,
    Bool,
    Str,
    Ref,
    Op,
    LBrace,
    RBrace,
    LBracket,
    RBracket,
    LParen,
    RParen,
};

struct Token {
    Tok kind = Tok::Eof;
    SourcePos pos;
    bool first_on_line = false;
    Op op = Op::Eq;
    Value value;  // filled for scalar tokens (kind and pos already set)
};

/// Collects diagnostics, enforces the error cap and tracks fatal errors.
class Reporter {
public:
    void report(SourcePos pos, ErrorCode code, std::string message);

    [[nodiscard]] bool fatal() const noexcept { return fatal_; }
    /// Sorted, with everything after a fatal error dropped.
    [[nodiscard]] std::vector<Diagnostic> finish();

private:
    std::vector<Diagnostic> errors_;
    bool fatal_ = false;
    SourcePos fatal_pos_;
};

/// Position (line, column in code points) of a byte offset.
[[nodiscard]] SourcePos position_of_offset(std::string_view source, std::size_t offset);

/// Offset of the first byte that is not valid UTF-8, or npos.
[[nodiscard]] std::size_t find_invalid_utf8(std::string_view source);

/// Splits the source into tokens. The result always ends with an Eof token.
[[nodiscard]] std::vector<Token> lex(std::string_view source, Reporter& reporter);

}  // namespace pat::data::detail
