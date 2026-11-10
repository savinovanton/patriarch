#pragma once

#include "pat/content/credits_content.hpp"
#include "pat/loc/localizer.hpp"
#include "pat/ui/credits_window.hpp"

// Bridge from the credits list (data/credits, docs/dev/credits.md) to the credits window.

namespace pat::game {

/// Turns the sorted entries into the sections of the window in the language of `loc`: a key is
/// formatted through the string table, a plain name or text is shown as it is, and the meta line joins
/// the provider with "Лицензия: …" (ui.credits.license). Sections without entries are left out; titles
/// come from `ui.credits.section.<authors|data|assets>`.
[[nodiscard]] ui::CreditsSnapshot credits_snapshot(const loc::Localizer& loc,
                                                   const content::CreditsList& credits);

}  // namespace pat::game
