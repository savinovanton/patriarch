#include "pat/ui/text.hpp"

#include "pat/core/utf8.hpp"

namespace pat::ui {

using loc::Arg;

std::string format_number(const loc::Localizer& loc, std::int64_t n) {
    return loc.format("ui.fmt.number", {Arg("n", n)});
}

std::string format_thousands(const loc::Localizer& loc, std::int64_t n) {
    return loc.format("ui.fmt.thousands", {Arg("n", n)});
}

std::string format_delta(const loc::Localizer& loc, std::int64_t n) {
    if (n < 0) {
        // U+2212 MINUS SIGN, as in the mockup (a hyphen is too short next to digits).
        return std::string("\xE2\x88\x92") + format_number(loc, -n);
    }
    const std::string text = format_number(loc, n);
    return n > 0 ? "+" + text : text;
}

std::string upper_text(std::string_view text) {
    // RmlUi's text-transform handles Latin only, and names of the world are Russian.
    return core::to_upper_utf8(text);
}

}  // namespace pat::ui
