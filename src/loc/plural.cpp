#include "pat/loc/plural.hpp"

namespace pat::loc {

std::vector<LanguageInfo> builtin_languages() {
    std::vector<LanguageInfo> languages;
    languages.push_back({"en", kPluralEn});
    languages.push_back({"ru", kPluralRu});
    return languages;
}

bool is_valid_language_code(std::string_view code) noexcept {
    if (code.empty() || code.size() > 16)
        return false;
    if (code[0] < 'a' || code[0] > 'z')
        return false;
    for (const char c : code) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        if (!ok)
            return false;
    }
    return true;
}

bool is_valid_plural_rule(const PluralRule& rule) noexcept {
    return rule.select != nullptr && rule.form_count >= 1 && rule.form_count <= kMaxPluralForms &&
           rule.fraction_form < rule.form_count;
}

}  // namespace pat::loc
