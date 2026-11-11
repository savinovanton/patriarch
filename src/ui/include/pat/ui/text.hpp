#pragma once

#include "pat/loc/localizer.hpp"

#include <cstdint>
#include <string>
#include <string_view>

// Small helpers that turn numbers into text through the string table (ui.fmt.*). Windows and the code
// that builds their snapshots share them, so "12 480" and "94 т" look the same everywhere.

namespace pat::ui {

/// "12 480": the number with group separators (ui.fmt.number).
[[nodiscard]] std::string format_number(const loc::Localizer& loc, std::int64_t n);

/// "94 т": the number in thousands (ui.fmt.thousands).
[[nodiscard]] std::string format_thousands(const loc::Localizer& loc, std::int64_t n);

/// "+84", "0" or "-12": a change with an explicit plus sign and a true minus sign U+2212 (the sign decides
/// the colour in the HUD).
[[nodiscard]] std::string format_delta(const loc::Localizer& loc, std::int64_t n);

/// Upper case for Latin and Cyrillic letters in UTF-8 ("Голицыных" -> "ГОЛИЦЫНЫХ"): RmlUi's
/// `text-transform: uppercase` works for Latin only. Used for names that come from the world and sit in
/// upper-case captions ("РОД ГОЛИЦЫНЫХ"); fixed captions are written upper case in the string table.
[[nodiscard]] std::string upper_text(std::string_view text);

}  // namespace pat::ui
