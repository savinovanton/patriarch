#include "pat/loc/message.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <limits>
#include <utility>

namespace pat::loc {

namespace {

constexpr std::string_view kGroupSeparator = "\xC2\xA0";  // U+00A0 NO-BREAK SPACE

bool is_name_start(char c) noexcept {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

bool is_name_char(char c) noexcept {
    return is_name_start(c) || (c >= '0' && c <= '9');
}

const Arg* find_arg(std::span<const Arg> args, std::string_view name) noexcept {
    for (const Arg& arg : args) {
        if (arg.name() == name)
            return &arg;
    }
    return nullptr;
}

}  // namespace

// ---- Output ----------------------------------------------------------------------------------

void Output::append(std::string_view piece) {
    if (str_ != nullptr) {
        str_->append(piece);
        written_ += piece.size();
        return;
    }
    if (truncated_ || piece.empty())
        return;
    const std::size_t room = span_.size() - written_;
    std::size_t n = std::min(room, piece.size());
    if (n < piece.size()) {
        // Do not cut a multi-byte sequence: step back while the first dropped byte is a continuation.
        while (n > 0 && (static_cast<unsigned char>(piece[n]) & 0xC0) == 0x80)
            --n;
        truncated_ = true;
    }
    std::copy_n(piece.data(), n, span_.data() + written_);
    written_ += n;
}

// ---- integers --------------------------------------------------------------------------------

std::string_view
format_integer(std::span<char, kIntegerBufferSize> buffer, std::int64_t value, bool group) noexcept {
    // Digits are produced from the end of the buffer towards the front.
    std::size_t pos = buffer.size();
    const bool negative = value < 0;
    std::uint64_t magnitude =
        negative ? 0 - static_cast<std::uint64_t>(value) : static_cast<std::uint64_t>(value);
    int digits = 0;
    do {
        if (group && digits > 0 && digits % 3 == 0) {
            pos -= kGroupSeparator.size();
            buffer[pos] = kGroupSeparator[0];
            buffer[pos + 1] = kGroupSeparator[1];
        }
        buffer[--pos] = static_cast<char>('0' + magnitude % 10);
        magnitude /= 10;
        ++digits;
    } while (magnitude != 0);
    if (negative)
        buffer[--pos] = '-';
    return std::string_view(buffer.data() + pos, buffer.size() - pos);
}

// ---- errors ----------------------------------------------------------------------------------

std::string message_error_code_name(MessageErrorCode code) {
    return std::format("LC{:03}", static_cast<unsigned>(code));
}

// ---- compile ---------------------------------------------------------------------------------

namespace {

using Err = core::Result<CompiledMessage, MessageError>;

Err fail(MessageErrorCode code, std::size_t offset, std::string message) {
    return Err::err(MessageError{code, offset, std::move(message)});
}

}  // namespace

core::Result<CompiledMessage, MessageError> CompiledMessage::compile(std::string_view text,
                                                                     const PluralRule& rule) {
    if (text.size() >= std::numeric_limits<std::uint32_t>::max() / 2)
        return fail(MessageErrorCode::TooLong, 0, "message is too long");

    CompiledMessage out;
    out.rule_ = rule;
    std::string literal;  // the literal run being accumulated, with escapes resolved

    const auto flush_literal = [&out, &literal]() {
        if (literal.empty())
            return;
        Segment seg;
        seg.kind = Segment::Kind::Literal;
        seg.text_begin = static_cast<std::uint32_t>(out.pool_.size());
        seg.text_len = static_cast<std::uint32_t>(literal.size());
        out.pool_ += literal;
        out.segments_.push_back(seg);
        literal.clear();
    };

    std::size_t i = 0;
    const std::size_t n = text.size();
    while (i < n) {
        const char c = text[i];
        if (c == '}') {
            if (i + 1 < n && text[i + 1] == '}') {
                literal += '}';
                i += 2;
                continue;
            }
            return fail(
                MessageErrorCode::UnmatchedClose, i, "unmatched '}' (write '}}' for a literal brace)");
        }
        if (c != '{') {
            literal += c;
            ++i;
            continue;
        }
        if (i + 1 < n && text[i + 1] == '{') {
            literal += '{';
            i += 2;
            continue;
        }

        // A field: `{name}`, `{name:group}` or `{name:plural(a|b|c)}`.
        const std::size_t open = i;
        std::size_t p = i + 1;
        if (p >= n)
            return fail(MessageErrorCode::UnterminatedField, open, "unterminated field: '{' without '}'");
        if (!is_name_start(text[p])) {
            if (text[p] == '}')
                return fail(MessageErrorCode::BadName,
                            p,
                            "empty parameter name (write '{{' for a literal brace; positional "
                            "parameters are not supported)");
            return fail(MessageErrorCode::BadName,
                        p,
                        "a parameter name starts with a letter or '_' (write '{{' for a literal brace)");
        }
        const std::size_t name_begin = p;
        while (p < n && is_name_char(text[p]))
            ++p;
        const std::string_view name = text.substr(name_begin, p - name_begin);
        if (p >= n)
            return fail(MessageErrorCode::UnterminatedField, open, "unterminated field: '{' without '}'");

        Segment seg;
        if (text[p] == '}') {
            seg.kind = Segment::Kind::Param;
            ++p;
        } else if (text[p] == ':') {
            ++p;
            const std::size_t mod_begin = p;
            while (p < n && is_name_char(text[p]))
                ++p;
            const std::string_view modifier = text.substr(mod_begin, p - mod_begin);
            if (modifier == "group") {
                if (p >= n)
                    return fail(
                        MessageErrorCode::UnterminatedField, open, "unterminated field: '{' without '}'");
                if (text[p] != '}')
                    return fail(MessageErrorCode::BadName, p, "'group' takes no arguments; expected '}'");
                seg.kind = Segment::Kind::Group;
                ++p;
            } else if (modifier == "plural") {
                if (p >= n || text[p] != '(')
                    return fail(
                        MessageErrorCode::BadPlural, p, "'plural' needs its forms: {n:plural(one|few|many)}");
                ++p;                                                     // '('
                std::vector<std::pair<std::size_t, std::size_t>> forms;  // (begin, end) offsets in text
                std::size_t form_begin = p;
                bool closed = false;
                while (p < n) {
                    const char f = text[p];
                    if (f == '{' || f == '}')
                        return fail(
                            MessageErrorCode::BadPlural, p, "braces are not allowed inside plural forms");
                    if (f == '|') {
                        forms.emplace_back(form_begin, p);
                        form_begin = p + 1;
                    } else if (f == ')') {
                        forms.emplace_back(form_begin, p);
                        closed = true;
                        ++p;
                        break;
                    }
                    ++p;
                }
                if (!closed)
                    return fail(MessageErrorCode::BadPlural, open, "unterminated plural forms: missing ')'");
                if (p >= n)
                    return fail(
                        MessageErrorCode::UnterminatedField, open, "unterminated field: '{' without '}'");
                if (text[p] != '}')
                    return fail(MessageErrorCode::BadPlural, p, "expected '}' after the plural forms");
                if (forms.size() != rule.form_count)
                    return fail(MessageErrorCode::WrongFormCount,
                                mod_begin,
                                std::format("plural has {} form(s), the language needs {}",
                                            forms.size(),
                                            static_cast<unsigned>(rule.form_count)));
                ++p;  // '}'
                seg.kind = Segment::Kind::Plural;
                seg.form_count = rule.form_count;
                seg.forms_begin = static_cast<std::uint32_t>(out.forms_.size());
                for (const auto& [b, e] : forms) {
                    TextRef ref;
                    ref.begin = static_cast<std::uint32_t>(out.pool_.size());
                    ref.len = static_cast<std::uint32_t>(e - b);
                    out.pool_ += text.substr(b, e - b);
                    out.forms_.push_back(ref);
                }
            } else {
                if (modifier.empty())
                    return fail(MessageErrorCode::UnknownModifier,
                                mod_begin,
                                "empty modifier after ':' (known: group, plural)");
                return fail(MessageErrorCode::UnknownModifier,
                            mod_begin,
                            std::format("unknown modifier '{}' (known: group, plural)", modifier));
            }
        } else {
            return fail(
                MessageErrorCode::BadName,
                p,
                std::format("unexpected character after the parameter name '{}'; expected '}}' or ':'",
                            name));
        }

        flush_literal();
        seg.text_begin = static_cast<std::uint32_t>(out.pool_.size());
        seg.text_len = static_cast<std::uint32_t>(name.size());
        out.pool_ += name;
        out.segments_.push_back(seg);
        i = p;
    }
    flush_literal();
    return Err::ok(std::move(out));
}

// ---- format ----------------------------------------------------------------------------------

void CompiledMessage::format(Output& out, std::span<const Arg> args, FormatResult& status) const {
    std::array<char, kIntegerBufferSize> scratch{};
    const auto bad = [&out, &status](std::string_view name) {
        status.bad_argument = true;
        out.append("{!");
        out.append(name);
        out.append("}");
    };

    for (const Segment& seg : segments_) {
        const std::string_view text = view(seg.text_begin, seg.text_len);
        switch (seg.kind) {
            case Segment::Kind::Literal:
                out.append(text);
                break;
            case Segment::Kind::Param: {
                const Arg* arg = find_arg(args, text);
                if (arg == nullptr)
                    bad(text);
                else if (arg->is_text())
                    out.append(arg->text());
                else
                    out.append(format_integer(scratch, arg->number(), false));
                break;
            }
            case Segment::Kind::Group: {
                const Arg* arg = find_arg(args, text);
                if (arg == nullptr || arg->is_text())
                    bad(text);
                else
                    out.append(format_integer(scratch, arg->number(), true));
                break;
            }
            case Segment::Kind::Plural: {
                const Arg* arg = find_arg(args, text);
                if (arg == nullptr || arg->is_text()) {
                    bad(text);
                    break;
                }
                const std::size_t form = plural_form(rule_, arg->number());
                const TextRef& ref =
                    forms_[seg.forms_begin + std::min<std::size_t>(form, seg.form_count - 1u)];
                out.append(view(ref.begin, ref.len));
                break;
            }
        }
    }
}

std::vector<std::string> CompiledMessage::parameter_names() const {
    std::vector<std::string> names;
    for (const Segment& seg : segments_) {
        if (seg.kind != Segment::Kind::Literal)
            names.emplace_back(view(seg.text_begin, seg.text_len));
    }
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}

bool CompiledMessage::is_plain() const noexcept {
    return std::all_of(segments_.begin(), segments_.end(), [](const Segment& seg) {
        return seg.kind == Segment::Kind::Literal;
    });
}

}  // namespace pat::loc
