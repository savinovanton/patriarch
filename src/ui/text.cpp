#include "pat/ui/text.hpp"

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
    // RmlUi's text-transform handles Latin only, and names of the world are Russian. Latin and Cyrillic
    // (U+0430..U+044F -> U+0410..U+042F, U+0450..U+045F -> U+0400..U+040F) are mapped, the rest is kept.
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c >= 'a' && c <= 'z') {
            out += static_cast<char>(c - 'a' + 'A');
        } else if ((c == 0xD0 || c == 0xD1) && i + 1 < text.size()) {
            const auto d = static_cast<unsigned char>(text[i + 1]);
            const unsigned cp = ((c & 0x1Fu) << 6) | (d & 0x3Fu);
            unsigned mapped = cp;
            if (cp >= 0x430 && cp <= 0x44F) {
                mapped = cp - 0x20;
            } else if (cp >= 0x450 && cp <= 0x45F) {
                mapped = cp - 0x50;
            }
            out += static_cast<char>(0xC0u | (mapped >> 6));
            out += static_cast<char>(0x80u | (mapped & 0x3Fu));
            ++i;
        } else {
            out += static_cast<char>(c);
        }
    }
    return out;
}

}  // namespace pat::ui
