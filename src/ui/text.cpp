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
    const std::string text = format_number(loc, n);
    return n > 0 ? "+" + text : text;
}

}  // namespace pat::ui
