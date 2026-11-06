#include "pat/data/pd_parser.hpp"

#include <string>
#include <utility>

#include "pd_internal.hpp"

namespace pat::data {

namespace {

using detail::Reporter;
using detail::Tok;
using detail::Token;

[[nodiscard]] bool is_key_token(Tok kind) noexcept {
    return kind == Tok::Ident || kind == Tok::Int || kind == Tok::Date;
}

[[nodiscard]] bool is_scalar_token(Tok kind) noexcept {
    switch (kind) {
        case Tok::Ident:
        case Tok::Int:
        case Tok::Decimal:
        case Tok::Date:
        case Tok::Bool:
        case Tok::Str:
        case Tok::Ref:
            return true;
        default:
            return false;
    }
}

class Parser {
public:
    Parser(std::vector<Token> tokens, Reporter& reporter) : tokens_(std::move(tokens)), rep_(reporter) {}

    void parse_file(ParsedFile& out) {
        while (!rep_.fatal() && peek().kind != Tok::Eof) {
            const Token& t = peek();
            if (t.kind == Tok::RBrace || t.kind == Tok::RBracket) {
                report_unmatched(t);
                take();
                continue;
            }
            parse_entry_or_resync(out.entries);
        }
        if (rep_.fatal())
            return;
        check_header(out);
    }

private:
    std::vector<Token> tokens_;
    Reporter& rep_;
    std::size_t at_ = 0;
    int depth_ = 0;

    [[nodiscard]] const Token& peek(std::size_t ahead = 0) const noexcept {
        const std::size_t i = at_ + ahead;
        return i < tokens_.size() ? tokens_[i] : tokens_.back();
    }

    Token& take() {
        Token& t = tokens_[at_];
        if (at_ + 1 < tokens_.size())
            ++at_;  // never step past the final Eof token
        return t;
    }

    void report(const Token& at, ErrorCode code, std::string message) {
        rep_.report(at.pos, code, std::move(message));
    }

    void report_unmatched(const Token& t) {
        report(t, ErrorCode::UnmatchedClose, t.kind == Tok::RBrace ? "unmatched '}'" : "unmatched ']'");
    }

    // Skips the rest of a broken entry (see "Восстановление после ошибки" in the specification).
    void resync() {
        int nesting = 0;
        bool first = true;
        while (!rep_.fatal()) {
            const Token& t = peek();
            if (t.kind == Tok::Eof)
                return;
            const bool closer = t.kind == Tok::RBrace || t.kind == Tok::RBracket || t.kind == Tok::RParen;
            const bool opener = t.kind == Tok::LBrace || t.kind == Tok::LBracket || t.kind == Tok::LParen;
            if (closer && nesting == 0 && t.kind != Tok::RParen)
                return;
            if (!first && nesting == 0 && t.first_on_line && is_key_token(t.kind))
                return;
            if (opener)
                ++nesting;
            if (closer && nesting > 0)
                --nesting;
            take();
            first = false;
        }
    }

    void parse_entry_or_resync(std::vector<Entry>& into) {
        Entry entry;
        if (parse_entry(entry)) {
            into.push_back(std::move(entry));
        } else {
            resync();
        }
    }

    bool parse_entry(Entry& entry) {
        if (rep_.fatal())
            return false;
        const Token& key = peek();
        if (key.kind == Tok::Error)
            return false;
        if (!is_key_token(key.kind)) {
            if (key.kind == Tok::LParen || key.kind == Tok::RParen) {
                report(key, ErrorCode::Reserved, "parentheses are reserved for the rule language");
            } else {
                report(key, ErrorCode::ExpectedKey, "expected a key (identifier, integer or date)");
            }
            return false;
        }
        if (key.kind == Tok::Ident && peek(1).kind == Tok::LParen) {
            report(key, ErrorCode::Reserved, "function call syntax is reserved for the rule language");
            return false;
        }
        entry.key_kind = key.value.kind;
        entry.key = key.kind == Tok::Int ? std::to_string(key.value.integer) : key.value.text;
        entry.pos = key.pos;
        take();

        const Token& next = peek();
        switch (next.kind) {
            case Tok::Op:
                entry.op = next.op;
                take();
                return parse_value(entry.value);
            case Tok::LBrace:
                entry.op = Op::Eq;
                return parse_value(entry.value);
            case Tok::Str:
            case Tok::Ident:
                if (peek(1).kind == Tok::LBrace) {
                    entry.label = next.value.text;
                    entry.op = Op::Eq;
                    take();
                    return parse_value(entry.value);
                }
                break;
            case Tok::Error:
                return false;
            default:
                break;
        }
        report(next, ErrorCode::ExpectedOperator, "expected '=', a comparison operator or '{' after the key");
        return false;
    }

    bool parse_value(Value& out) {
        if (rep_.fatal())
            return false;
        const Token& t = peek();
        if (is_scalar_token(t.kind)) {
            if (t.kind == Tok::Ident && peek(1).kind == Tok::LParen) {
                report(t, ErrorCode::Reserved, "function call syntax is reserved for the rule language");
                return false;
            }
            out = std::move(take().value);
            return true;
        }
        switch (t.kind) {
            case Tok::LBrace:
                return parse_block(out);
            case Tok::LBracket:
                return parse_list(out);
            case Tok::Error:
                return false;
            case Tok::LParen:
            case Tok::RParen:
                report(t, ErrorCode::Reserved, "parentheses are reserved for the rule language");
                return false;
            default:
                report(t, ErrorCode::ExpectedValue, "expected a value");
                return false;
        }
    }

    bool enter(const Token& open) {
        if (++depth_ > kMaxNesting) {
            report(open, ErrorCode::TooDeep, "nesting is deeper than 64 levels");
            return false;
        }
        return true;
    }

    bool parse_block(Value& out) {
        const Token open_tok = take();  // copy: the vector is not modified, but keep the position stable
        if (!enter(open_tok))
            return false;
        out.kind = ValueKind::Block;
        out.pos = open_tok.pos;
        while (!rep_.fatal()) {
            const Token& t = peek();
            if (t.kind == Tok::Eof) {
                rep_.report(open_tok.pos, ErrorCode::Unclosed, "unclosed '{'");
                break;
            }
            if (t.kind == Tok::RBrace) {
                take();
                break;
            }
            if (t.kind == Tok::RBracket) {
                report_unmatched(t);
                take();
                continue;
            }
            parse_entry_or_resync(out.entries);
        }
        --depth_;
        return !rep_.fatal();
    }

    bool parse_list(Value& out) {
        const Token open_tok = take();
        if (!enter(open_tok))
            return false;
        out.kind = ValueKind::List;
        out.pos = open_tok.pos;
        while (!rep_.fatal()) {
            const Token& t = peek();
            if (t.kind == Tok::Eof) {
                rep_.report(open_tok.pos, ErrorCode::Unclosed, "unclosed '['");
                break;
            }
            if (t.kind == Tok::RBracket) {
                take();
                break;
            }
            if (t.kind == Tok::RBrace) {
                report_unmatched(t);
                take();
                continue;
            }
            Value item;
            if (parse_value(item)) {
                out.items.push_back(std::move(item));
            } else {
                resync();
            }
        }
        --depth_;
        return !rep_.fatal();
    }

    void check_header(ParsedFile& file) {
        const SourcePos where = file.entries.empty() ? SourcePos{1, 1} : file.entries.front().pos;
        if (file.entries.empty()) {
            rep_.report(
                where, ErrorCode::MissingVersion, "the file must start with 'format_version = <number>'");
            return;
        }
        const Entry& first = file.entries.front();
        const bool ok = first.key_kind == ValueKind::Ident && first.key == "format_version" &&
                        first.op == Op::Eq && first.value.kind == ValueKind::Int && first.value.integer >= 1;
        if (!ok) {
            rep_.report(where,
                        ErrorCode::MissingVersion,
                        "the first entry of a file must be 'format_version = <positive integer>'");
            return;
        }
        file.format_version = first.value.integer;
    }
};

}  // namespace

ParseResult parse_pd(std::string_view source) {
    ParseResult result;
    Reporter reporter;
    if (source.size() > kMaxFileBytes) {
        reporter.report({1, 1}, ErrorCode::FileTooLarge, "file is larger than 16 MiB");
        result.errors = reporter.finish();
        return result;
    }
    const std::size_t bad = detail::find_invalid_utf8(source);
    if (bad != std::string_view::npos) {
        reporter.report(detail::position_of_offset(source, bad), ErrorCode::BadUtf8, "invalid UTF-8");
        result.errors = reporter.finish();
        return result;
    }
    std::vector<Token> tokens = detail::lex(source, reporter);
    Parser parser(std::move(tokens), reporter);
    parser.parse_file(result.file);
    result.errors = reporter.finish();
    return result;
}

}  // namespace pat::data
