#include "pat/ui/explanation.hpp"

#include <charconv>

namespace pat::ui {

std::string signed_value(int value) {
    if (value > 0) {
        return "+" + std::to_string(value);
    }
    return std::to_string(value);  // "0" or "-N"
}

SplitPath split_tooltip_path(std::string_view path) {
    const std::size_t slash = path.find('/');
    if (slash == std::string_view::npos) {
        return {path, {}};
    }
    return {path.substr(0, slash), path.substr(slash + 1)};
}

const Explanation* find_term(const Explanation& root, std::string_view path) {
    const Explanation* node = &root;
    while (!path.empty()) {
        const std::size_t slash = path.find('/');
        const std::string_view part = path.substr(0, slash);
        std::size_t index = 0;
        const auto [end, ec] = std::from_chars(part.data(), part.data() + part.size(), index);
        if (ec != std::errc{} || end != part.data() + part.size() || index >= node->terms.size()) {
            return nullptr;
        }
        node = &node->terms[index];
        path = slash == std::string_view::npos ? std::string_view{} : path.substr(slash + 1);
        if (slash != std::string_view::npos && path.empty()) {
            return nullptr;  // trailing slash
        }
    }
    return node;
}

std::string escape_markup(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        switch (c) {
            case '&':
                out += "&amp;";
                break;
            case '<':
                out += "&lt;";
                break;
            case '>':
                out += "&gt;";
                break;
            case '"':
                out += "&quot;";
                break;
            default:
                out += c;
                break;
        }
    }
    return out;
}

namespace {

const char* sign_class(const std::string& value) {
    if (!value.empty() && value.front() == '+') {
        return " pos";
    }
    if (!value.empty() && value.front() == '-') {
        return " neg";
    }
    return "";
}

}  // namespace

std::string
render_tooltip_rml(const Explanation& node, std::string_view path, int level, std::string_view inline_style) {
    std::string rml;
    rml += "<rml><head>";
    if (!inline_style.empty()) {
        rml += "<style>";
        rml += inline_style;
        rml += "</style>";
    }
    rml += "</head><body>";
    rml += "<div class=\"tip\" id=\"tip\" data-tip-level=\"" + std::to_string(level) + "\">";
    rml += "<div class=\"tip-head\"><span class=\"tip-title\">" + escape_markup(node.label) + "</span>";
    if (!node.value.empty()) {
        rml += "<span class=\"tip-total\">" + escape_markup(node.value) + "</span>";
    }
    rml += "</div>";
    for (std::size_t i = 0; i < node.terms.size(); ++i) {
        const Explanation& term = node.terms[i];
        const bool nested = !term.terms.empty();
        rml += "<div class=\"tip-row";
        rml += nested ? " nested" : "";
        rml += "\"";
        if (nested) {
            rml += " data-tip=\"" + escape_markup(std::string(path) + "/" + std::to_string(i)) + "\"";
        }
        rml += "><span class=\"tip-label\">" + escape_markup(term.label) + "</span>";
        rml += "<span class=\"tip-value" + std::string(sign_class(term.value)) + "\">" +
               escape_markup(term.value) + "</span></div>";
    }
    if (!node.note.empty()) {
        rml += "<div class=\"tip-note\">" + escape_markup(node.note) + "</div>";
    }
    rml += "</div></body></rml>";
    return rml;
}

}  // namespace pat::ui
