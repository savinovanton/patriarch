#include "pat/data/pd_tree.hpp"

#include <format>

namespace pat::data {

std::string_view op_text(Op op) noexcept {
    switch (op) {
        case Op::Eq:
            return "=";
        case Op::Ne:
            return "!=";
        case Op::Lt:
            return "<";
        case Op::Le:
            return "<=";
        case Op::Gt:
            return ">";
        case Op::Ge:
            return ">=";
    }
    return "?";
}

std::string_view value_kind_name(ValueKind kind) noexcept {
    switch (kind) {
        case ValueKind::Int:
            return "int";
        case ValueKind::Decimal:
            return "dec";
        case ValueKind::Bool:
            return "bool";
        case ValueKind::Date:
            return "date";
        case ValueKind::String:
            return "str";
        case ValueKind::Ident:
            return "ident";
        case ValueKind::Ref:
            return "ref";
        case ValueKind::Block:
            return "block";
        case ValueKind::List:
            return "list";
    }
    return "?";
}

std::string_view ref_prefix(const Value& ref) noexcept {
    const std::string_view text = ref.text;
    return text.substr(0, text.find(':'));
}

std::string_view ref_name(const Value& ref) noexcept {
    const std::string_view text = ref.text;
    const std::size_t colon = text.find(':');
    return colon == std::string_view::npos ? std::string_view{} : text.substr(colon + 1);
}

std::string error_code_name(ErrorCode code) {
    return std::format("PD{:03}", static_cast<unsigned>(code));
}

std::string format_diagnostic(std::string_view file_name, const Diagnostic& diagnostic) {
    return std::format("{}:{}:{}: error[{}]: {}",
                       file_name,
                       diagnostic.pos.line,
                       diagnostic.pos.column,
                       error_code_name(diagnostic.code),
                       diagnostic.message);
}

namespace {

void pad(std::string& out, int indent) {
    out.append(static_cast<std::size_t>(indent) * 2, ' ');
}

void append_quoted(std::string& out, std::string_view text) {
    out += '"';
    for (const char ch : text) {
        const auto c = static_cast<unsigned char>(ch);
        switch (c) {
            case '\\':
                out += "\\\\";
                break;
            case '"':
                out += "\\\"";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\t':
                out += "\\t";
                break;
            case '\r':
                out += "\\r";
                break;
            default:
                if (c < 0x20 || c == 0x7F) {
                    out += std::format("\\u{{{:X}}}", static_cast<unsigned>(c));
                } else {
                    out += ch;
                }
        }
    }
    out += '"';
}

void append_scalar(std::string& out, const Value& v) {
    out += value_kind_name(v.kind);
    out += ':';
    switch (v.kind) {
        case ValueKind::Int:
            out += std::to_string(v.integer);
            break;
        case ValueKind::Bool:
            out += v.integer != 0 ? "yes" : "no";
            break;
        case ValueKind::String:
            append_quoted(out, v.text);
            break;
        default:
            out += v.text;
            break;  // Decimal, Date, Ident, Ref keep their source text
    }
}

void dump_value(std::string& out, const Value& v, int indent);

void dump_entry(std::string& out, const Entry& e, int indent) {
    pad(out, indent);
    out += e.key;
    if (e.label) {
        out += ' ';
        append_quoted(out, *e.label);
    }
    out += ' ';
    out += op_text(e.op);
    out += ' ';
    dump_value(out, e.value, indent);
}

// Writes the value after the "KEY OP " or "- " prefix and the trailing newline.
void dump_value(std::string& out, const Value& v, int indent) {
    switch (v.kind) {
        case ValueKind::Block:
            out += "{\n";
            for (const Entry& e : v.entries)
                dump_entry(out, e, indent + 1);
            pad(out, indent);
            out += "}\n";
            break;
        case ValueKind::List:
            out += "[\n";
            for (const Value& item : v.items) {
                pad(out, indent + 1);
                out += "- ";
                dump_value(out, item, indent + 1);
            }
            pad(out, indent);
            out += "]\n";
            break;
        default:
            append_scalar(out, v);
            out += '\n';
    }
}

}  // namespace

std::string dump_tree(const ParsedFile& file) {
    std::string out;
    for (const Entry& e : file.entries)
        dump_entry(out, e, 0);
    return out;
}

}  // namespace pat::data
