#pragma once

#include "pat/loc/localizer.hpp"

#include <cstdint>
#include <string>

// Small helpers that turn numbers into text through the string table (ui.fmt.*). Windows and the code
// that builds their snapshots share them, so "12 480" and "94 т" look the same everywhere.

namespace pat::ui {

/// "12 480": the number with group separators (ui.fmt.number).
[[nodiscard]] std::string format_number(const loc::Localizer& loc, std::int64_t n);

/// "94 т": the number in thousands (ui.fmt.thousands).
[[nodiscard]] std::string format_thousands(const loc::Localizer& loc, std::int64_t n);

/// "+84", "0" or "-12": a change with an explicit plus sign (the sign decides the colour in the HUD).
[[nodiscard]] std::string format_delta(const loc::Localizer& loc, std::int64_t n);

}  // namespace pat::ui
