#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Plural rules and the language registry of the localisation module (E4-T06, docs/dev/localization.md).
// Everything here is integer arithmetic only (determinism rule, no float/double).

namespace pat::loc {

/// The most plural forms any supported language may declare (CLDR has at most six categories).
inline constexpr std::size_t kMaxPluralForms = 6;

/// How a language picks one of its plural forms for a count. `select` receives the magnitude of the
/// count (the sign is dropped by plural_form()) and returns a form index below `form_count`.
struct PluralRule {
    std::uint8_t form_count = 0;
    /// Form used for a non-integer count ("1,5 года"). Arguments are integers today, so this is
    /// reserved for decimal arguments; both built-in rules use form 1.
    std::uint8_t fraction_form = 0;
    std::uint8_t (*select)(std::uint64_t magnitude) noexcept = nullptr;
};

/// Russian: 3 forms (год | года | лет). Form 0 when n % 10 == 1 and n % 100 != 11; form 1 when n % 10
/// is 2..4 and n % 100 is not 12..14; form 2 otherwise. Non-integers use form 1 (CLDR "other" of ru
/// is the genitive singular, the second form).
[[nodiscard]] constexpr std::uint8_t plural_select_ru(std::uint64_t n) noexcept {
    const std::uint64_t mod10 = n % 10;
    const std::uint64_t mod100 = n % 100;
    if (mod10 == 1 && mod100 != 11)
        return 0;
    if (mod10 >= 2 && mod10 <= 4 && !(mod100 >= 12 && mod100 <= 14))
        return 1;
    return 2;
}

/// English: 2 forms (one | other). Form 0 only for n == 1.
[[nodiscard]] constexpr std::uint8_t plural_select_en(std::uint64_t n) noexcept {
    return n == 1 ? 0 : 1;
}

inline constexpr PluralRule kPluralRu{3, 1, &plural_select_ru};
inline constexpr PluralRule kPluralEn{2, 1, &plural_select_en};

/// Index of the plural form for `n` (the sign is ignored: -1 is "1"). Always below rule.form_count
/// for a valid rule.
[[nodiscard]] constexpr std::uint8_t plural_form(const PluralRule& rule, std::int64_t n) noexcept {
    // Negation in unsigned arithmetic is well defined for INT64_MIN as well.
    const std::uint64_t magnitude = n < 0 ? 0 - static_cast<std::uint64_t>(n) : static_cast<std::uint64_t>(n);
    return rule.select(magnitude);
}

/// A language the localisation module understands: its code (the directory name under `loc/`) and
/// its plural rule. More languages are added by passing extra entries in LoadOptions::languages.
struct LanguageInfo {
    std::string code;
    PluralRule rule;
};

/// The built-in languages: "en" and "ru" (sorted by code).
[[nodiscard]] std::vector<LanguageInfo> builtin_languages();

/// A language code is `[a-z][a-z0-9_]*`, at most 16 characters ("ru", "en", "pt_br").
[[nodiscard]] bool is_valid_language_code(std::string_view code) noexcept;

/// True when the rule can be used: 1..kMaxPluralForms forms, a selector, a valid fraction form.
[[nodiscard]] bool is_valid_plural_rule(const PluralRule& rule) noexcept;

}  // namespace pat::loc
