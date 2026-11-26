#include "pat/core/json_reader.hpp"

#include "pat/core/utf8.hpp"

#include <algorithm>
#include <limits>

namespace pat::core {

namespace {

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    Result<JsonValue, Error> run() {
        using R = Result<JsonValue, Error>;
        JsonValue root;
        skip_ws();
        if (!value(root, 0)) {
            return R::err(Error{1, error_});
        }
        skip_ws();
        if (pos_ != text_.size()) {
            return R::err(Error{1, "JSON: trailing characters at byte " + std::to_string(pos_)});
        }
        return R::ok(std::move(root));
    }

private:
    bool fail(std::string what) {
        if (error_.empty()) {
            error_ = "JSON: " + std::move(what) + " at byte " + std::to_string(pos_);
        }
        return false;
    }

    void skip_ws() {
        while (pos_ < text_.size() &&
               (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\n' || text_[pos_] == '\r')) {
            ++pos_;
        }
    }

    bool literal(std::string_view word) {
        if (text_.substr(pos_, word.size()) != word) {
            return fail("invalid literal");
        }
        pos_ += word.size();
        return true;
    }

    bool value(JsonValue& out, std::size_t depth) {
        if (depth >= kMaxJsonDepth) {
            return fail("nesting is too deep");
        }
        if (pos_ >= text_.size()) {
            return fail("unexpected end of input");
        }
        switch (text_[pos_]) {
            case '{':
                return object(out, depth);
            case '[':
                return array(out, depth);
            case '"':
                out.type = JsonValue::Type::string;
                return string(out.text);
            case 't':
                out.type = JsonValue::Type::boolean;
                out.boolean = true;
                return literal("true");
            case 'f':
                out.type = JsonValue::Type::boolean;
                return literal("false");
            case 'n':
                out.type = JsonValue::Type::null;
                return literal("null");
            default:
                return number(out);
        }
    }

    bool object(JsonValue& out, std::size_t depth) {
        out.type = JsonValue::Type::object;
        ++pos_;  // '{'
        skip_ws();
        if (pos_ < text_.size() && text_[pos_] == '}') {
            ++pos_;
            return true;
        }
        while (true) {
            skip_ws();
            if (pos_ >= text_.size() || text_[pos_] != '"') {
                return fail("expected a member name");
            }
            std::string key;
            if (!string(key)) {
                return false;
            }
            const bool duplicate = std::any_of(
                out.members.begin(), out.members.end(), [&](const auto& m) { return m.first == key; });
            if (duplicate) {
                return fail("duplicate member \"" + key + "\"");
            }
            skip_ws();
            if (pos_ >= text_.size() || text_[pos_] != ':') {
                return fail("expected ':'");
            }
            ++pos_;
            skip_ws();
            JsonValue member;
            if (!value(member, depth + 1)) {
                return false;
            }
            out.members.emplace_back(std::move(key), std::move(member));
            skip_ws();
            if (pos_ < text_.size() && text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (pos_ < text_.size() && text_[pos_] == '}') {
                ++pos_;
                return true;
            }
            return fail("expected ',' or '}'");
        }
    }

    bool array(JsonValue& out, std::size_t depth) {
        out.type = JsonValue::Type::array;
        ++pos_;  // '['
        skip_ws();
        if (pos_ < text_.size() && text_[pos_] == ']') {
            ++pos_;
            return true;
        }
        while (true) {
            skip_ws();
            JsonValue item;
            if (!value(item, depth + 1)) {
                return false;
            }
            out.items.push_back(std::move(item));
            skip_ws();
            if (pos_ < text_.size() && text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (pos_ < text_.size() && text_[pos_] == ']') {
                ++pos_;
                return true;
            }
            return fail("expected ',' or ']'");
        }
    }

    bool hex4(std::uint32_t& out) {
        if (text_.size() - pos_ < 4) {
            return fail("truncated \\u escape");
        }
        out = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = text_[pos_++];
            std::uint32_t d = 0;
            if (c >= '0' && c <= '9') {
                d = static_cast<std::uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                d = static_cast<std::uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                d = static_cast<std::uint32_t>(c - 'A' + 10);
            } else {
                return fail("invalid \\u escape");
            }
            out = out * 16U + d;
        }
        return true;
    }

    static void put_utf8(std::string& s, std::uint32_t cp) {
        if (cp < 0x80U) {
            s.push_back(static_cast<char>(cp));
        } else if (cp < 0x800U) {
            s.push_back(static_cast<char>(0xC0U | (cp >> 6U)));
            s.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
        } else if (cp < 0x10000U) {
            s.push_back(static_cast<char>(0xE0U | (cp >> 12U)));
            s.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
            s.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
        } else {
            s.push_back(static_cast<char>(0xF0U | (cp >> 18U)));
            s.push_back(static_cast<char>(0x80U | ((cp >> 12U) & 0x3FU)));
            s.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
            s.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
        }
    }

    bool string(std::string& out) {
        ++pos_;  // opening quote
        while (true) {
            if (pos_ >= text_.size()) {
                return fail("unterminated string");
            }
            const auto c = static_cast<unsigned char>(text_[pos_]);
            if (c == '"') {
                ++pos_;
                return true;
            }
            if (c < 0x20U) {
                return fail("control character in a string");
            }
            if (c != '\\') {
                const std::size_t len = utf8_sequence_length(text_.substr(pos_));
                if (len == 0) {
                    return fail("invalid UTF-8");
                }
                out.append(text_.substr(pos_, len));
                pos_ += len;
                continue;
            }
            ++pos_;
            if (pos_ >= text_.size()) {
                return fail("unterminated escape");
            }
            const char e = text_[pos_++];
            switch (e) {
                case '"':
                case '\\':
                case '/':
                    out.push_back(e);
                    break;
                case 'b':
                    out.push_back('\b');
                    break;
                case 'f':
                    out.push_back('\f');
                    break;
                case 'n':
                    out.push_back('\n');
                    break;
                case 'r':
                    out.push_back('\r');
                    break;
                case 't':
                    out.push_back('\t');
                    break;
                case 'u': {
                    std::uint32_t cp = 0;
                    if (!hex4(cp)) {
                        return false;
                    }
                    if (cp >= 0xDC00U && cp <= 0xDFFFU) {
                        return fail("unpaired low surrogate");
                    }
                    if (cp >= 0xD800U && cp <= 0xDBFFU) {
                        std::uint32_t low = 0;
                        if (text_.substr(pos_, 2) != "\\u") {
                            return fail("unpaired high surrogate");
                        }
                        pos_ += 2;
                        if (!hex4(low)) {
                            return false;
                        }
                        if (low < 0xDC00U || low > 0xDFFFU) {
                            return fail("unpaired high surrogate");
                        }
                        cp = 0x10000U + ((cp - 0xD800U) << 10U) + (low - 0xDC00U);
                    }
                    put_utf8(out, cp);
                    break;
                }
                default:
                    return fail("invalid escape");
            }
        }
    }

    bool number(JsonValue& out) {
        const std::size_t start = pos_;
        const auto digit = [&]() { return pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9'; };
        if (pos_ < text_.size() && text_[pos_] == '-') {
            ++pos_;
        }
        if (!digit()) {
            return fail("invalid value");
        }
        if (text_[pos_] == '0') {
            ++pos_;
        } else {
            while (digit()) {
                ++pos_;
            }
        }
        if (pos_ < text_.size() && text_[pos_] == '.') {
            ++pos_;
            if (!digit()) {
                return fail("invalid number");
            }
            while (digit()) {
                ++pos_;
            }
        }
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) {
                ++pos_;
            }
            if (!digit()) {
                return fail("invalid number");
            }
            while (digit()) {
                ++pos_;
            }
        }
        out.type = JsonValue::Type::number;
        out.text.assign(text_.substr(start, pos_ - start));
        return true;
    }

    std::string_view text_;
    std::size_t pos_ = 0;
    std::string error_;
};

}  // namespace

const JsonValue* JsonValue::find(std::string_view key) const noexcept {
    for (const auto& m : members) {
        if (m.first == key) {
            return &m.second;
        }
    }
    return nullptr;
}

std::optional<std::int64_t> JsonValue::as_i64() const noexcept {
    if (type != Type::number || text.empty()) {
        return std::nullopt;
    }
    const bool negative = text[0] == '-';
    std::uint64_t magnitude = 0;
    for (std::size_t i = negative ? 1 : 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c < '0' || c > '9') {
            return std::nullopt;  // fraction or exponent
        }
        const auto d = static_cast<std::uint64_t>(c - '0');
        if (magnitude > (std::numeric_limits<std::uint64_t>::max() - d) / 10U) {
            return std::nullopt;
        }
        magnitude = magnitude * 10U + d;
    }
    constexpr auto kMax = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (negative) {
        if (magnitude > kMax + 1U) {
            return std::nullopt;
        }
        return magnitude == kMax + 1U ? std::numeric_limits<std::int64_t>::min()
                                      : -static_cast<std::int64_t>(magnitude);
    }
    if (magnitude > kMax) {
        return std::nullopt;
    }
    return static_cast<std::int64_t>(magnitude);
}

Result<JsonValue, Error> parse_json(std::string_view text) {
    return Parser(text).run();
}

}  // namespace pat::core
