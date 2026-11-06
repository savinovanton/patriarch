#include <algorithm>
#include <format>

#include "pd_internal.hpp"

namespace pat::data::detail {

void Reporter::report(SourcePos pos, ErrorCode code, std::string message) {
    if (fatal_)
        return;
    if (errors_.size() >= kMaxErrors) {
        errors_.push_back({pos, ErrorCode::TooManyErrors, "too many errors, parsing stopped"});
        fatal_ = true;
        fatal_pos_ = pos;
        return;
    }
    errors_.push_back({pos, code, std::move(message)});
    if (code == ErrorCode::TooDeep || code == ErrorCode::BadUtf8 || code == ErrorCode::FileTooLarge) {
        fatal_ = true;
        fatal_pos_ = pos;
    }
}

std::vector<Diagnostic> Reporter::finish() {
    if (fatal_) {
        std::erase_if(errors_, [this](const Diagnostic& d) { return d.pos > fatal_pos_; });
    }
    std::stable_sort(errors_.begin(), errors_.end(), [](const Diagnostic& a, const Diagnostic& b) {
        return a.pos < b.pos;
    });
    return std::move(errors_);
}

SourcePos position_of_offset(std::string_view source, std::size_t offset) {
    SourcePos pos{1, 1};
    offset = std::min(offset, source.size());
    for (std::size_t i = 0; i < offset; ++i) {
        const auto c = static_cast<unsigned char>(source[i]);
        if (c == '\n') {
            ++pos.line;
            pos.column = 1;
        } else if ((c & 0xC0) != 0x80) {
            ++pos.column;
        }
    }
    return pos;
}

std::size_t find_invalid_utf8(std::string_view s) {
    const std::size_t n = s.size();
    std::size_t i = 0;
    while (i < n) {
        const auto c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            ++i;
            continue;
        }
        std::size_t len = 0;
        std::uint32_t cp = 0;
        std::uint32_t min_cp = 0;
        if (c >= 0xC2 && c <= 0xDF) {
            len = 2;
            cp = c & 0x1Fu;
            min_cp = 0x80;
        } else if (c >= 0xE0 && c <= 0xEF) {
            len = 3;
            cp = c & 0x0Fu;
            min_cp = 0x800;
        } else if (c >= 0xF0 && c <= 0xF4) {
            len = 4;
            cp = c & 0x07u;
            min_cp = 0x10000;
        } else {
            return i;
        }
        if (i + len > n)
            return i;
        for (std::size_t k = 1; k < len; ++k) {
            const auto cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80)
                return i;
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        if (cp < min_cp || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
            return i;
        i += len;
    }
    return std::string_view::npos;
}

namespace {

bool is_digit(char c) noexcept {
    return c >= '0' && c <= '9';
}
bool is_ident_start(char c) noexcept {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}
bool is_ident_char(char c) noexcept {
    return is_ident_start(c) || is_digit(c);
}
int hex_value(char c) noexcept {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

void append_utf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

class Lexer {
public:
    Lexer(std::string_view src, Reporter& rep) : src_(src), rep_(rep) {}

    std::vector<Token> run() {
        if (src_.size() >= 3 && src_.substr(0, 3) == "\xEF\xBB\xBF")
            i_ = 3;
        while (!rep_.fatal()) {
            skip_trivia();
            if (i_ >= src_.size())
                break;
            lex_token();
        }
        Token eof;
        eof.kind = Tok::Eof;
        eof.pos = pos();
        eof.first_on_line = true;
        tokens_.push_back(std::move(eof));
        return std::move(tokens_);
    }

private:
    std::string_view src_;
    Reporter& rep_;
    std::size_t i_ = 0;
    std::uint32_t line_ = 1;
    std::uint32_t col_ = 1;
    bool line_start_ = true;
    bool tok_first_ = true;  // the token being lexed is the first one on its line
    std::vector<Token> tokens_;

    [[nodiscard]] SourcePos pos() const noexcept { return {line_, col_}; }
    [[nodiscard]] bool eof() const noexcept { return i_ >= src_.size(); }
    [[nodiscard]] char peek(std::size_t ahead = 0) const noexcept {
        return i_ + ahead < src_.size() ? src_[i_ + ahead] : '\0';
    }

    char advance() {
        const char c = src_[i_++];
        if (c == '\n') {
            ++line_;
            col_ = 1;
            line_start_ = true;
        } else if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) {
            ++col_;
        }
        return c;
    }

    void skip_trivia() {
        while (!eof()) {
            const char c = peek();
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ',') {
                advance();
            } else if (c == '#') {
                while (!eof() && peek() != '\n')
                    advance();
            } else {
                break;
            }
        }
    }

    Token& begin(Tok kind, SourcePos at) {
        Token t;
        t.kind = kind;
        t.pos = at;
        t.first_on_line = tok_first_;
        t.value.pos = at;
        line_start_ = false;
        tokens_.push_back(std::move(t));
        return tokens_.back();
    }

    void error_token(SourcePos at, ErrorCode code, std::string message) {
        begin(Tok::Error, at);
        rep_.report(at, code, std::move(message));
    }

    void lex_token() {
        tok_first_ = line_start_;
        const SourcePos at = pos();
        const char c = peek();
        if (is_ident_start(c))
            return lex_word(at);
        if (is_digit(c) || (c == '-' && is_digit(peek(1))))
            return lex_number(at);
        if (c == '.' && is_digit(peek(1))) {
            advance();
            while (is_ident_char(peek()) || peek() == '.')
                advance();
            return error_token(at, ErrorCode::BadNumber, "malformed number");
        }
        if (c == '"')
            return lex_string(at);
        switch (c) {
            case '=': {
                advance();
                if (peek() == '=') {
                    advance();
                    rep_.report(at, ErrorCode::DoubleEquals, "'==' is not an operator, use '='");
                }
                begin(Tok::Op, at).op = Op::Eq;
                return;
            }
            case '!':
                advance();
                if (peek() == '=') {
                    advance();
                    begin(Tok::Op, at).op = Op::Ne;
                    return;
                }
                return error_token(at, ErrorCode::BadChar, "unexpected character '!'");
            case '<':
            case '>': {
                advance();
                Op op = c == '<' ? Op::Lt : Op::Gt;
                if (peek() == '=') {
                    advance();
                    op = c == '<' ? Op::Le : Op::Ge;
                }
                begin(Tok::Op, at).op = op;
                return;
            }
            case '{':
                advance();
                begin(Tok::LBrace, at);
                return;
            case '}':
                advance();
                begin(Tok::RBrace, at);
                return;
            case '[':
                advance();
                begin(Tok::LBracket, at);
                return;
            case ']':
                advance();
                begin(Tok::RBracket, at);
                return;
            case '(':
                advance();
                begin(Tok::LParen, at);
                return;
            case ')':
                advance();
                begin(Tok::RParen, at);
                return;
            default:
                break;
        }
        // Anything else: consume one whole code point.
        advance();
        while (!eof() && (static_cast<unsigned char>(peek()) & 0xC0) == 0x80)
            advance();
        const auto shown = static_cast<unsigned char>(c) < 0x80 ? std::format("'{}'", c)
                                                                : std::string("a non-ASCII character");
        error_token(at,
                    ErrorCode::BadChar,
                    "unexpected character " + shown +
                        " (non-ASCII text is allowed only in strings and comments)");
    }

    std::string scan_dotted_ident() {
        std::string text;
        while (true) {
            while (is_ident_char(peek()))
                text += advance();
            if (peek() == '.' && is_ident_start(peek(1))) {
                text += advance();
            } else {
                break;
            }
        }
        return text;
    }

    void lex_word(SourcePos at) {
        std::string text = scan_dotted_ident();
        bool is_ref = false;
        if (peek() == ':' && is_ident_start(peek(1))) {
            text += advance();
            text += scan_dotted_ident();
            is_ref = true;
        }
        if (text.size() > kMaxIdentBytes) {
            return error_token(at, ErrorCode::OutOfRange, "identifier is longer than 256 characters");
        }
        if (!is_ref && (text == "true" || text == "false")) {
            rep_.report(at, ErrorCode::TrueFalse, "use 'yes' or 'no' instead of '" + text + "'");
        }
        Tok kind = is_ref ? Tok::Ref : Tok::Ident;
        ValueKind vk = is_ref ? ValueKind::Ref : ValueKind::Ident;
        std::int64_t integer = 0;
        if (!is_ref && (text == "yes" || text == "no")) {
            kind = Tok::Bool;
            vk = ValueKind::Bool;
            integer = text == "yes" ? 1 : 0;
        }
        Token& t = begin(kind, at);
        t.value.kind = vk;
        t.value.integer = integer;
        t.value.text = std::move(text);
    }

    std::string_view scan_digits() {
        const std::size_t start = i_;
        while (is_digit(peek()))
            advance();
        return src_.substr(start, i_ - start);
    }

    static std::int64_t digits_to_int(std::string_view digits) noexcept {
        std::int64_t v = 0;
        for (const char d : digits)
            v = v * 10 + (d - '0');
        return v;
    }

    void lex_number(SourcePos at) {
        const std::size_t start = i_;
        const bool negative = peek() == '-';
        if (negative)
            advance();
        const std::string_view d1 = scan_digits();
        std::string_view d2;
        std::string_view d3;
        int groups = 1;
        if (peek() == '.' && is_digit(peek(1))) {
            advance();
            d2 = scan_digits();
            groups = 2;
            if (peek() == '.' && is_digit(peek(1))) {
                advance();
                d3 = scan_digits();
                groups = 3;
                while (peek() == '.' && is_digit(peek(1))) {
                    advance();
                    scan_digits();
                    groups = 4;
                }
            }
        }
        if (is_ident_char(peek())) {
            while (is_ident_char(peek()))
                advance();
            return error_token(at, ErrorCode::BadNumber, "malformed number");
        }
        if (peek() == '.') {
            advance();
            return error_token(
                at, ErrorCode::BadNumber, "malformed number: digits are required after the decimal point");
        }
        const std::string text(src_.substr(start, i_ - start));
        if (groups == 4)
            return error_token(at, ErrorCode::BadNumber, "malformed number");
        if (groups == 3)
            return finish_date(at, text, negative, d1, d2, d3);
        if (groups == 2) {
            if (d2.size() > 9 || d1.size() + d2.size() > 18) {
                return error_token(at,
                                   ErrorCode::OutOfRange,
                                   "decimal number is out of range (at most 18 digits, 9 after the point)");
            }
            std::string all(d1);
            all += d2;
            Token& t = begin(Tok::Decimal, at);
            t.value.kind = ValueKind::Decimal;
            const std::int64_t mantissa = digits_to_int(all);
            t.value.integer = negative ? -mantissa : mantissa;
            t.value.scale = static_cast<std::uint8_t>(d2.size());
            t.value.text = text;
            return;
        }
        if (d1.size() > 18) {
            return error_token(at, ErrorCode::OutOfRange, "integer is out of range (at most 18 digits)");
        }
        Token& t = begin(Tok::Int, at);
        t.value.kind = ValueKind::Int;
        const std::int64_t v = digits_to_int(d1);
        t.value.integer = negative ? -v : v;
    }

    void finish_date(SourcePos at,
                     const std::string& text,
                     bool negative,
                     std::string_view y,
                     std::string_view m,
                     std::string_view d) {
        const bool shape_ok = !negative && y.size() <= 4 && m.size() == 2 && d.size() == 2;
        const std::int64_t year = digits_to_int(y.substr(0, 4));
        const std::int64_t month = shape_ok ? digits_to_int(m) : 0;
        const std::int64_t day = shape_ok ? digits_to_int(d) : 0;
        if (!shape_ok || year < 1 || month < 1 || month > 12 || day < 1 || day > 31) {
            return error_token(at, ErrorCode::BadDate, "malformed date, expected YYYY.MM.DD");
        }
        Token& t = begin(Tok::Date, at);
        t.value.kind = ValueKind::Date;
        t.value.year = static_cast<std::int32_t>(year);
        t.value.month = static_cast<std::uint8_t>(month);
        t.value.day = static_cast<std::uint8_t>(day);
        t.value.text = text;
    }

    void lex_string(SourcePos at) {
        if (peek(1) == '"' && peek(2) == '"')
            return lex_block_string(at);
        advance();  // opening quote
        std::string text;
        bool bad_escape = false;
        while (true) {
            if (eof() || peek() == '\n' || peek() == '\r') {
                return error_token(at, ErrorCode::UnterminatedString, "unterminated string");
            }
            const char c = advance();
            if (c == '"')
                break;
            if (c != '\\') {
                text += c;
                continue;
            }
            const SourcePos esc_at{line_, col_ - 1};
            const char e = peek();
            std::uint32_t cp = 0;
            bool ok = true;
            switch (e) {
                case '\\':
                    text += '\\';
                    advance();
                    break;
                case '"':
                    text += '"';
                    advance();
                    break;
                case 'n':
                    text += '\n';
                    advance();
                    break;
                case 't':
                    text += '\t';
                    advance();
                    break;
                case 'r':
                    text += '\r';
                    advance();
                    break;
                case 'u': {
                    advance();
                    ok = peek() == '{';
                    if (ok) {
                        advance();
                        int digits = 0;
                        while (hex_value(peek()) >= 0 && digits < 7) {
                            cp = cp * 16 + static_cast<std::uint32_t>(hex_value(advance()));
                            ++digits;
                        }
                        ok = digits >= 1 && digits <= 6 && peek() == '}';
                        if (peek() == '}')
                            advance();
                        ok = ok && cp != 0 && cp <= 0x10FFFF && !(cp >= 0xD800 && cp <= 0xDFFF);
                    }
                    if (ok)
                        append_utf8(text, cp);
                    break;
                }
                default:
                    ok = false;
                    if (!eof() && e != '\n' && e != '\r' && e != '"')
                        advance();
                    break;
            }
            if (!ok && !bad_escape) {
                bad_escape = true;
                rep_.report(esc_at, ErrorCode::BadEscape, "invalid escape sequence");
            }
        }
        if (text.size() > kMaxStringBytes) {
            return error_token(at, ErrorCode::OutOfRange, "string is longer than 1 MiB");
        }
        if (bad_escape) {
            begin(Tok::Error, at);
            return;
        }
        Token& t = begin(Tok::Str, at);
        t.value.kind = ValueKind::String;
        t.value.text = std::move(text);
    }

    void lex_block_string(SourcePos at) {
        advance();
        advance();
        advance();
        if (peek() == '\r' && peek(1) == '\n') {
            advance();
            advance();
        } else if (peek() == '\n') {
            advance();
        }
        std::string text;
        while (true) {
            if (eof()) {
                return error_token(at, ErrorCode::UnterminatedBlockString, "unterminated block string");
            }
            if (peek() == '"' && peek(1) == '"' && peek(2) == '"') {
                advance();
                advance();
                advance();
                break;
            }
            const char c = advance();
            if (c == '\r' && peek() == '\n')
                continue;  // normalise CRLF to LF
            text += c;
        }
        if (text.size() > kMaxStringBytes) {
            return error_token(at, ErrorCode::OutOfRange, "string is longer than 1 MiB");
        }
        Token& t = begin(Tok::Str, at);
        t.value.kind = ValueKind::String;
        t.value.text = std::move(text);
    }
};

}  // namespace

std::vector<Token> lex(std::string_view source, Reporter& reporter) {
    return Lexer(source, reporter).run();
}

}  // namespace pat::data::detail
