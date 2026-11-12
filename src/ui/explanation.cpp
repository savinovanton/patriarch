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
    // ASCII hyphen or U+2212 (E2 88 92).
    if (!value.empty() && (value.front() == '-' || value.rfind("−", 0) == 0)) {
        return " neg";
    }
    return "";
}

}  // namespace

std::string render_tooltip_body(const Explanation& node, std::string_view path, std::string_view icons) {
    std::string rml;
    rml += "<div class=\"tip-close\" id=\"tip-close\" data-tip-close=\"1\">";
    if (!icons.empty()) {
        rml += "<svg src=\"" + escape_markup(icons) + "/close@quiet.svg\"/>";
    }
    rml += "</div>";
    const bool total_row = !node.total_label.empty() && !node.value.empty();
    rml += "<div class=\"tip-head\"><span class=\"tip-title";
    rml += node.tone == TipTone::Warning ? " warn" : "";
    rml += "\">" + escape_markup(node.label) + "</span>";
    if (!node.value.empty() && !total_row) {
        rml += "<span class=\"tip-total\">" + escape_markup(node.value) + "</span>";
    }
    rml += "</div>";
    if (!node.note.empty()) {
        rml += "<div class=\"tip-note\">" + escape_markup(node.note) + "</div>";
    }
    if (!node.headline.empty()) {
        rml += "<div class=\"tip-headline\">" + escape_markup(node.headline) + "</div>";
    }
    for (const std::string& line : node.lines) {
        rml += "<div class=\"tip-line\">" + escape_markup(line) + "</div>";
    }
    if (!node.terms.empty()) {
        rml += "<div class=\"tip-rule\"></div>";
    }
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
        if (!term.note.empty()) {
            rml += "<span class=\"tip-factor\">" + escape_markup(term.note) + "</span>";
        }
        rml += "<span class=\"tip-value" + std::string(sign_class(term.value)) + "\">" +
               escape_markup(term.value) + "</span></div>";
    }
    if (total_row) {
        rml += "<div class=\"tip-rule\"></div><div class=\"tip-sum\"><span class=\"tip-sum-label\">" +
               escape_markup(node.total_label) + "</span><span class=\"tip-sum-value" +
               std::string(sign_class(node.value)) + "\">" + escape_markup(node.value) + "</span></div>";
    }
    if (!node.footer.empty()) {
        rml += "<div class=\"tip-footer\">" + escape_markup(node.footer) + "</div>";
    }
    return rml;
}

std::string
render_tooltip_rml(const Explanation& node, std::string_view path, int level, const TooltipRender& render) {
    std::string rml;
    rml += "<rml><head>";
    if (!render.style.empty()) {
        rml += "<style>";
        rml += render.style;
        rml += "</style>";
    }
    rml += "</head><body class=\"tip-doc\">";
    rml += "<div class=\"tip tip-float";
    rml += node.wide ? " tip-wide" : "";
    rml += render.pinned ? " pinned" : "";
    rml += "\" id=\"tip\" data-tip-level=\"" + std::to_string(level) + "\">";
    rml += render_tooltip_body(node, path, render.icons);
    rml += "</div></body></rml>";
    return rml;
}

}  // namespace pat::ui
