#include "pat/ui/theme.hpp"

#include "pat/core/log.hpp"
#include "pat/data/pd_parser.hpp"
#include "pat/engine/ui.hpp"
#include "pat/ui/focus_visibility.hpp"

#include <RmlUi/Core.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <utility>

namespace pat::ui {

namespace {

constexpr const char* kLogCat = "ui";

using R = core::Result<Theme>;

core::Error theme_error(const data::SourcePos& pos, const std::string& message) {
    return core::Error{
        1, "theme: " + std::to_string(pos.line) + ":" + std::to_string(pos.column) + ": " + message};
}

const data::Entry* find_entry(const std::vector<data::Entry>& entries, std::string_view key) {
    for (const data::Entry& e : entries) {
        if (e.key == key) {
            return &e;
        }
    }
    return nullptr;
}

int hex_digit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/// An integer or decimal value as thousandths; false for anything else or more than three decimals.
bool to_milli(const data::Value& v, std::int32_t& out) {
    if (v.kind == data::ValueKind::Int) {
        out = static_cast<std::int32_t>(v.integer * 1000);
        return true;
    }
    if (v.kind == data::ValueKind::Decimal && v.scale <= 3) {
        std::int64_t milli = v.integer;
        for (int i = v.scale; i < 3; ++i) {
            milli *= 10;
        }
        out = static_cast<std::int32_t>(milli);
        return true;
    }
    return false;
}

/// 8000 -> "8", 1500 -> "1.5", 1350 -> "1.35".
std::string format_milli(std::int32_t milli) {
    std::string out = std::to_string(milli / 1000);
    const int frac = milli % 1000;
    if (frac != 0) {
        std::string digits = std::to_string(1000 + frac).substr(1);
        while (!digits.empty() && digits.back() == '0') {
            digits.pop_back();
        }
        out += "." + digits;
    }
    return out;
}

std::string dp(std::int32_t milli) {
    return format_milli(milli) + "dp";
}

std::string kebab(std::string name) {
    std::replace(name.begin(), name.end(), '_', '-');
    return name;
}

constexpr const char* kRequiredColors[] = {"ink",
                                           "panel",
                                           "raised",
                                           "paper",
                                           "paper2",
                                           "text",
                                           "muted",
                                           "light",
                                           "quiet",
                                           "gold",
                                           "rule",
                                           "line",
                                           "green",
                                           "green_hover",
                                           "success",
                                           "red",
                                           "softred"};

/// Reads a group of `name = number` entries into `numbers`.
bool read_numbers(const data::Entry* group, std::vector<ThemeNumber>& numbers, core::Error& error) {
    if (group == nullptr) {
        return true;
    }
    for (const data::Entry& e : group->value.entries) {
        std::int32_t milli = 0;
        if (!to_milli(e.value, milli)) {
            error = theme_error(e.pos, "'" + e.key + "' must be a number");
            return false;
        }
        numbers.push_back({e.key, milli});
    }
    return true;
}

std::string read_file(const std::filesystem::path& path, bool& ok) {
    std::ifstream in(path, std::ios::binary);
    if (!in.good()) {
        ok = false;
        return {};
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    ok = true;
    return buffer.str();
}

bool is_token_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

/// Resolves one token name to its stylesheet text; false when the theme has no such token.
bool resolve_token(const Theme& theme, std::string_view name, std::string& out) {
    if (name == "line_height") {
        out = format_milli(theme.line_height_milli);
        return true;
    }
    if (const ThemeFont* font = theme.find_font(name)) {
        out = "\"" + font->family + "\"";
        return true;
    }
    if (const ThemeNumber* number = theme.find_number(name)) {
        out = dp(number->milli);
        return true;
    }
    if (const ThemeColor* color = theme.find_color(name)) {
        out = color_hex(*color);
        return true;
    }
    // "<colour>_a<percent>": the colour with an alpha.
    const std::size_t cut = name.rfind("_a");
    if (cut != std::string_view::npos && cut + 2 < name.size()) {
        const std::string_view digits = name.substr(cut + 2);
        const bool numeric =
            std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; });
        if (numeric && digits.size() <= 3) {
            const ThemeColor* color = theme.find_color(name.substr(0, cut));
            const int percent = std::stoi(std::string(digits));
            if (color != nullptr && percent <= 100) {
                out = color_hex_alpha(*color, percent);
                return true;
            }
        }
    }
    return false;
}

}  // namespace

const ThemeColor* Theme::find_color(std::string_view name) const noexcept {
    for (const ThemeColor& c : colors) {
        if (c.name == name) {
            return &c;
        }
    }
    return nullptr;
}

const ThemeNumber* Theme::find_number(std::string_view name) const noexcept {
    for (const ThemeNumber& n : numbers) {
        if (n.name == name) {
            return &n;
        }
    }
    return nullptr;
}

const ThemeFont* Theme::find_font(std::string_view key) const noexcept {
    for (const ThemeFont& f : fonts) {
        if (f.key == key) {
            return &f;
        }
    }
    return nullptr;
}

std::string color_hex(const ThemeColor& color) {
    constexpr const char* kDigits = "0123456789ABCDEF";
    std::string out = "#";
    for (const std::uint8_t channel : {color.r, color.g, color.b}) {
        out += kDigits[channel >> 4];
        out += kDigits[channel & 15];
    }
    return out;
}

std::string color_hex_alpha(const ThemeColor& color, int percent) {
    constexpr const char* kDigits = "0123456789ABCDEF";
    const int alpha = (std::clamp(percent, 0, 100) * 255 + 50) / 100;
    std::string out = color_hex(color);
    out += kDigits[alpha >> 4];
    out += kDigits[alpha & 15];
    return out;
}

R parse_theme(std::string_view pd_text) {
    const data::ParseResult parsed = data::parse_pd(pd_text);
    if (!parsed.ok()) {
        return R::err(core::Error{1, "theme: " + data::format_diagnostic("theme.pd", parsed.errors.front())});
    }
    const data::Entry* block = find_entry(parsed.file.entries, "theme");
    if (block == nullptr || block->value.kind != data::ValueKind::Block) {
        return R::err(core::Error{1, "theme: no `theme` block"});
    }
    const auto& body = block->value.entries;

    Theme theme;
    theme.id = block->label.value_or("");

    if (const data::Entry* reference = find_entry(body, "reference")) {
        for (const data::Entry& e : reference->value.entries) {
            if (e.value.kind != data::ValueKind::Int || e.value.integer <= 0) {
                return R::err(theme_error(e.pos, "'" + e.key + "' must be a positive integer"));
            }
            if (e.key == "width") {
                theme.reference_width = static_cast<int>(e.value.integer);
            } else if (e.key == "height") {
                theme.reference_height = static_cast<int>(e.value.integer);
            }
        }
    }

    const data::Entry* colors = find_entry(body, "color");
    if (colors == nullptr) {
        return R::err(core::Error{1, "theme: no `color` group"});
    }
    for (const data::Entry& e : colors->value.entries) {
        const std::string& text = e.value.text;
        if (e.value.kind != data::ValueKind::String || text.size() != 7 || text[0] != '#') {
            return R::err(theme_error(e.pos, "colour '" + e.key + "' must be a string \"#RRGGBB\""));
        }
        int channels[3] = {};
        for (int i = 0; i < 3; ++i) {
            const int hi = hex_digit(text[1 + 2 * i]);
            const int lo = hex_digit(text[2 + 2 * i]);
            if (hi < 0 || lo < 0) {
                return R::err(theme_error(e.pos, "colour '" + e.key + "' is not a hex colour"));
            }
            channels[i] = hi * 16 + lo;
        }
        theme.colors.push_back({e.key,
                                static_cast<std::uint8_t>(channels[0]),
                                static_cast<std::uint8_t>(channels[1]),
                                static_cast<std::uint8_t>(channels[2])});
    }
    for (const char* required : kRequiredColors) {
        if (theme.find_color(required) == nullptr) {
            return R::err(core::Error{1, std::string("theme: missing colour '") + required + "'"});
        }
    }

    core::Error error;
    for (const char* group : {"spacing", "font_size", "font_size_extra", "component"}) {
        if (!read_numbers(find_entry(body, group), theme.numbers, error)) {
            return R::err(error);
        }
    }

    const data::Entry* shape = find_entry(body, "shape");
    if (shape != nullptr) {
        for (const data::Entry& e : shape->value.entries) {
            std::int32_t milli = 0;
            if (to_milli(e.value, milli)) {  // `shadow = none` is not a number and is skipped
                theme.numbers.push_back({e.key, milli});
            }
        }
    }
    for (const char* required : {"type_14", "radius", "stroke_hairline"}) {
        if (theme.find_number(required) == nullptr) {
            return R::err(core::Error{1, std::string("theme: missing number '") + required + "'"});
        }
    }

    const data::Entry* font = find_entry(body, "font");
    if (font == nullptr) {
        return R::err(core::Error{1, "theme: no `font` group"});
    }
    for (const data::Entry& e : font->value.entries) {
        if (e.key == "line_height") {
            if (!to_milli(e.value, theme.line_height_milli)) {
                return R::err(theme_error(e.pos, "line_height must be a number"));
            }
        } else if (e.value.kind == data::ValueKind::Block) {
            const data::Entry* family = find_entry(e.value.entries, "family");
            if (family == nullptr || family->value.kind != data::ValueKind::String ||
                family->value.text.empty()) {
                return R::err(theme_error(e.pos, "font '" + e.key + "' has no family"));
            }
            theme.fonts.push_back({e.key, family->value.text});
        }
    }
    for (const char* required : {"serif", "sans"}) {
        if (theme.find_font(required) == nullptr) {
            return R::err(core::Error{1, std::string("theme: missing font '") + required + "'"});
        }
    }
    return R::ok(std::move(theme));
}

R load_theme(const std::filesystem::path& file) {
    bool ok = false;
    const std::string text = read_file(file, ok);
    if (!ok) {
        return R::err(core::Error{1, "theme: cannot read " + file.generic_string()});
    }
    return parse_theme(text);
}

ExpandResult expand_tokens(const Theme& theme, std::string_view rcss) {
    ExpandResult result;
    result.text.reserve(rcss.size());
    std::size_t i = 0;
    while (i < rcss.size()) {
        if (rcss[i] != '$') {
            result.text += rcss[i++];
            continue;
        }
        std::size_t end = i + 1;
        while (end < rcss.size() && is_token_char(rcss[end])) {
            ++end;
        }
        const std::string_view name = rcss.substr(i + 1, end - i - 1);
        std::string value;
        if (!name.empty() && resolve_token(theme, name, value)) {
            result.text += value;
        } else {
            result.text.append(rcss.substr(i, end - i));
            if (!name.empty()) {
                result.unknown.emplace_back(name);
            }
        }
        i = end;
    }
    return result;
}

std::string generate_theme_rcss(const Theme& theme) {
    std::ostringstream out;
    out << "/* GENERATED by pat::ui::generate_theme_rcss from data/ui/theme.pd (theme \"" << theme.id
        << "\"): do not edit.\n   The layout is drawn for " << theme.reference_width << "x"
        << theme.reference_height
        << ": 1dp = 1 design pixel (the application scales dp, docs/dev/ui-design.md section 10). */\n\n";

    // RmlUi has no user-agent stylesheet: block-level elements must be declared.
    out << "div, p, h1, h2, h3 {\n    display: block;\n}\n\n";

    // A document body covers the whole screen: it must not swallow the mouse meant for the map or for windows
    // below it. pointer-events is inherited, so every interactive block opts back in (.panel, .interactive).
    out << "body {\n    width: 100%;\n    height: 100%;\n    font-family: " << "$sans"
        << ";\n    font-size: $type_14;\n"
        << "    line-height: $line_height;\n    color: $light;\n    pointer-events: none;\n}\n\n";
    out << ".interactive {\n    pointer-events: auto;\n}\n\n";
    out << ".panel {\n    pointer-events: auto;\n    background-color: $panel;\n    border: $stroke_hairline "
           "$rule;\n"
        << "    border-radius: $radius;\n}\n\n";
    out << ".heading {\n    font-family: $serif;\n    font-weight: bold;\n    color: $light;\n}\n\n";
    out << ".muted {\n    color: $quiet;\n}\n\n";

    for (const ThemeFont& font : theme.fonts) {
        out << "." << kebab(font.key) << " {\n    font-family: $" << font.key << ";\n}\n";
    }
    out << "\n";
    for (const ThemeColor& c : theme.colors) {
        const std::string name = kebab(c.name);
        out << ".fg-" << name << " { color: $" << c.name << "; }\n";
        out << ".bg-" << name << " { background-color: $" << c.name << "; }\n";
        out << ".bd-" << name << " { border-color: $" << c.name << "; }\n";
    }
    out << "\n";
    for (const ThemeNumber& n : theme.numbers) {
        if (n.name.rfind("type_", 0) == 0) {
            out << ".type-" << n.name.substr(5) << " { font-size: $" << n.name << "; }\n";
        }
    }
    return expand_tokens(theme, out.str()).text;
}

std::string recolor_svg(std::string_view svg, const ThemeColor& color) {
    const std::string hex = color_hex(color);
    std::string out;
    out.reserve(svg.size());
    std::size_t i = 0;
    while (i < svg.size()) {
        std::size_t attr_len = 0;
        for (const std::string_view attr : {std::string_view("stroke=\"#"), std::string_view("fill=\"#")}) {
            if (svg.compare(i, attr.size(), attr) == 0) {
                attr_len = attr.size();
            }
        }
        if (attr_len == 0) {
            out += svg[i++];
            continue;
        }
        out.append(svg.substr(i, attr_len - 1));  // up to the '#'
        i += attr_len - 1;
        const std::size_t close = svg.find('"', i);
        if (close == std::string_view::npos) {
            out.append(svg.substr(i));
            break;
        }
        out += hex;
        i = close;
    }
    return out;
}

void install_theme(engine::UiContext& ui, std::shared_ptr<const Theme> theme) {
    if (Rml::Context* context = ui.context()) {
        install_focus_visibility(*context);
    }
    ui.set_file_provider(
        [theme = std::move(theme)](const std::filesystem::path& path) -> std::optional<std::string> {
            const std::string name = path.filename().string();
            const std::string ext = path.extension().string();
            if (name == "theme.rcss") {
                return generate_theme_rcss(*theme);
            }
            bool ok = false;
            if (ext == ".rcss") {
                const std::string text = read_file(path, ok);
                if (!ok) {
                    return std::nullopt;
                }
                ExpandResult expanded = expand_tokens(*theme, text);
                for (const std::string& token : expanded.unknown) {
                    PAT_LOG_WARN(kLogCat, "{}: unknown theme token '${}'", name, token);
                }
                return std::move(expanded.text);
            }
            if (ext == ".svg") {
                const std::string stem = path.stem().string();
                const std::size_t at = stem.rfind('@');
                if (at == std::string::npos) {
                    return std::nullopt;
                }
                const ThemeColor* color = theme->find_color(stem.substr(at + 1));
                if (color == nullptr) {
                    PAT_LOG_WARN(kLogCat, "{}: unknown colour token '{}'", name, stem.substr(at + 1));
                    return std::nullopt;
                }
                std::filesystem::path base = path;
                base.replace_filename(stem.substr(0, at) + ".svg");
                const std::string text = read_file(base, ok);
                if (!ok) {
                    return std::nullopt;
                }
                return recolor_svg(text, *color);
            }
            return std::nullopt;
        });
}

}  // namespace pat::ui
